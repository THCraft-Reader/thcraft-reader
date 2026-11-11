#!/usr/bin/env python3
"""Generate the offline Thai corpus EPUB and production-probe XHTML inputs.

The checked-in corpus owns all text and semantic expectations. Invalid UTF-8
fixtures are analyzer-only and never enter XML. Run from any working directory.
"""

import argparse
import hashlib
import json
import re
import xml.etree.ElementTree as ET
from html import escape
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
FIXED_TIME = (2026, 1, 1, 0, 0, 0)
XHTML_NS = "http://www.w3.org/1999/xhtml"
STYLE = "body { margin: 0; padding: 0; } p { margin: 0; padding: 0; text-indent: 0; }\n"


def xhtml_page(title: str, body: str) -> str:
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE html>\n'
        f'<html xmlns="{XHTML_NS}" xmlns:epub="http://www.idpf.org/2007/ops" '
        'lang="th" xml:lang="th">\n'
        '<head><meta charset="UTF-8"/>'
        f'<title>{escape(title)}</title><link rel="stylesheet" type="text/css" href="style.css"/>'
        f'</head>\n<body>{body}</body>\n</html>\n'
    )


def byte_boundaries(text: str) -> set[int]:
    result = {0}
    offset = 0
    for char in text:
        offset += len(char.encode("utf-8"))
        result.add(offset)
    return result


def validate_case(case: dict) -> None:
    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", case["id"]):
        raise ValueError(f"Invalid stable ID: {case['id']!r}")
    boundaries = byte_boundaries(case["input"])
    for begin, end in case["protected_spans"]:
        if begin not in boundaries or end not in boundaries or begin >= end:
            raise ValueError(f"{case['id']}: invalid protected UTF-8 span {(begin, end)}")
    for offset in case["dictionary_boundaries"]:
        if offset not in boundaries or offset in (0, max(boundaries)):
            raise ValueError(f"{case['id']}: invalid dictionary boundary {offset}")
    if "input_hex" in case:
        if not case.get("analyzer_only"):
            raise ValueError(f"{case['id']}: raw bytes must be analyzer-only")
        bytes.fromhex(case["input_hex"])
    if "expected_utf8_bytes" in case and max(boundaries) != case["expected_utf8_bytes"]:
        raise ValueError(f"{case['id']}: wrong stress byte count")
    if "input_sha256" in case:
        actual = hashlib.sha256(case["input"].encode("utf-8")).hexdigest()
        if actual != case["input_sha256"]:
            raise ValueError(f"{case['id']}: attributed text checksum mismatch")


def add_file(archive: ZipFile, name: str, contents: str, compression: int) -> None:
    info = ZipInfo(name, FIXED_TIME)
    info.create_system = 3
    info.compress_type = compression
    info.external_attr = 0o100644 << 16
    archive.writestr(info, contents.encode("utf-8"))


def generate(corpus_path: Path, output: Path, xhtml_dir: Path) -> dict:
    corpus_bytes = corpus_path.read_bytes()
    corpus = json.loads(corpus_bytes)
    if corpus["schema_version"] != 1:
        raise ValueError("Unsupported Thai corpus schema")
    files = {}
    entries = []
    ids = set()
    for case in corpus["cases"]:
        validate_case(case)
        case_id = case["id"]
        if case_id in ids:
            raise ValueError(f"Duplicate corpus ID: {case_id}")
        ids.add(case_id)
        if case.get("analyzer_only"):
            continue
        filename = f"{case_id}.xhtml"
        body = case.get("xhtml_body", f'<p id="{case_id}">{escape(case["input"])}</p>')
        page = xhtml_page(case_id, body)
        ET.fromstring(page)  # Fail before writing an invalid-XML EPUB.
        files[filename] = page
        entries.append({**case, "xhtml": filename})

    glyph_cases = [case for case in corpus["cases"] if case["category"] in ("combining", "word")]
    files["glyph-cases.xhtml"] = xhtml_page(
        "Native CPFont glyph cases",
        "\n".join(f'<p id="{case["id"]}">{escape(case["input"])}</p>' for case in glyph_cases),
    )
    entries.append({
        "id": "glyph-cases", "category": "glyph-sheet", "xhtml": "glyph-cases.xhtml",
        "input": "\n".join(case["input"] for case in glyph_cases),
        "protected_spans": [], "dictionary_boundaries": [],
        "case_ids": [case["id"] for case in glyph_cases],
    })
    attribution = []
    for source in corpus["sources"]:
        attribution.append(
            f'<p>{escape(source["title"])} — {escape(source["author"])}; '
            f'{source["edition_year"]} edition; revision {source["revision"]}. '
            f'{escape(source["work_license"])}. {escape(source["transcription_attribution"])}. '
            f'<a href="{escape(source["url"], quote=True)}">Source revision</a>; '
            f'<a href="{escape(source["transcription_license_url"], quote=True)}">Transcription license</a>.</p>'
        )
    attribution.append(
        '<p>The prose-space-stripped chapter is explicitly a space-stripped stress derivative, '
        'not an original quotation. Source spaces are retained in prose-vetala-1 through prose-vetala-3. '
        'Synthetic boundary and malformed-sign cases are fixtures, not literary quotations.</p>'
    )
    files["attribution.xhtml"] = xhtml_page("Sources and stress derivatives", "\n".join(attribution))
    files["style.css"] = STYLE
    ordered_chapters = ["attribution.xhtml"] + [entry["xhtml"] for entry in entries]
    files["nav.xhtml"] = xhtml_page(
        "Contents",
        '<nav epub:type="toc"><h1>Thai reading corpus</h1><ol>'
        + "".join(f'<li><a href="{name}">{escape(Path(name).stem)}</a></li>' for name in ordered_chapters)
        + "</ol></nav>",
    )
    manifest_items = [
        '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
        '<item id="css" href="style.css" media-type="text/css"/>',
    ]
    manifest_items.extend(
        f'<item id="chapter-{index}" href="{name}" media-type="application/xhtml+xml"/>'
        for index, name in enumerate(ordered_chapters)
    )
    spine = "".join(f'<itemref idref="chapter-{index}"/>' for index in range(len(ordered_chapters)))
    opf = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="book-id">'
        '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
        '<dc:identifier id="book-id">thcraft-thai-reading-corpus-v1</dc:identifier>'
        '<dc:title>Thai Reading Engine Corpus</dc:title><dc:language>th</dc:language>'
        '<dc:creator>THCraft Reader contributors</dc:creator>'
        '<dc:rights>Includes public-domain น.ม.ส. prose; Wikisource transcription attribution and '
        'CC BY-SA 4.0 details in attribution.xhtml.</dc:rights>'
        '<meta property="dcterms:modified">2026-01-01T00:00:00Z</meta></metadata>'
        f'<manifest>{"".join(manifest_items)}</manifest><spine>{spine}</spine></package>\n'
    )
    container = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
        '<rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles></container>\n'
    )
    for name, text in files.items():
        if name.endswith(".xhtml"):
            ET.fromstring(text)
    ET.fromstring(opf)
    ET.fromstring(container)
    xhtml_dir.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        (xhtml_dir / name).write_bytes(text.encode("utf-8"))
    manifest = {
        "schema_version": 1,
        "corpus_sha256": hashlib.sha256(corpus_bytes).hexdigest(),
        "offset_unit": corpus["offset_unit"],
        "expectation_scope": corpus["expectation_scope"],
        "sources": corpus["sources"],
        "cases": entries,
        "analyzer_only_ids": [case["id"] for case in corpus["cases"] if case.get("analyzer_only")],
    }
    (xhtml_dir / "manifest.json").write_bytes((json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w") as archive:
        add_file(archive, "mimetype", "application/epub+zip", ZIP_STORED)
        add_file(archive, "META-INF/container.xml", container, ZIP_DEFLATED)
        add_file(archive, "OEBPS/content.opf", opf, ZIP_DEFLATED)
        for name, text in files.items():
            add_file(archive, "OEBPS/" + name, text, ZIP_DEFLATED)
    return {
        "epub": str(output), "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "xhtml_dir": str(xhtml_dir), "render_cases": len(entries),
        "analyzer_only_cases": len(manifest["analyzer_only_ids"]),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=ROOT / "test/language/Thai/corpus.json")
    parser.add_argument("--output", type=Path, default=ROOT / "test/epubs/test_thai_reading.epub")
    parser.add_argument("--xhtml-dir", type=Path, default=ROOT / "build/thai/corpus")
    args = parser.parse_args()
    print(json.dumps(generate(args.corpus, args.output, args.xhtml_dir), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
