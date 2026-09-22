#!/usr/bin/env python3
"""Build pinned, shaping-preserving native font assets; never rasterize fonts.

Usage (Python 3.12, fonttools==4.65.0):
    python lib/EpdFont/scripts/build-native-text-assets.py --output build/native-assets

The default language set is all, matching custom_i18n_builtin_langs. Pass the
same --builtin-langs selection as gen_i18n.py when building a reduced set.
--fetch-sources restores missing Thai sources from the checksum-pinned archives;
normal builds are offline. Existing builtin sources must already be in the tree.

Outputs: NativeFontAssets.generated.h/.cpp, NativeFontAssets.metadata.json,
subsets/<asset-name>.ttf, licenses/<source-family>/<license-name>. The C++ arrays
and metadata are generated from exactly the same subset bytes. Metadata records
SHA-256 source/subset digests, FNV-1a-64 fingerprints, real cmap coverage, naming,
license digests and the tool/input identities needed to reproduce the result.
"""

import argparse
import hashlib
import importlib.util
import io
import json
import re
import sys
import unicodedata
import urllib.request
import zipfile
from pathlib import Path

import fontTools
from fontTools import subset
from fontTools.ttLib import TTFont

from font_ranges import BUILTIN_INTERVALS, NATIVE_SCRIPT_INTERVALS, UI_INTERVALS

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
HEADER_NAME = "NativeFontAssets.generated.h"
SOURCE_NAME = "NativeFontAssets.generated.cpp"
METADATA_NAME = "NativeFontAssets.metadata.json"
GENERATOR_REVISION = "native-font-assets-v1"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def canonical_json(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def text_bytes(path):
    # Git's Windows newline conversion must not change generated identities.
    return path.read_bytes().replace(b"\r\n", b"\n")


def source_path(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError(f"Source path escapes font directory: {relative}")
    return path


def check_pin(data, pin, label):
    if len(data) != pin["size"] or sha256(data) != pin["sha256"]:
        raise ValueError(f"Pinned source mismatch: {label} ({len(data)} bytes, SHA-256 {sha256(data)})")


def write_output(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    # Keep generated C++ mtimes stable on unchanged asset builds.
    if path.exists() and path.read_bytes() == data:
        return
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_bytes(data)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def fetch_sources(manifest, root):
    for archive_spec in manifest["archives"]:
        # Never fetch or overwrite a present-but-modified source silently.
        missing = []
        for member in archive_spec["members"]:
            destination = source_path(root, member["path"])
            if not destination.exists():
                missing.append(member)
            elif destination.suffix.lower() == ".ttf":
                check_pin(destination.read_bytes(), member, member["path"])
        if not missing:
            continue
        url = archive_spec["url"]
        if not url.startswith("https://"):
            raise ValueError(f"Source download requires HTTPS: {url}")
        with urllib.request.urlopen(url, timeout=90) as response:
            if not response.url.startswith("https://"):
                raise ValueError(f"Source download redirected away from HTTPS: {url}")
            archive_bytes = response.read()
        if sha256(archive_bytes) != archive_spec["sha256"]:
            raise ValueError(f"Archive checksum mismatch: {url}")
        # Check the complete archive before opening it, and each explicit member
        # before writing. No extractall(), traversal, or unlisted font variants.
        with zipfile.ZipFile(io.BytesIO(archive_bytes)) as archive:
            extracted = [(member, archive.read(member["member"])) for member in missing]
        for member, data in extracted:
            check_pin(data, member, member["member"])
        for member, data in extracted:
            write_output(source_path(root, member["path"]), data)


def translation_coverage(directory, builtin_langs):
    # Use the production parser and its English fallback semantics, not a second
    # interpretation of YAML escapes, missing keys or language selection.
    generator_path = REPO_ROOT / "scripts/gen_i18n.py"
    spec = importlib.util.spec_from_file_location("native_assets_i18n", generator_path)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    codes, names, tags, keys, translations, _ = generator.load_translations(str(directory))
    builtin = generator.parse_builtin_langs(builtin_langs, codes)
    indices = [i for i, code in enumerate(codes) if builtin is None or code in builtin]
    strings = [translations[key][i] for key in keys for i in indices]
    # Language names are UI text too, including the language picker labels.
    strings.extend(names)
    characters = {ord(character) for text in strings for character in text}
    semantic_inputs = {
        "languages": [codes[i] for i in indices],
        "language_names": names,
        "language_tags": tags,
        "strings": {key: [translations[key][i] for i in indices] for key in keys},
    }
    return characters, {
        "languages": semantic_inputs["languages"],
        "sha256": sha256(canonical_json(semantic_inputs)),
        "generator_sha256": sha256(text_bytes(generator_path)),
    }


def requested_coverage(translations):
    characters = set(translations)
    for start, end in BUILTIN_INTERVALS + UI_INTERVALS + NATIVE_SCRIPT_INTERVALS:
        characters.update(range(start, end + 1))
    # Canonical decomposition keeps NFD Latin bases/marks. Compatibility
    # decomposition also restores logical Arabic bases from legacy presentation
    # forms and the components of Thai SARA AM. This never rewrites input text.
    for codepoint in tuple(characters):
        characters.update(ord(c) for c in unicodedata.normalize("NFKD", chr(codepoint)))
    return characters


def rename_subset(font, entry):
    family = entry["subset_family"]
    style = {"BoldItalic": "Bold Italic"}.get(entry["style"], entry["style"])
    postscript_name = re.sub(r"[^A-Za-z0-9-]", "", family) + "-" + entry["style"]
    if len(postscript_name) > 63:
        raise ValueError(f"PostScript font name is too long: {postscript_name}")
    full_name = family + " " + style
    names = {
        1: family,
        2: style,
        3: f"{postscript_name};{entry['sha256'][:16]};{GENERATOR_REVISION}",
        4: full_name,
        6: postscript_name,
        16: family,
        17: style,
        18: full_name,
        21: family,
        22: style,
        25: re.sub(r"[^A-Za-z0-9]", "", family),
    }
    table = font["name"]
    for record in list(table.names):
        if record.nameID in names:
            table.setName(names[record.nameID], record.nameID, record.platformID,
                          record.platEncID, record.langID)
    for name_id in (1, 2, 3, 4, 6, 16, 17):
        table.setName(names[name_id], name_id, 3, 1, 0x0409)
    # Copyright, trademark, designer, attribution and license name records are
    # deliberately retained. Ubuntu names follow UFL 1.0 clause 2(c); Noto
    # derivatives use entirely new THCraft-prefixed primary names.
    if "CFF " in font:
        cff = font["CFF "].cff
        cff.fontNames = [postscript_name]
        cff.topDictIndex[0].FamilyName = family
        cff.topDictIndex[0].FullName = full_name


def subset_font(source, entry, requested):
    options = subset.Options()
    options.layout_features = ["*"]
    options.layout_scripts = ["*"]
    options.layout_closure = True
    options.hinting = True
    options.legacy_kern = True
    options.glyph_names = True
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.name_legacy = True
    options.notdef_glyph = True
    options.notdef_outline = True
    options.recommended_glyphs = True
    options.recalc_timestamp = False
    with TTFont(io.BytesIO(source), recalcTimestamp=False) as font:
        cmap = font.getBestCmap()
        if not cmap or not ("glyf" in font or "CFF " in font or "CFF2" in font):
            raise ValueError(f"Source has no Unicode outline face: {entry['name']}")
        # Thai assets keep every original encoded glyph, not just translations
        # or an example word list. Layout closure preserves unencoded forms.
        selected = set(cmap) if entry["keep_full_cmap"] else requested.intersection(cmap)
        layout_tables = {tag for tag in ("GSUB", "GPOS", "GDEF") if tag in font}
        subsetter = subset.Subsetter(options=options)
        subsetter.populate(unicodes=sorted(selected))
        subsetter.subset(font)
        retained = set(font.getBestCmap() or {})
        if selected != retained:
            raise ValueError(f"Subsetting changed requested cmap coverage: {entry['name']}")
        if not layout_tables.issubset(font.keys()):
            raise ValueError(f"Subsetting removed a shaping table: {entry['name']}")
        rename_subset(font, entry)
        # Fixed OpenType timestamps (1970-01-01) and canonical table ordering
        # remove wall-clock time from the output on every supported host.
        font["head"].created = 2082844800
        font["head"].modified = 2082844800
        stream = io.BytesIO()
        font.save(stream, reorderTables=True)
        return stream.getvalue(), retained, sorted(layout_tables)


def fnv1a64(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def emit_cpp(output, assets):
    header = """// Generated by build-native-text-assets.py; do not edit.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace native_text {
namespace assets {
struct FontAsset {
  const char* name;
  const unsigned char* data;
  size_t size;
  uint64_t fingerprint;
};
extern const FontAsset fonts[];
extern const size_t fontCount;
}  // namespace assets
}  // namespace native_text
"""
    lines = ["// Generated by build-native-text-assets.py; do not edit.",
             f'#include "{HEADER_NAME}"', "", "namespace native_text {",
             "namespace assets {", "namespace {"]
    for index, (entry, data, fingerprint) in enumerate(assets):
        lines.append(f"// {entry['name']}; source SHA-256: {entry['sha256']}")
        lines.append(f"alignas(4) const unsigned char font_{index}[] = {{")
        for offset in range(0, len(data), 16):
            lines.append("  " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16]) + ",")
        lines.append("};")
    lines.extend(["}  // namespace", "", "const FontAsset fonts[] = {"])
    for index, (entry, data, fingerprint) in enumerate(assets):
        lines.append(f'  {{"{entry["name"]}", font_{index}, sizeof(font_{index}), '
                     f'UINT64_C(0x{fingerprint:016x})}},')
    lines.extend(["};", "const size_t fontCount = sizeof(fonts) / sizeof(fonts[0]);",
                  "}  // namespace assets", "}  // namespace native_text", ""])
    write_output(output / HEADER_NAME, header.encode("utf-8"))
    write_output(output / SOURCE_NAME, "\n".join(lines).encode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", required=True, type=Path, help="Generated resource directory (not checked in)")
    parser.add_argument("--manifest", type=Path, default=SCRIPT_DIR / "native-text-assets.json")
    parser.add_argument("--translations", type=Path, default=REPO_ROOT / "lib/I18n/translations")
    parser.add_argument("--builtin-langs", default="all", help="gen_i18n.py language codes; English is always included")
    parser.add_argument("--fetch-sources", action="store_true", help="Download only missing pinned Thai sources")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest["version"] != 1:
        raise ValueError("Unsupported native font manifest version")
    if fontTools.__version__ != manifest["fonttools_version"]:
        raise ValueError(f"Reproducible generation requires fonttools=={manifest['fonttools_version']}")
    if unicodedata.unidata_version != manifest["unicode_data_version"]:
        raise ValueError(f"Reproducible generation requires Unicode {manifest['unicode_data_version']} (Python 3.12)")
    root = (args.manifest.resolve().parent / manifest["source_root"]).resolve()
    if args.fetch_sources:
        fetch_sources(manifest, root)
    licenses = {}
    for name, license_spec in manifest["licenses"].items():
        data = text_bytes(source_path(root, license_spec["path"]))
        if sha256(data) != license_spec["sha256_lf"]:
            raise ValueError(f"License checksum mismatch: {license_spec['path']}")
        licenses[name] = data
    entries = manifest["fonts"]
    if not entries or len({entry["name"] for entry in entries}) != len(entries):
        raise ValueError("Manifest must contain unique named font assets")
    sources = []
    for entry in entries:
        if not re.fullmatch(r"[A-Za-z0-9-]+", entry["name"]):
            raise ValueError(f"Invalid asset name: {entry['name']}")
        if entry["license"] not in licenses:
            raise ValueError(f"Font has no verified license: {entry['name']}")
        data = source_path(root, entry["path"]).read_bytes()
        check_pin(data, entry, entry["path"])
        sources.append((entry, data))
    translations, translation_inputs = translation_coverage(args.translations, args.builtin_langs)
    requested = requested_coverage(translations)
    assets = []
    metadata_fonts = []
    all_retained = set()
    for entry, source in sources:
        data, retained, layout_tables = subset_font(source, entry, requested)
        fingerprint = fnv1a64(data)
        assets.append((entry, data, fingerprint))
        all_retained.update(retained)
        write_output(args.output / "subsets" / (entry["name"] + ".ttf"), data)
        metadata_fonts.append({
            "name": entry["name"], "source": entry["path"],
            "source_sha256": entry["sha256"], "source_size": entry["size"],
            "sha256": sha256(data), "size": len(data),
            "fingerprint_fnv1a64": f"{fingerprint:016x}",
            "subset_family": entry["subset_family"], "style": entry["style"],
            "license": entry["license"], "layout_tables": layout_tables,
            "codepoints": [f"U+{cp:04X}" for cp in sorted(retained)],
        })
    for name, data in licenses.items():
        write_output(args.output / "licenses" / manifest["licenses"][name]["path"], data)
    # Sources cannot supply characters absent from their cmaps. Record them
    # explicitly instead of fabricating coverage; the engine uses real U+FFFD.
    uncovered = sorted(cp for cp in translations - all_retained
                       if unicodedata.category(chr(cp)) not in ("Cc", "Cf"))
    metadata = {
        "revision": GENERATOR_REVISION,
        "fonttools_version": fontTools.__version__,
        "unicode_data_version": unicodedata.unidata_version,
        "manifest_sha256": sha256(canonical_json(manifest)),
        "generator_sha256": sha256(text_bytes(Path(__file__))),
        "ranges_sha256": sha256(text_bytes(SCRIPT_DIR / "font_ranges.py")),
        "translations": translation_inputs,
        "fonts": metadata_fonts,
        "licenses": manifest["licenses"],
        "stacks": manifest["stacks"],
        "render_policy": manifest["render_policy"],
        "uncovered_translation_codepoints": [f"U+{cp:04X}" for cp in uncovered],
    }
    emit_cpp(args.output, assets)
    write_output(args.output / METADATA_NAME, canonical_json(metadata))
    print(f"Generated {len(assets)} fonts, {sum(len(data) for _, data, _ in assets)} bytes: {args.output}")
    if uncovered:
        print("Source fonts do not cover these translation characters: " +
              ", ".join(f"U+{cp:04X}" for cp in uncovered), file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"Native font generation failed: {error}", file=sys.stderr)
        sys.exit(1)
