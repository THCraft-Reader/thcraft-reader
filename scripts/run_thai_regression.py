#!/usr/bin/env python3
"""Run the production Thai probe offline and retain pixels, traces and comparisons.

The default is the complete Cartesian matrix, not a sampling scheme. --case,
--font-id and --list are diagnostic selectors; selected runs are labelled partial.
A baseline records existing defects. A candidate enforces protected source spans,
control-page pixel/layout identity and non-worsening glyph-sheet reference placement.
Desktop reference differences are evidence for human review, never an automatic
claim that native Thai typography is legible. Host timings have no pass threshold.
"""
from __future__ import annotations

import argparse
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
import itertools
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import unicodedata
import xml.etree.ElementTree as ET

from prepare_thai_test_assets import ROOT, ReferenceRenderer, dependencies, digest, save_json


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def check_assets(assets: Path, manifest: dict) -> None:
    for entry in [manifest["fallback"], *manifest["fonts"], *manifest["controls"]]:
        pairs = [("path", "sha256"), ("cpfont", "cpfont_sha256"),
                 ("regular", "regular_sha256"), ("source", "source_sha256"),
                 ("fallback", "fallback_sha256"), ("license", "license_sha256"), ("shape", "shape_sha256")]
        for field, hash_field in pairs:
            if field in entry and hash_field in entry:
                path = assets / entry[field]
                if not path.is_file() or digest(path) != entry[hash_field]:
                    raise RuntimeError(f"Missing/changed prepared asset: {path}; rerun explicit preparation")


def asset_contract(manifest):
    # Shaping may add alternate glyphs/metrics, but never change the pinned source faces.
    font_fields = ("id", "source_sha256", "regular_sha256", "frozen_axes", "point_size")
    control_fields = ("id", "cpfont_sha256", "regular_sha256", "fallback_sha256", "point_size")
    return {"dpi": manifest["dpi"], "intervals": manifest["intervals"],
            "fallback": manifest["fallback"]["sha256"],
            "fonts": [{key: font[key] for key in font_fields}
                      for font in sorted(manifest["fonts"], key=lambda font: font["id"])],
            "controls": [{key: font[key] for key in control_fields}
                         for font in sorted(manifest["controls"], key=lambda font: font["id"])]}


def settings_matrix(viewports):
    for (width, height), orientation, tracking, spacing, compression, hyphenation, focus, alignment in itertools.product(
            viewports, ("portrait", "inverted", "cw", "ccw"), (0, 2), (100, 125),
            (1.0, 0.95), ("off", "on"), ("off", "on"), ("left", "justify")):
        yield {"width": width, "height": height, "orientation": orientation,
               "character_spacing": tracking, "word_spacing_percent": spacing,
               "line_compression": compression, "hyphenation": hyphenation,
               "focus": focus, "alignment": alignment}


def run_key(font: dict, case: dict, settings: dict, mode: str) -> str:
    return (f"{font['id']}/{case['id']}/{mode}/"
            f"{settings['width']}x{settings['height']}-{settings['orientation']}"
            f"-c{settings['character_spacing']}-w{settings['word_spacing_percent']}"
            f"-l{settings['line_compression']:g}-h{settings['hyphenation']}"
            f"-f{settings['focus']}-{settings['alignment']}")


def jobs(manifest, corpus, viewports, selected_cases, selected_fonts):
    for font in manifest["fonts"] + manifest["controls"]:
        if selected_fonts and font["id"] not in selected_fonts:
            continue
        for case in corpus["cases"]:
            if selected_cases and case["id"] not in selected_cases:
                continue
            if (case["category"] == "control") != ("case_id" in font):
                continue
            if "case_id" in font and font["case_id"] != case["id"]:
                continue
            modes = ("normal", "rotated90cw") if case["category"] == "glyph-sheet" else ("paragraph",)
            for settings in settings_matrix(viewports):
                for mode in modes:
                    yield font, case, settings, mode


def aligned_xhtml(source: Path, destination: Path, alignment: str) -> Path:
    if destination.exists():
        return destination
    ET.register_namespace("", "http://www.w3.org/1999/xhtml")
    ET.register_namespace("epub", "http://www.idpf.org/2007/ops")
    tree = ET.parse(source)
    for element in tree.iter():
        if element.tag.rsplit("}", 1)[-1] in ("body", "p", "div", "td", "th"):
            element.set("style", element.get("style", "") + f";text-align:{alignment}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    for element in tree.iter():
        if element.tag.rsplit("}", 1)[-1] == "link" and element.get("rel") == "stylesheet":
            href = element.get("href", "")
            if "://" in href or Path(href).is_absolute() or ".." in Path(href).parts:
                raise RuntimeError(f"Nonlocal stylesheet in offline fixture: {href}")
            stylesheet = source.parent / href
            target = destination.parent / href
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(stylesheet, target)
    tree.write(destination, encoding="utf-8", xml_declaration=True)
    return destination


def logical_to_physical(image, orientation):
    from PIL import Image
    if orientation == "portrait":
        return image.transpose(Image.Transpose.ROTATE_90)
    if orientation == "inverted":
        return image.transpose(Image.Transpose.ROTATE_270)
    if orientation == "cw":
        return image.transpose(Image.Transpose.ROTATE_180)
    return image


def all_tokens(report):
    for page in report["pages"]:
        for line in page["lines"]:
            yield from line["tokens"]


def content_signature(report) -> str:
    # NFC and invisible layout controls may change representation without losing text.
    text = "".join(token["text"] for token in all_tokens(report))
    text = unicodedata.normalize("NFC", text)
    return "".join(ch for ch in text if not ch.isspace() and ch not in "-\u00ad\u200b\u200e\u200f")


def semantic_findings(report: dict, case: dict, mode: str) -> list[dict]:
    findings = []
    if mode != "paragraph":
        return findings
    offsets = [0]
    for ch in case["input"]:
        offsets.append(offsets[-1] + len(ch.encode("utf-8")))
    previous = -1
    spans = case.get("protected_spans", [])
    for page_index, page in enumerate(report["pages"]):
        for line_index, line in enumerate(page["lines"]):
            offset = line["source_offset"]
            if offset < previous:
                findings.append({"kind": "nonmonotonic_source_offset", "page": page_index,
                                 "line": line_index, "offset": offset, "previous": previous})
            previous = offset
            if 0 <= offset < len(offsets):
                byte_offset = offsets[offset]
                for begin, end in spans:
                    if begin < byte_offset < end:
                        findings.append({"kind": "protected_cluster_split", "page": page_index,
                                         "line": line_index, "byte_offset": byte_offset,
                                         "protected_span": [begin, end]})
            else:
                findings.append({"kind": "source_offset_out_of_range", "offset": offset})
            for token in line["tokens"]:
                if abs(token["advance"] - token["render_advance"]) > 1:
                    findings.append({"kind": "measurement_draw_advance_mismatch", "text": token["text"],
                                     "layout": token["advance"], "render": token["render_advance"]})
    return findings


def placement_signature(report: dict):
    line_fields = ("text", "x", "y", "baseline", "baseline_axis", "source_offset", "ink_bounds")
    token_fields = ("text", "x", "y", "baseline", "style", "source_offset", "focus_boundary",
                    "focus_suffix_x", "advance", "render_advance", "ink_bounds")
    return [{"source_offset": page["source_offset"], "width": page["width"], "height": page["height"],
             "lines": [{**{key: line[key] for key in line_fields if key in line},
                        "tokens": [{key: token[key] for key in token_fields if key in token}
                                   for token in line["tokens"]]} for line in page["lines"]]}
            for page in report["pages"]]


def pixel_difference(first, second) -> dict:
    from PIL import ImageChops
    if first.size != second.size:
        return {"same_dimensions": False, "changed_pixels": None, "ink_mismatch": None}
    delta = ImageChops.difference(first.convert("L"), second.convert("L"))
    changed = sum(delta.histogram()[1:])
    mask_a = first.convert("L").point(lambda value: 255 if value < 255 else 0)
    mask_b = second.convert("L").point(lambda value: 255 if value < 255 else 0)
    mismatch = ImageChops.difference(mask_a, mask_b).histogram()[255]
    return {"same_dimensions": True, "changed_pixels": changed, "ink_mismatch": mismatch}


def reference_page(renderer, report, page, mode, cluster_map=None):
    from PIL import Image, ImageDraw
    settings = report["settings"]
    image = Image.new("L", (settings["width"], settings["height"]), 255)
    annotation = Image.new("RGB", image.size, "white")
    rows = []
    tracking = settings["character_spacing"] if mode != "rotated90cw" else 0
    for line in page["lines"]:
        # Use the production parser's token placement; no reference word wrapper exists.
        items = [line] if mode != "paragraph" else line["tokens"]
        for token in items:
            rows.append(renderer.draw(image, token["text"], token["x"], token["y"],
                tracking=tracking, word_spacing=settings["word_spacing_percent"] if mode == "paragraph" else 100,
                rotated=mode == "rotated90cw", ascender=settings["font_ascender"],
                clusters=token.get("clusters", (cluster_map or {}).get(token["text"])),
                scale_half=bool(token.get("style", 0) & (16 | 32))))
    annotation.paste(image)
    draw = ImageDraw.Draw(annotation)
    for row in rows:
        if mode == "rotated90cw":
            baseline = row["x"] + settings["font_ascender"]
            draw.line((baseline, 0, baseline, image.height), fill=(200, 120, 120))
        else:
            draw.line((0, row["baseline"], image.width, row["baseline"]), fill=(200, 120, 120))
        if row["ink_bounds"]:
            draw.rectangle(row["ink_bounds"], outline=(30, 130, 220))
    return (logical_to_physical(image, settings["orientation"]),
            logical_to_physical(annotation, settings["orientation"]), rows)


def annotate_native(image, report, page, mode):
    from PIL import Image, ImageDraw
    settings = report["settings"]
    overlay = Image.new("RGBA", (settings["width"], settings["height"]), (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    for line in page["lines"]:
        if mode == "rotated90cw":
            baseline = line["x"] + settings["font_ascender"]
            draw.line((baseline, 0, baseline, overlay.height), fill=(200, 120, 120, 255))
        else:
            draw.line((0, line["baseline"], overlay.width, line["baseline"]), fill=(200, 120, 120, 255))
        for token in line["tokens"]:
            x, y, width, height = token["ink_bounds"]
            if width and height:
                draw.rectangle((x, y, x + width - 1, y + height - 1), outline=(30, 130, 220, 255))
    result = image.convert("RGBA")
    result.alpha_composite(logical_to_physical(overlay, settings["orientation"]))
    return result.convert("RGB")


def glyph_band(image, settings, line, mode):
    from PIL import Image
    inverse = {"portrait": Image.Transpose.ROTATE_270, "inverted": Image.Transpose.ROTATE_90,
               "cw": Image.Transpose.ROTATE_180}
    if settings["orientation"] in inverse:
        image = image.transpose(inverse[settings["orientation"]])
    step = 2 * settings["line_height"]
    if mode == "rotated90cw":
        return image.crop((line["x"], 0, min(image.width, line["x"] + step), image.height))
    return image.crop((0, line["y"], image.width, min(image.height, line["y"] + step)))


def images_and_gates(directory, baseline_dir, report, renderer, mode, control):
    from PIL import Image, ImageDraw
    findings, metrics = [], []
    baseline_report = load_json(baseline_dir / "report.json") if baseline_dir else None
    cluster_map = {line["text"]: line["clusters"] for page in report["pages"] for line in page["lines"]
                   if "clusters" in line}
    old_rows = {(line["source_offset"], line["text"]): (index, line)
                for index, page in enumerate((baseline_report or {}).get("pages", []))
                for line in page["lines"]}
    old_glyph_images = {}
    if baseline_report:
        if content_signature(report) != content_signature(baseline_report):
            findings.append({"kind": "content_changed_from_baseline"})
        if control and placement_signature(report) != placement_signature(baseline_report):
            findings.append({"kind": "control_layout_changed"})
        if control and len(report["pages"]) != len(baseline_report["pages"]):
            findings.append({"kind": "control_page_count_changed"})
    for index, page in enumerate(report["pages"]):
        path = directory / page.get("gray_image", page["image"])
        with path.open("rb") as stream:
            with Image.open(stream) as source:
                native = source.convert("L")
        if native.size != (page["width"], page["height"]):
            raise RuntimeError(f"Probe image dimensions disagree with trace: {path}")
        native.save(directory / f"page-{index:03d}.png")
        result = {"page": index, "native_ink_bbox": native.point(lambda v: 255 - v).getbbox()}
        if renderer is not None:
            reference, annotated, rows = reference_page(renderer, report, page, mode)
            if native.size != reference.size:
                raise RuntimeError(f"Probe logical/physical orientation contract mismatch: {directory}")
            reference.save(directory / f"reference-{index:03d}.png")
            save_json(directory / f"reference-{index:03d}.json", {"rows": rows})
            result["reference_difference"] = pixel_difference(native, reference)
            comparison = Image.new("RGB", (native.width * 2, native.height + 24), "white")
            comparison.paste(annotate_native(native, report, page, mode), (0, 24))
            comparison.paste(annotated, (native.width, 24))
            draw = ImageDraw.Draw(comparison)
            draw.text((4, 4), "Native CPFont / production renderer", fill="black")
            draw.text((native.width + 4, 4), "HarfBuzz + FreeType / same baseline", fill="black")
            comparison.resize((comparison.width * 2, comparison.height * 2),
                              Image.Resampling.NEAREST).save(directory / f"comparison-{index:03d}-2x.png")
        if baseline_report and index < len(baseline_report["pages"]):
            old_page = baseline_report["pages"][index]
            with Image.open(baseline_dir / old_page.get("gray_image", old_page["image"])) as old_image:
                old = old_image.convert("L")
            difference = pixel_difference(native, old)
            result["baseline_difference"] = difference
            if control and (not difference["same_dimensions"] or difference["changed_pixels"]):
                findings.append({"kind": "control_pixels_changed", "page": index, **difference})
        if baseline_report and renderer is not None and mode != "paragraph":
            # Font metrics may legitimately repaginate glyph sheets. Compare the same
            # source row, not unrelated whole pages at the same page index.
            row_metrics = []
            for line in page["lines"]:
                key = (line["source_offset"], line["text"])
                if key not in old_rows:
                    findings.append({"kind": "missing_baseline_glyph_row", "text": line["text"]})
                    continue
                old_index, old_line = old_rows[key]
                if old_index not in old_glyph_images:
                    old_page = baseline_report["pages"][old_index]
                    with Image.open(baseline_dir / old_page.get("gray_image", old_page["image"])) as source:
                        old_native = source.convert("L")
                    old_reference, _, _ = reference_page(renderer, baseline_report, old_page, mode, cluster_map)
                    old_glyph_images[old_index] = old_native, old_reference
                old_native, old_reference = old_glyph_images[old_index]
                old_error = pixel_difference(glyph_band(old_native, baseline_report["settings"], old_line, mode),
                                             glyph_band(old_reference, baseline_report["settings"], old_line, mode))
                new_error = pixel_difference(glyph_band(native, report["settings"], line, mode),
                                             glyph_band(reference, report["settings"], line, mode))
                row_metrics.append({"text": line["text"], "baseline": old_error, "candidate": new_error})
                if (not new_error["same_dimensions"] or
                        new_error["ink_mismatch"] > old_error["ink_mismatch"]):
                    findings.append({"kind": "glyph_sheet_placement_worsened", "text": line["text"],
                                     "baseline": old_error, "candidate": new_error})
            result["glyph_rows"] = row_metrics
        metrics.append(result)
        # PNG preserves all four ink levels without retaining a raw framebuffer per run.
        raw_paths = {directory / page["image"], directory / page.get("gray_image", page["image"])}
        page["image"] = page["gray_image"] = f"page-{index:03d}.png"
        for raw_path in raw_paths:
            if raw_path.suffix in (".pbm", ".pgm"):
                raw_path.unlink()
    save_json(directory / "report.json", report)
    return findings, metrics


_worker = {}


def initialize_worker(assets, output, probe, corpus_dir, baseline, candidate, resume, fallback):
    _worker.update(assets=assets, output=output, probe=probe, corpus_dir=corpus_dir,
                   baseline=baseline, candidate=candidate, resume=resume,
                   fallback=fallback, renderers={})


def execute_job(job):
    font, case, settings, mode = job
    assets, output = _worker["assets"], _worker["output"]
    candidate = _worker["candidate"]
    key = run_key(font, case, settings, mode)
    destination = output / key
    destination.mkdir(parents=True, exist_ok=True)
    old_dir = _worker["baseline"] / key if candidate else None
    if old_dir and not (old_dir / "report.json").is_file():
        raise RuntimeError(f"Candidate has no corresponding baseline trace: {old_dir}")
    record_path = destination / "comparison.json"
    if _worker["resume"] and record_path.is_file():
        return load_json(record_path)
    xhtml = output / "inputs" / settings["alignment"] / Path(case["xhtml"]).name
    command = [str(_worker["probe"]), "--font", str(assets / font["cpfont"]),
               "--xhtml", str(xhtml), "--output", str(destination)]
    for option, value in settings.items():
        if option != "alignment":
            command.extend(["--" + option.replace("_", "-"), str(value)])
    if mode != "paragraph":
        command.extend(["--glyph-sheet", mode])
    completed = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                               errors="replace", check=False)
    (destination / "probe.stdout.log").write_text(completed.stdout, encoding="utf-8")
    (destination / "probe.stderr.log").write_text(completed.stderr, encoding="utf-8")
    if completed.returncode:
        raise RuntimeError(f"Probe exited {completed.returncode}: {key}; see probe.stderr.log")
    report = load_json(destination / "report.json")
    if report.get("schema_version") != 1 or not report.get("pages"):
        raise RuntimeError(f"Invalid/empty production probe trace: {destination}")
    for field in ("width", "height", "orientation", "word_spacing_percent", "line_compression",
                  "hyphenation", "focus"):
        if report["settings"].get(field) != settings[field]:
            raise RuntimeError(f"Probe effective setting {field} disagrees with request: {key}")
    effective_tracking = 0 if mode == "rotated90cw" else settings["character_spacing"]
    if report["settings"]["character_spacing"] != effective_tracking:
        raise RuntimeError(f"Probe effective character spacing disagrees with request: {key}")
    control = case["category"] == "control"
    renderer = None
    if not control:
        renderers = _worker["renderers"]
        if font["id"] not in renderers:
            renderers[font["id"]] = ReferenceRenderer(assets / font["regular"],
                                                     assets / _worker["fallback"], font["point_size"])
        renderer = renderers[font["id"]]
    findings = semantic_findings(report, case, mode)
    image_findings, metrics = images_and_gates(destination, old_dir, report, renderer, mode, control)
    findings.extend(image_findings)
    record = {"key": key, "font_id": font["id"], "case_id": case["id"],
              "mode": mode, "settings": settings, "findings": findings, "pages": metrics,
              "host_measurements": report["measurements"], "visual_review_required": not control,
              "passed": not findings if candidate else None}
    save_json(record_path, record)
    return record


def run(args) -> int:
    assets, output, probe = args.assets.resolve(), args.output.resolve(), args.probe.resolve()
    if not probe.is_file():
        raise RuntimeError(f"Probe executable not found: {probe}")
    manifest = load_json(assets / "manifest.json")
    corpus_dir = args.corpus.resolve()
    corpus = load_json(corpus_dir / "manifest.json")
    check_assets(assets, manifest)
    versions = dependencies()
    viewports = [(480, 800), (800, 480)]
    for value in args.device_viewport:
        width, height = map(int, value.lower().split("x"))
        if width <= 0 or height <= 0:
            raise ValueError("Device viewport must be positive WIDTHxHEIGHT")
        if (width, height) not in viewports:
            viewports.append((width, height))
    selected_cases, selected_fonts = set(args.case), set(args.font_id)
    unknown_cases = selected_cases - {case["id"] for case in corpus["cases"]}
    unknown_fonts = selected_fonts - {font["id"] for font in manifest["fonts"] + manifest["controls"]}
    if unknown_cases or unknown_fonts:
        raise RuntimeError(f"Unknown selection: cases={sorted(unknown_cases)}, fonts={sorted(unknown_fonts)}")
    baseline = output.parent / "baseline"
    candidate = output != baseline and baseline.is_dir()
    if args.list:
        count = 0
        for font, case, settings, mode in jobs(manifest, corpus, viewports, selected_cases, selected_fonts):
            print(run_key(font, case, settings, mode))
            count += 1
        print(f"{count} probe invocations", file=sys.stderr)
        return 0
    output.mkdir(parents=True, exist_ok=True)
    identity = {"assets_sha256": digest(assets / "manifest.json"),
                "corpus_manifest_sha256": digest(corpus_dir / "manifest.json"),
                "probe_sha256": digest(probe), "runner_sha256": digest(Path(__file__)),
                "reference_renderer_sha256": digest(ROOT / "scripts/prepare_thai_test_assets.py")}
    previous_manifest = output / "run-manifest.json"
    if previous_manifest.exists():
        previous = load_json(previous_manifest)
        if previous["identity"] != identity:
            raise RuntimeError(f"Output belongs to different inputs/tools; choose a new output directory: {output}")
        if not args.resume:
            raise RuntimeError(f"Output already has a run manifest; use --resume explicitly: {output}")
    run_manifest = {"schema_version": 1, "identity": identity, "versions": versions,
                    "matrix": {"viewports": viewports, "orientations": ["portrait", "inverted", "cw", "ccw"],
                               "character_spacing": [0, 2], "word_spacing_percent": [100, 125],
                               "line_compression": [1.0, 0.95], "hyphenation": ["off", "on"],
                               "focus": ["off", "on"], "alignment": ["left", "justify"],
                               "glyph_sheet": ["normal", "rotated90cw"]},
                    "partial_selection": bool(selected_cases or selected_fonts),
                    "selected_cases": sorted(selected_cases), "selected_fonts": sorted(selected_fonts),
                    "mode": "candidate" if candidate else "baseline",
                    "device_viewport_verified": bool(args.device_viewport),
                    "device_performance": "unverified; host timings are not hardware acceptance"}
    if candidate:
        old_manifest = load_json(baseline / "run-manifest.json")
        if old_manifest["identity"]["corpus_manifest_sha256"] != identity["corpus_manifest_sha256"]:
            raise RuntimeError("Baseline/candidate corpus identity differs")
        pinned_assets = baseline / "assets-manifest.json"
        if pinned_assets.is_file():
            if digest(pinned_assets) != old_manifest["identity"]["assets_sha256"]:
                raise RuntimeError("Retained baseline assets manifest failed its identity check")
            if asset_contract(load_json(pinned_assets)) != asset_contract(manifest):
                raise RuntimeError("Baseline/candidate source-font contract differs")
        elif old_manifest["identity"]["assets_sha256"] != identity["assets_sha256"]:
            raise RuntimeError("Changed assets require the retained, hash-verified baseline assets-manifest.json")
    save_json(previous_manifest, run_manifest)
    shutil.copyfile(assets / "manifest.json", output / "assets-manifest.json")
    if args.jobs < 1:
        raise ValueError("--jobs must be positive")
    # Prepare shared input files before workers start; no concurrent partial XML reads.
    for case in corpus["cases"]:
        for alignment in ("left", "justify"):
            original = corpus_dir / case["xhtml"]
            aligned_xhtml(original, output / "inputs" / alignment / original.name, alignment)
    counts = Counter()
    aggregate_findings = Counter()
    samples = {}
    pending = iter(jobs(manifest, corpus, viewports, selected_cases, selected_fonts))
    with ProcessPoolExecutor(max_workers=args.jobs, initializer=initialize_worker,
                             initargs=(assets, output, probe, corpus_dir, baseline, candidate,
                                       args.resume, manifest["fallback"]["path"])) as workers, \
            (output / "results.jsonl").open("w", encoding="utf-8") as log:
        # Bound both submitted work and retained results even for the full corpus matrix.
        while batch := list(itertools.islice(pending, args.jobs * 4)):
            for record in workers.map(execute_job, batch):
                log.write(json.dumps(record, ensure_ascii=False) + "\n")
                counts["runs"] += 1
                counts["pages"] += len(record["pages"])
                counts["runs_with_findings"] += bool(record["findings"])
                for finding in record["findings"]:
                    aggregate_findings[finding["kind"]] += 1
                for field, value in record["host_measurements"].items():
                    if isinstance(value, (float, int)):
                        samples.setdefault(field, []).append(value)
            log.flush()
    if not counts["runs"]:
        raise RuntimeError("No matching regression jobs")
    timings = {}
    for field, values in samples.items():
        ordered = sorted(values)
        timings[field] = {"samples": len(values), "median": statistics.median(values),
                          "p95": ordered[max(0, (95 * len(ordered) + 99) // 100 - 1)],
                          "host_only": True, "acceptance_threshold": None}
    summary = {"schema_version": 1, **run_manifest, "counts": dict(counts),
               "findings": dict(aggregate_findings), "host_measurements": timings,
               "automated_gate": ("fail" if counts["runs_with_findings"] else "pass") if candidate else "baseline capture",
               "native_visual_gate": "pending human inspection of enlarged comparisons and metric traces",
               "complete_milestone": False}
    save_json(output / "summary.json", summary)
    print(json.dumps({"output": str(output), "counts": dict(counts),
                      "automated_gate": summary["automated_gate"]}, ensure_ascii=False))
    return 1 if candidate and counts["runs_with_findings"] else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, default=ROOT / "build/thai/corpus")
    parser.add_argument("--device-viewport", action="append", default=[], metavar="WIDTHxHEIGHT",
                        help="Actual viewport reported by the X4 Pro renderer; never assume device dimensions")
    parser.add_argument("--case", action="append", default=[], help="Diagnostic case ID; labels run partial")
    parser.add_argument("--font-id", action="append", default=[], help="Diagnostic font ID; labels run partial")
    parser.add_argument("--list", action="store_true", help="Print full selected job matrix without executing probes")
    parser.add_argument("--resume", action="store_true", help="Reuse completed runs only with identical inputs/tools")
    parser.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 1),
                        help="Bounded independent host workers; host timings are not device acceptance")
    args = parser.parse_args()
    try:
        return run(args)
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        parser.exit(1, f"Thai regression failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
