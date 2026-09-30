#!/usr/bin/env python3
"""Build an offline, paired CPFont/CPSHAPE SD-card installer from verified Thai assets.

Requires the converter's desktop dependencies (freetype-py, fonttools, uharfbuzz).
No sources are downloaded, and no generated fonts enter the firmware assets.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

from prepare_thai_test_assets import CONVERTER, FALLBACK_SHA256, FONTS, ROOT

SIZES = (8, 10, 12, 14, 16, 18)
FAMILIES = {
    "noto-sans-thai": "THCraft-NotoSansThai",
    "noto-serif-thai": "THCraft-NotoSerifThai",
    "sarabun": "THCraft-Sarabun",
}
INTERVALS = "ascii,punctuation,thai"
ZIP_TIME = (1980, 1, 1, 0, 0, 0)
INSTALL = """THCraft Thai paired bitmap fonts

1. Extract the fonts folder from this archive to the SD card root. Merge it
   with an existing fonts folder; keep all your original fonts and books.
2. Keep each .cpfont and its same-basename .cpshape together. Install all six
   sizes (8, 10, 12, 14, 16, 18) for each family you use, including the UI sizes.
3. Reinsert the SD card and restart the reader. In Settings > Reader > Font
   Family select THCraft-NotoSansThai; 16 pt is recommended for reading.
   The selected family also supplies missing Thai glyphs at UI sizes 8/10/12.
4. Do not delete reading progress or book caches. If this family already exists
   under the hidden .fonts folder, update that copy too: .fonts takes priority
   over fonts for duplicate family names.

This pack uses the CPFont bitmap path with optional CPSHAPE Thai positioning.
It is not a direct TTF/OTF/TTC installation: copying the source TTF alone does
not install these paired bitmap recipes. Use firmware with Thai CPSHAPE support;
older firmware can read the CPFont but will not apply the companion positioning.
The fonts contain a regular face; existing reader style fallback still applies.
OFL.txt and NotoSans-OFL.txt retain source and Latin/punctuation fallback notices.
Build reports are desktop provenance, not required files on the SD card.
"""


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")


def verified_file(assets: Path, relative: str, expected: str) -> Path:
    path = (assets / relative).resolve()
    if Path(relative).is_absolute() or not path.is_relative_to(assets):
        raise ValueError(f"Asset path must stay inside --assets: {relative}")
    if not path.is_file():
        raise ValueError(f"Missing local asset: {path}; prepare assets before packaging (no downloads here)")
    if len(expected) != 64 or digest(path) != expected:
        raise ValueError(f"SHA-256 mismatch for {path}; expected {expected}")
    return path


def checked_license(path: Path) -> None:
    text = path.read_text(encoding="utf-8").upper()
    if "SIL OPEN FONT LICENSE" not in text or "1.1" not in text or "COPYRIGHT" not in text:
        raise ValueError(f"Missing OFL 1.1 or copyright notice: {path}")


def load_sources(assets: Path) -> tuple[dict, list[dict], Path, Path]:
    manifest = json.loads((assets / "manifest.json").read_text(encoding="utf-8"))
    if manifest["schema_version"] != 1 or manifest["dpi"] != 150:
        raise ValueError("Expected the Thai asset manifest schema 1 at 150 DPI")
    fallback_record = manifest["fallback"]
    if fallback_record["sha256"] != FALLBACK_SHA256:
        raise ValueError("Manifest fallback does not match the pinned Noto Sans source")
    fallback = verified_file(assets, fallback_record["path"], FALLBACK_SHA256)
    fallback_license = verified_file(assets, fallback_record["license"], fallback_record["license_sha256"])
    checked_license(fallback_license)
    sources = []
    fields = ("source", "source_sha256", "source_git_blob", "regular", "regular_sha256",
              "frozen_axes", "license", "license_sha256")
    for key, _, _, expected, blob in FONTS:
        records = [entry for entry in manifest["fonts"] if entry["family"] == key]
        if not records:
            raise ValueError(f"Manifest has no source for {key}")
        record = {field: records[0][field] for field in fields}
        if any(any(entry[field] != record[field] for field in fields) for entry in records):
            raise ValueError(f"Conflicting source records for {key}")
        if record["source_sha256"] != expected or record["source_git_blob"] != blob:
            raise ValueError(f"Manifest source does not match the pinned {key} font")
        expected_axes = {} if key == "sarabun" else {"wght": 400, "wdth": 100}
        if record["frozen_axes"] != expected_axes:
            raise ValueError(f"Unexpected frozen regular axes for {key}: {record['frozen_axes']}")
        for path_field, hash_field in (("source", "source_sha256"), ("regular", "regular_sha256"),
                                       ("license", "license_sha256")):
            verified_file(assets, record[path_field], record[hash_field])
        checked_license(assets / record["license"])
        sources.append({"family": FAMILIES[key], **record})
    return manifest, sources, fallback, fallback_license


def validate_pair(cpfont: Path, source: dict, size: int) -> tuple[dict, int]:
    companion = cpfont.with_suffix(".cpshape")
    report_path = companion.with_suffix(".cpshape.json")
    for path in (cpfont, companion, report_path):
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f"Converter produced an incomplete font pair: missing or empty {path}")
    font = cpfont.read_bytes()
    shape = companion.read_bytes()
    if len(font) < 64 or font[:8] != b"CPFONT\0\0" or struct.unpack_from("<H", font, 8)[0] != 4:
        raise ValueError(f"Expected CPFont v4: {cpfont}")
    if len(shape) < 44:
        raise ValueError(f"Truncated companion: {companion}")
    magic, version, header, font_bytes, metrics_crc, payload_crc, total, styles = struct.unpack_from(
        "<8sHHIIIII", shape)
    if (magic != b"CPSHAPE\0" or version != 1 or header != 32 or font_bytes != len(font)
            or total != len(shape) or total > 384 * 1024 or styles != 1 or font[12] != styles
            or zlib.crc32(shape[header:]) != payload_crc):
        raise ValueError(f"Invalid or mismatched companion: {companion}")
    style, offset, length = struct.unpack_from("<B3xII", shape, 32)
    if style != 0 or offset != 44 or not 28 <= length <= 96 * 1024 or offset + length != total:
        raise ValueError(f"Invalid regular companion payload: {companion}")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if (report["dpi"] != 150 or report["point_size"] != size or report["format_version"] != version
            or report["companion_bytes"] != total or report["companion_sha256"] != digest(companion)
            or report["metrics_crc32"] != metrics_crc or report["payload_crc32"] != payload_crc
            or set(report["styles"]) != {"0"}
            or report["styles"]["0"]["source_sha256"] != source["regular_sha256"]
            or report["styles"]["0"]["fallback_sha256"] != FALLBACK_SHA256):
        raise ValueError(f"Converter provenance does not match the requested font pair: {cpfont}")
    # Keep desktop reports reproducible across checkout and staging directory paths.
    report["styles"]["0"]["source"] = source["regular"]
    return report, styles


def file_record(path: Path, root: Path) -> dict:
    return {"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size, "sha256": digest(path)}


def make_zip(root: Path, target: Path) -> None:
    # Stored entries avoid compressor-version differences; every metadata field is fixed.
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_STORED) as archive:
        for path in sorted(p for p in root.rglob("*") if p.is_file()):
            entry = zipfile.ZipInfo(path.relative_to(root).as_posix(), ZIP_TIME)
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            entry.compress_type = zipfile.ZIP_STORED
            archive.writestr(entry, path.read_bytes())


def package(assets: Path, output: Path, archive: Path) -> None:
    if output == assets or output.is_relative_to(assets) or assets.is_relative_to(output):
        raise ValueError("--output and --assets must be separate, non-nested directories")
    if archive.is_relative_to(output) or archive.is_relative_to(assets):
        raise ValueError("--zip must be outside --output and --assets")
    manifest, sources, fallback, fallback_license = load_sources(assets)
    output.parent.mkdir(parents=True, exist_ok=True)
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="thai-font-pack-", dir=output.parent) as temporary:
        stage = Path(temporary) / "pack"
        stage.mkdir()
        pairs = []
        for source in sources:
            family = source["family"]
            family_dir = stage / "fonts" / family
            family_dir.mkdir(parents=True)
            shutil.copyfile(assets / source["license"], family_dir / "OFL.txt")
            shutil.copyfile(fallback_license, family_dir / "NotoSans-OFL.txt")
            for size in SIZES:
                cpfont = family_dir / f"{family}_{size}.cpfont"
                subprocess.run([sys.executable, str(CONVERTER), "--regular", str(assets / source["regular"]),
                                "--fallback-regular", str(fallback), "--intervals", INTERVALS,
                                "--size", str(size), "--thai-shaping", "--output", str(cpfont)],
                               cwd=ROOT, check=True)
                report, styles = validate_pair(cpfont, source, size)
                report_path = stage / "reports" / f"{family}_{size}.cpshape.json"
                save_json(report_path, report)
                cpfont.with_suffix(".cpshape.json").unlink()
                pairs.append({"family": family, "point_size": size, "companion_style_count": styles,
                              "cpfont": file_record(cpfont, stage),
                              "cpshape": file_record(cpfont.with_suffix(".cpshape"), stage),
                              "report": report_path.relative_to(stage).as_posix()})
        (stage / "INSTALL.txt").write_text(INSTALL, encoding="utf-8", newline="\n")
        save_json(stage / "build-report.json", {
            "schema_version": 1, "dpi": 150, "sizes": list(SIZES), "intervals": INTERVALS,
            "thai_shaping": True, "style": "regular", "source_manifest_sha256": digest(assets / "manifest.json"),
            "sources": sources, "fallback": manifest["fallback"],
            "converter_sha256": digest(CONVERTER),
            "shaping_converter_sha256": digest(CONVERTER.with_name("thai_shape.py")),
            "packager_sha256": digest(Path(__file__)), "pairs": pairs,
            "files": [file_record(path, stage) for path in sorted(stage.rglob("*")) if path.is_file()],
        })
        # Publish only after all 18 pairs and their licenses have been validated.
        files = [path for path in stage.rglob("*") if path.is_file()]
        expected = {path.relative_to(stage) for path in files}
        if output.exists():
            if not output.is_dir():
                raise ValueError(f"--output is not a directory: {output}")
            unexpected = {path.relative_to(output) for path in output.rglob("*") if path.is_file()} - expected
            if unexpected:
                raise ValueError(f"Refusing to overwrite output containing unrelated files: {sorted(map(str, unexpected))}")
        with tempfile.TemporaryDirectory(prefix="thai-font-zip-", dir=archive.parent) as zip_temporary:
            staged_zip = Path(zip_temporary) / archive.name
            make_zip(stage, staged_zip)
            for path in files:
                destination = output / path.relative_to(stage)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, destination)
            staged_zip.replace(archive)
    print(f"Packaged {len(pairs)} paired fonts: {archive}")
    print(f"SHA-256: {digest(archive)}")
    print(f"Build report: {output / 'build-report.json'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=ROOT / "build/thai/assets")
    parser.add_argument("--output", type=Path, default=ROOT / "build/thai/font-pack")
    parser.add_argument("--zip", type=Path, default=ROOT / "build/thai/THCraft-Thai-Fonts.zip")
    args = parser.parse_args()
    try:
        package(args.assets.resolve(), args.output.resolve(), args.zip.resolve())
    except (OSError, ValueError, KeyError, TypeError, struct.error, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Thai font packaging failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
