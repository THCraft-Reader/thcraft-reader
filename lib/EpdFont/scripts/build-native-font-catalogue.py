#!/usr/bin/env python3
"""Derive the Pro download catalogue from sd-fonts.yaml, without embedding fonts.

Ordinary builds are offline:
    python build-native-font-catalogue.py --output <generated-directory>

Explicitly update every source, validate original SFNT bytes and refresh pins:
    python build-native-font-catalogue.py --refresh --output <generated-directory>

The checked-in lock records immutable GitHub commits, source digests and actual
font metadata. Non-GitHub HTTPS sources retain verified content digests; if an
upstream changes those bytes, firmware rejects them until an explicit refresh.
Original fonts live only in the ignored downloaded_fonts/native/ cache. Variable
fonts are never instanced, subsetted or rasterized by this generator.
"""

import argparse
import hashlib
import json
import math
import re
import struct
import sys
import urllib.parse
import urllib.request
import zlib
from pathlib import Path

import yaml

from font_sources import DOWNLOAD_DIR, HttpsRedirectHandler, public_source_url, require_https, resolve_font_source

SCRIPT_DIR = Path(__file__).resolve().parent
STYLE_SUFFIXES = {"regular": "Regular", "bold": "Bold", "italic": "Italic", "bolditalic": "BoldItalic"}
REVISION = "native-font-catalogue-v1"
HEADER_NAME = "NativeFontCatalogue.generated.h"
SOURCE_NAME = "NativeFontCatalogue.generated.cpp"


def canonical_json(value):
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("ascii")


def write_output(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() == data:
        return
    temporary = path.with_name(path.name + ".part")
    try:
        temporary.write_bytes(data)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def write_json(path, value):
    write_output(path, (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))


def load_config(path):
    config = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(config, dict) or not isinstance(config.get("families"), list) or not config["families"]:
        raise ValueError(f"{path}: no families declared")
    groups = config.get("scriptGroups")
    if not isinstance(groups, list) or not groups:
        raise ValueError(f"{path}: no scriptGroups declared")
    tags = set()
    for group in groups:
        if not isinstance(group, dict) or not isinstance(group.get("tag"), str) or not group["tag"]:
            raise ValueError(f"{path}: invalid script group tag")
        if group["tag"] in tags or not isinstance(group.get("label"), str) or not group["label"]:
            raise ValueError(f"{path}: duplicate script group or missing label: {group['tag']}")
        tags.add(group["tag"])
    names = set()
    for family in config["families"]:
        name = family.get("name", "")
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,30}", name):
            raise ValueError(f"{path}: invalid family name (maximum 31 ASCII characters): {name!r}")
        if name.casefold() in names:
            raise ValueError(f"{path}: duplicate family: {name}")
        names.add(name.casefold())
        if not isinstance(family.get("description"), str) or not family["description"]:
            raise ValueError(f"{name}: missing description")
        scripts = family.get("scripts")
        if not isinstance(scripts, list) or not scripts or any(tag not in tags for tag in scripts):
            raise ValueError(f"{name}: missing or unknown scripts")
        if len(scripts) != len(set(scripts)):
            raise ValueError(f"{name}: duplicate script tags")
        styles = family.get("styles")
        if not isinstance(styles, dict) or "regular" not in styles or set(styles) - STYLE_SUFFIXES.keys():
            raise ValueError(f"{name}: styles require regular and only regular/bold/italic/bolditalic")
        for style, spec in styles.items():
            try:
                if not isinstance(spec, dict):
                    raise ValueError("invalid source specification")
                url = public_source_url(spec)
                extension = Path(urllib.parse.unquote(urllib.parse.urlsplit(url).path)).suffix.lower()
                if extension not in (".ttf", ".otf"):
                    raise ValueError(f"source URL is not a TTF/OTF: {url}")
                axes = spec.get("variable", {})
                if not isinstance(axes, dict) or len(axes) > 8:
                    raise ValueError("at most eight variation axes are supported")
                for tag, value in axes.items():
                    if not isinstance(tag, str) or not re.fullmatch(r"[\x20-\x7e]{4}", tag):
                        raise ValueError(f"invalid four-character axis tag: {tag!r}")
                    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                        raise ValueError(f"invalid axis coordinate: {tag}={value!r}")
            except (ValueError, TypeError) as error:
                raise ValueError(f"{name}/{style}: {error}") from error
    return config


def pin_github_url(url, refs):
    parsed = urllib.parse.urlsplit(url)
    if parsed.hostname != "raw.githubusercontent.com":
        return url
    parts = parsed.path.lstrip("/").split("/", 3)
    if len(parts) != 4 or parsed.query:
        raise ValueError(f"Unsupported GitHub raw URL: {url}")
    owner, repository, ref, relative = parts
    if re.fullmatch(r"[0-9a-fA-F]{40}", ref):
        return url
    key = f"{owner}/{repository}/{ref}"
    if key not in refs:
        api = f"https://api.github.com/repos/{owner}/{repository}/commits/{urllib.parse.quote(ref, safe='')}"
        opener = urllib.request.build_opener(HttpsRedirectHandler())
        last_error = None
        for attempt in range(3):
            try:
                request = urllib.request.Request(api, headers={"User-Agent": "THCraft-Font-Assets/1"})
                with opener.open(request, timeout=60) as response:
                    result = json.load(response)
                commit = result.get("sha", "")
                if not re.fullmatch(r"[0-9a-f]{40}", commit):
                    raise ValueError(f"GitHub returned no immutable commit: {api}")
                refs[key] = commit
                print(f"  Pinned {key}: {commit}", flush=True)
                break
            except Exception as error:
                last_error = error
                print(f"  GitHub resolve attempt {attempt + 1}: {error}", flush=True)
        else:
            raise RuntimeError(f"Cannot resolve {key}: {last_error}") from last_error
    return f"https://raw.githubusercontent.com/{owner}/{repository}/{refs[key]}/{relative}"


def inspect_font(path):
    from fontTools.ttLib import TTFont

    data = path.read_bytes()
    if len(data) < 12 or data[:4] not in (b"\x00\x01\x00\x00", b"OTTO"):
        raise ValueError("not a standalone TrueType/OpenType SFNT (collections/WOFF/bitmap fonts unsupported)")
    table_count = struct.unpack_from(">H", data, 4)[0]
    directory_end = 12 + 16 * table_count
    if not table_count or directory_end > len(data):
        raise ValueError("truncated SFNT directory")
    tags = set()
    for index in range(table_count):
        tag, _, offset, length = struct.unpack_from(">4sIII", data, 12 + 16 * index)
        if tag in tags or offset < directory_end or offset > len(data) or length > len(data) - offset:
            raise ValueError(f"invalid/duplicate/out-of-bounds SFNT table {tag!r}")
        tags.add(tag)
    with TTFont(path, lazy=False, checkChecksums=2) as font:
        font.ensureDecompiled()
        if font.flavor is not None or not {"head", "maxp", "hhea", "hmtx", "cmap", "name"}.issubset(font.keys()):
            raise ValueError("missing scalable Unicode font metadata")
        outlines = [tag for tag in ("glyf", "CFF ", "CFF2") if tag in font]
        if len(outlines) != 1 or ("glyf" in font and "loca" not in font):
            raise ValueError("font must have one supported scalable outline format")
        if not 16 <= font["head"].unitsPerEm <= 16384 or font["maxp"].numGlyphs <= 0:
            raise ValueError("invalid scalable font metrics")
        cmap = font.getBestCmap()
        glyphs = set(font.getGlyphOrder())
        if not cmap or not any(table.isUnicode() for table in font["cmap"].tables) or not set(cmap.values()) <= glyphs:
            raise ValueError("no valid Unicode character map")
        supported_axes = {}
        if "fvar" in font:
            for axis in font["fvar"].axes:
                values = (axis.minValue, axis.defaultValue, axis.maxValue)
                if not all(math.isfinite(value) for value in values) or not values[0] <= values[1] <= values[2]:
                    raise ValueError(f"invalid variation axis range: {axis.axisTag}")
                supported_axes[axis.axisTag] = {"min": values[0], "default": values[1], "max": values[2]}
        return {
            "size": len(data),
            "crc32": zlib.crc32(data) & 0xFFFFFFFF,
            "sha256": hashlib.sha256(data).hexdigest(),
            "extension": "ttf" if data[:4] == b"\x00\x01\x00\x00" else "otf",
            "metadata": {"outline": outlines[0], "glyphCount": font["maxp"].numGlyphs,
                         "unicodeCodepoints": len(cmap), "axes": supported_axes},
        }


def style_axes(spec, source):
    axes = {}
    supported = source["metadata"]["axes"]
    for tag, value in spec.get("variable", {}).items():
        if tag not in supported:
            raise ValueError(f"source does not support requested variation axis {tag}")
        bounds = supported[tag]
        axes[tag] = max(bounds["min"], min(bounds["max"], value))
    return axes


def refresh_lock(config):
    refs = {}
    sources = {}
    failures = []
    for family in config["families"]:
        for style, spec in family["styles"].items():
            label = f"{family['name']}/{style}"
            try:
                original_url = public_source_url(spec)
                if original_url not in sources:
                    url = pin_github_url(original_url, refs)
                    # Content-address the URL to share variable files between styles
                    # without conflating different commits or equally named files.
                    cache_key = hashlib.sha256(url.encode("utf-8")).hexdigest()
                    path, resolved_url = resolve_font_source(
                        {"url": url}, cache_key, style,
                        download_dir=DOWNLOAD_DIR / "native", https_only=True,
                        refresh=urllib.parse.urlsplit(url).hostname != "raw.githubusercontent.com")
                    source = inspect_font(path)
                    source.update({"source": original_url, "url": require_https(resolved_url)})
                    sources[original_url] = source
                style_axes(spec, sources[original_url])
                print(f"  Verified {label}: {sources[original_url]['size']} bytes", flush=True)
            except Exception as error:
                failures.append(f"{label}: {error}")
                print(f"ERROR: {failures[-1]}", file=sys.stderr, flush=True)
    if failures:
        raise RuntimeError("No catalogue published; source failures:\n  " + "\n  ".join(failures))
    return {
        "version": 1, "generator": REVISION,
        "configSha256": hashlib.sha256(canonical_json(config)).hexdigest(),
        "githubRefs": refs,
        "sources": list(sources.values()),
    }


def derive_catalogue(config, lock):
    if lock.get("version") != 1 or lock.get("generator") != REVISION:
        raise ValueError("Unsupported native source lock; run --refresh")
    if lock.get("configSha256") != hashlib.sha256(canonical_json(config)).hexdigest():
        raise ValueError("sd-fonts.yaml changed; explicitly regenerate the native catalogue with --refresh")
    sources = {}
    for source in lock["sources"]:
        if source["source"] in sources:
            raise ValueError(f"Duplicate source in lock: {source['source']}")
        require_https(source["url"])
        require_https(source["source"])
        if not re.fullmatch(r"[0-9a-f]{64}", source["sha256"]):
            raise ValueError(f"Invalid locked SHA-256: {source['source']}")
        if not isinstance(source["size"], int) or not 12 <= source["size"] <= 0xFFFFFFFF:
            raise ValueError(f"Invalid locked size: {source['source']}")
        if not isinstance(source["crc32"], int) or not 0 <= source["crc32"] <= 0xFFFFFFFF:
            raise ValueError(f"Invalid locked CRC32: {source['source']}")
        if source["extension"] not in ("ttf", "otf"):
            raise ValueError(f"Invalid locked font format: {source['source']}")
        if urllib.parse.urlsplit(source["url"]).hostname == "raw.githubusercontent.com":
            parts = urllib.parse.urlsplit(source["url"]).path.lstrip("/").split("/", 3)
            if len(parts) != 4 or not re.fullmatch(r"[0-9a-fA-F]{40}", parts[2]):
                raise ValueError(f"Unlocked GitHub source: {source['url']}")
        sources[source["source"]] = source
    families = []
    used_sources = set()
    used_scripts = set()
    for family in config["families"]:
        files = []
        styles = []
        for style, suffix in STYLE_SUFFIXES.items():
            if style not in family["styles"]:
                continue
            spec = family["styles"][style]
            original_url = public_source_url(spec)
            if original_url not in sources:
                raise ValueError(f"{family['name']}/{style}: source missing from lock; run --refresh")
            source = sources[original_url]
            used_sources.add(original_url)
            entry = {"name": f"{family['name']}-{suffix}.{source['extension']}",
                     "size": source["size"], "crc32": source["crc32"], "url": source["url"]}
            try:
                axes = style_axes(spec, source)
            except ValueError as error:
                raise ValueError(f"{family['name']}/{style}: {error}") from error
            if axes:
                entry["axes"] = axes
            files.append(entry)
            styles.append(style)
        families.append({"name": family["name"], "description": family["description"],
                         "styles": styles, "scripts": family["scripts"], "files": files})
        used_scripts.update(family["scripts"])
    if used_sources != sources.keys():
        raise ValueError("Native source lock contains obsolete sources; run --refresh")
    return {"version": 1, "format": "opentype",
            "scriptGroups": [group for group in config["scriptGroups"] if group["tag"] in used_scripts],
            "families": families}


def emit_cpp(output, catalogue):
    # Emit small adjacent ASCII literals instead of one huge raw literal (MSVC
    # limits individual literals). Escaped JSON preserves all original labels.
    data = canonical_json(catalogue).decode("ascii")
    header = """// Generated by build-native-font-catalogue.py; do not edit.
#pragma once
#include <stddef.h>

namespace native_text {
namespace assets {
extern const char fontCatalogue[];
extern const size_t fontCatalogueSize;
}  // namespace assets
}  // namespace native_text
"""
    lines = ["// Generated by build-native-font-catalogue.py; do not edit.",
             f'#include "{HEADER_NAME}"', "", "namespace native_text {", "namespace assets {",
             "const char fontCatalogue[] ="]
    lines.extend("    " + json.dumps(data[offset:offset + 1000]) for offset in range(0, len(data), 1000))
    lines.extend([";", "const size_t fontCatalogueSize = sizeof(fontCatalogue) - 1;",
                  "}  // namespace assets", "}  // namespace native_text", ""])
    write_output(output / HEADER_NAME, header.encode("ascii"))
    write_output(output / SOURCE_NAME, "\n".join(lines).encode("ascii"))
    return len(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", type=Path, default=SCRIPT_DIR / "sd-fonts.yaml")
    parser.add_argument("--lock", type=Path, default=SCRIPT_DIR / "native-font-sources.lock.json")
    parser.add_argument("--catalogue", type=Path, default=SCRIPT_DIR / "native-font-catalogue.json")
    parser.add_argument("--output", type=Path, help="Emit NativeFontCatalogue.generated.h/.cpp in this directory")
    parser.add_argument("--refresh", action="store_true", help="Explicit online source validation and pin refresh")
    args = parser.parse_args()
    config = load_config(args.config)
    if args.refresh:
        lock = refresh_lock(config)
    else:
        lock = json.loads(args.lock.read_text(encoding="utf-8"))
    catalogue = derive_catalogue(config, lock)
    # Publish only after every family/style succeeds, so outages never turn into
    # a truncated catalogue. Ordinary builds cannot update the checked-in inputs.
    if args.refresh:
        write_json(args.lock, lock)
        write_json(args.catalogue, catalogue)
    elif json.loads(args.catalogue.read_text(encoding="utf-8")) != catalogue:
        raise ValueError("Derived native catalogue differs from lock/config; run --refresh")
    compiled_size = emit_cpp(args.output, catalogue) if args.output else len(canonical_json(catalogue))
    print(f"Native catalogue: {len(catalogue['families'])} families, "
          f"{sum(len(f['files']) for f in catalogue['families'])} styles, "
          f"{len(lock['sources'])} original sources, {compiled_size} compiled JSON bytes")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
