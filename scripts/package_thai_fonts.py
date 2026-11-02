#!/usr/bin/env python3
"""Build an offline CPFont SD-card installer from verified Thai assets.

Requires the converter's desktop dependencies (freetype-py and fonttools).
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

from prepare_thai_test_assets import (
    ARABIC_FALLBACK_SHA256, CONVERTER, FALLBACK_SHA256, FONTS, PACK_FALLBACK_FONTS, ROOT,
)

SIZES = (8, 10, 12, 14, 16, 18, 20, 22, 24, 26)
FAMILIES = {
    "noto-sans-thai": "THCraft-NotoSansThai",
    "noto-serif-thai": "THCraft-NotoSerifThai",
    "sarabun": "THCraft-Sarabun",
}
INTERVALS = "latin-ext,ipa-chars,greek,(0x0300-0x036F),punctuation,symbols,thai,arabic"
ZIP_TIME = (1980, 1, 1, 0, 0, 0)
INSTALL = """THCraft Thai bitmap fonts

1. Extract the fonts folder from this archive to the SD card root. Merge it
   with an existing fonts folder; keep all your original fonts and books.
2. Install the .cpfont files at all ten sizes
   (8, 10, 12, 14, 16, 18, 20, 22, 24, 26) for each family you use, including the UI sizes.
3. Reinsert the SD card and restart the reader. In Settings > Reader > Font
   Family select THCraft-NotoSansThai; 16 pt is recommended for reading.
   The selected family also supplies missing Thai glyphs at UI sizes 8/10/12.
4. Do not delete reading progress or book caches. If this family already exists
   under the hidden .fonts folder, update that copy too: .fonts takes priority
   over fonts for duplicate family names.

This pack uses native Unicode CPFont bitmap glyphs with Thai positioning in firmware.
It is not a direct TTF/OTF/TTC installation: copying the source TTF alone does
not install a bitmap font. Use firmware with native Thai shaping support.
The fonts contain a regular face; existing reader style fallback still applies.
OFL.txt and the named fallback OFL files retain all source copyright notices.
Coverage includes IPA, arrows, mathematical/decorative symbols, and Arabic
letters and contextual presentation forms, in addition to Thai and Latin.
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


def load_sources(assets: Path) -> tuple[list[dict], list[dict]]:
    manifest = json.loads((assets / "manifest.json").read_text(encoding="utf-8"))
    if manifest["schema_version"] != 1 or manifest["dpi"] != 150:
        raise ValueError("Expected the Thai asset manifest schema 1 at 150 DPI")
    fallback_record = manifest["fallback"]
    if fallback_record["sha256"] != FALLBACK_SHA256:
        raise ValueError("Manifest fallback does not match the pinned Noto Sans source")
    verified_file(assets, fallback_record["path"], FALLBACK_SHA256)
    fallback_license = verified_file(assets, fallback_record["license"], fallback_record["license_sha256"])
    checked_license(fallback_license)
    fallbacks = [{"name": "NotoSans", **fallback_record}]
    records = manifest.get("pack_fallbacks", [])
    expected_fallbacks = [(name, expected, blob) for name, _, _, expected, blob in PACK_FALLBACK_FONTS]
    expected_fallbacks.append(("NotoSansArabic", ARABIC_FALLBACK_SHA256, None))
    if [record["name"] for record in records] != [name for name, _, _ in expected_fallbacks]:
        raise ValueError("Missing or reordered pack fallbacks; rerun prepare_thai_test_assets.py")
    for record, (name, expected, blob) in zip(records, expected_fallbacks):
        if record["source_sha256"] != expected or record["source_git_blob"] != blob:
            raise ValueError(f"Manifest fallback does not match the pinned {name} source")
        verified_file(assets, record["source"], expected)
        verified_file(assets, record["path"], record["sha256"])
        checked_license(verified_file(assets, record["license"], record["license_sha256"]))
        fallbacks.append(record)
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
    return sources, fallbacks


def validate_cpfont(cpfont: Path) -> None:
    """Validate the regular CPFont v4 sections before publishing the installer."""
    font = cpfont.read_bytes()
    if (len(font) < 64 or font[:8] != b"CPFONT\0\0"
            or struct.unpack_from("<HHB", font, 8) != (4, 1, 1) or any(font[13:32])):
        raise ValueError(f"Expected a native regular CPFont v4: {cpfont}")
    (style, intervals, glyphs, line, ascender, descender, kern_left, kern_right,
     left_classes, right_classes, ligatures, offset) = struct.unpack_from("<B3xIIBhhHHBBBI4x", font, 32)
    if style != 0 or offset != 64 or not intervals or not glyphs:
        raise ValueError(f"Invalid regular CPFont table: {cpfont}")
    glyph_start = offset + intervals * 12
    kern_start = glyph_start + glyphs * 16
    bitmap_start = kern_start + (kern_left + kern_right) * 3 + left_classes * right_classes + ligatures * 8
    if bitmap_start > len(font):
        raise ValueError(f"Truncated CPFont sections: {cpfont}")
    count, previous = 0, -1
    for index in range(intervals):
        start, end, first = struct.unpack_from("<III", font, offset + index * 12)
        if start <= previous or end < start or end > 0x10FFFF or first != count:
            raise ValueError(f"Invalid CPFont Unicode intervals: {cpfont}")
        previous = end
        count += end - start + 1
    if count != glyphs:
        raise ValueError(f"CPFont glyph count does not match intervals: {cpfont}")
    bitmap_bytes = 0
    for index in range(glyphs):
        width, height, advance, left, top, length, bitmap_offset = struct.unpack_from(
            "<BBHhhH2xI", font, glyph_start + index * 16)
        if length != (width * height + 3) // 4 or bitmap_offset != bitmap_bytes:
            raise ValueError(f"Invalid CPFont glyph bitmap: {cpfont}")
        bitmap_bytes += length
    if bitmap_start + bitmap_bytes != len(font):
        raise ValueError(f"Invalid CPFont bitmap bounds: {cpfont}")


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
    sources, fallbacks = load_sources(assets)
    output.parent.mkdir(parents=True, exist_ok=True)
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="thai-font-pack-", dir=output.parent) as temporary:
        stage = Path(temporary) / "pack"
        stage.mkdir()
        fonts = []
        for source in sources:
            family = source["family"]
            family_dir = stage / "fonts" / family
            family_dir.mkdir(parents=True)
            shutil.copyfile(assets / source["license"], family_dir / "OFL.txt")
            for fallback in fallbacks:
                shutil.copyfile(assets / fallback["license"], family_dir / f"{fallback['name']}-OFL.txt")
            for size in SIZES:
                cpfont = family_dir / f"{family}_{size}.cpfont"
                command = [sys.executable, str(CONVERTER), "--regular", str(assets / source["regular"]),
                           "--intervals", INTERVALS, "--size", str(size),
                           "--output", str(cpfont)]
                for fallback in fallbacks:
                    command.extend(["--fallback-regular", str(assets / fallback["path"])])
                subprocess.run(command, cwd=ROOT, check=True)
                validate_cpfont(cpfont)
                fonts.append({"family": family, "point_size": size,
                              "cpfont": file_record(cpfont, stage)})
        (stage / "INSTALL.txt").write_text(INSTALL, encoding="utf-8", newline="\n")
        save_json(stage / "build-report.json", {
            "schema_version": 3, "dpi": 150, "sizes": list(SIZES), "intervals": INTERVALS,
            "style": "regular", "source_manifest_sha256": digest(assets / "manifest.json"),
            "sources": sources, "fallbacks": fallbacks,
            "converter_sha256": digest(CONVERTER),
            "packager_sha256": digest(Path(__file__)), "fonts": fonts,
            "files": [file_record(path, stage) for path in sorted(stage.rglob("*")) if path.is_file()],
        })
        # Publish only after all fonts and their licenses have been validated.
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
    print(f"Packaged {len(fonts)} CPFonts: {archive}")
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
