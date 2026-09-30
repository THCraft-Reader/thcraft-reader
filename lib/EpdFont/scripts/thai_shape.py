"""Desktop-only Thai CPFont v4 companion baking; never imported by native conversion.

Recipe coordinates are signed y-up 12.4 pixels. All byte offsets in a style,
including entries of its suffix offset table, are relative to that style start.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import importlib.metadata
import json
from pathlib import Path
import struct
import zlib

MAGIC = b"CPSHAPE\0"
VERSION = 1
KEY_COUNT = 46 * 82
STYLE_LIMIT = 96 * 1024
FAMILY_LIMIT = 384 * 1024
FIRST_SIGNS = (None, 0xE31, *range(0xE34, 0xE3B), 0xE47, 0xE4D)
TERMINALS = (None, *range(0xE48, 0xE4D), 0xE4E)
RECORD = struct.Struct("<HHhh")
STYLE_HEADER = struct.Struct("<HHIIIIhhHH")


def contexts():
    """Yield exactly the dense key order, retaining original Unicode scalars."""
    for base in range(0xE01, 0xE2F):
        for first in FIRST_SIGNS:
            for terminal in TERMINALS:
                yield tuple(cp for cp in (base, first, terminal) if cp is not None)
        for terminal in (None, *range(0xE48, 0xE4C)):
            yield tuple(cp for cp in (base, terminal, 0xE33) if cp is not None)


def checked(value, low, high, label):
    if not low <= value <= high:
        raise ValueError(f"{label} {value} outside [{low}, {high}]")
    return value


def fp4(value):
    """26.6 to 12.4, nearest with half towards +infinity, as CPFont's converter."""
    return (value + 2) // 4


def allocate_codepoints(glyph_ids, cmap, occupied=()):
    """Reuse real BMP Unicode; assign only unencoded IDs sorted unused BMP PUA."""
    by_glyph = {}
    for cp, gid in sorted(cmap.items()):
        if 0 <= cp <= 0xFFFF and not 0xD800 <= cp <= 0xDFFF and gid:
            by_glyph.setdefault(gid, cp)
    used = set(cmap) | set(occupied)
    available = iter(cp for cp in range(0xE000, 0xF900) if cp not in used)
    mapping, alternates = {}, {}
    for gid in sorted(set(glyph_ids)):
        if not gid:
            raise ValueError("Thai shaping produced missing glyph ID 0")
        cp = by_glyph.get(gid)
        if cp is None:
            cp = next(available, None)
            if cp is None:
                raise ValueError("Thai alternate glyphs exhaust unused BMP PUA")
            alternates[cp] = gid
        mapping[gid] = cp
    return mapping, alternates


@dataclass
class BakedStyle:
    # Each record here retains the original source glyph ID, not a fabricated cmap.
    oracle: list
    codepoints: dict
    alternates: dict
    recipes: list
    payload: bytes = b""
    metrics: tuple = ()
    counts: dict = field(default_factory=dict)


def bake_style(fontfile, size, fallback_fontfile=None):
    import freetype
    from fontTools.ttLib import TTFont
    try:
        import uharfbuzz as hb
    except ImportError as exc:
        raise RuntimeError("--thai-shaping requires desktop package uharfbuzz") from exc

    checked(size, 1, 255, "point size")
    source = Path(fontfile).read_bytes()
    occupied = set()
    with TTFont(fontfile) as font:
        if "fvar" in font:
            raise ValueError("--thai-shaping requires a frozen font face, not variable axes")
        for table in font["cmap"].tables:
            if table.isUnicode():
                occupied.update(table.cmap)
    if fallback_fontfile:
        with TTFont(fallback_fontfile) as font:
            for table in font["cmap"].tables:
                if table.isUnicode():
                    occupied.update(table.cmap)
    face = freetype.Face(str(fontfile))
    face.set_char_size(size << 6, size << 6, 150, 150)
    cmap = dict(face.get_chars())
    required = {cp for text in contexts() for cp in text}
    missing = sorted(required - {cp for cp, gid in cmap.items() if gid})
    if missing:
        raise ValueError("Thai source face lacks " + ", ".join(f"U+{cp:04X}" for cp in missing))
    font = hb.Font(hb.Face(source))
    hb.ot_font_set_funcs(font)
    scale = round(size * 150 / 72 * 64)
    font.scale = (scale, scale)
    font.ppem = (face.size.x_ppem, face.size.y_ppem)
    oracle = []
    for key, text in enumerate(contexts()):
        buffer = hb.Buffer()
        buffer.add_codepoints(list(text))
        buffer.script, buffer.language, buffer.direction = "thai", "th", "ltr"
        hb.shape(font, buffer)
        records = []
        for info, pos in zip(buffer.glyph_infos, buffer.glyph_positions):
            if pos.y_advance:
                raise ValueError(f"Thai key {key} has nonzero y advance")
            if not info.codepoint:
                raise ValueError(f"Thai key {key} produced missing glyph")
            records.append((info.codepoint,
                            checked(fp4(pos.x_advance), 0, 65535, "advanceFP"),
                            checked(fp4(pos.x_offset), -32768, 32767, "xOffsetFP"),
                            checked(fp4(pos.y_offset), -32768, 32767, "yOffsetFP")))
        checked(len(records), 1, 6, f"key {key} glyph count")
        oracle.append(tuple(records))
    mapping, alternates = allocate_codepoints(
        (record[0] for recipe in oracle for record in recipe), cmap, occupied)
    recipes = [tuple((mapping[gid], advance, x, y) for gid, advance, x, y in recipe)
               for recipe in oracle]
    return BakedStyle(oracle, mapping, alternates, recipes)


def expanded_intervals(intervals, codepoints):
    merged = []
    for start, end in sorted(list(intervals) + [(cp, cp) for cp in codepoints]):
        if merged and start <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
        else:
            merged.append((start, end))
    return merged


def serialize_style(recipes, ascender, descender, advance_y):
    if len(recipes) != KEY_COUNT:
        raise ValueError(f"Expected {KEY_COUNT} dense keys")
    checked(ascender, -32768, 32767, "ascender")
    checked(descender, -32768, 32767, "descender")
    checked(advance_y, 0, 255, "CPFont line advance")
    bases, suffixes, base_ids, suffix_ids, dense = [], [], {}, {}, bytearray()
    for recipe in recipes:
        if recipe is None:
            dense += struct.pack("<HH", 0xFFFF, 0xFFFF)
            continue
        checked(len(recipe), 1, 6, "recipe glyph count")
        encoded = []
        for cp, advance, x, y in recipe:
            checked(cp, 0, 65535, "CPFont codepoint")
            checked(advance, 0, 65535, "advanceFP")
            checked(x, -32768, 32767, "xOffsetFP")
            checked(y, -32768, 32767, "yOffsetFP")
            encoded.append(RECORD.pack(cp, advance, x, y))
        base = encoded[0]
        suffix = bytes([len(encoded) - 1]) + b"".join(encoded[1:])
        if base not in base_ids:
            base_ids[base] = len(bases)
            bases.append(base)
        if suffix not in suffix_ids:
            suffix_ids[suffix] = len(suffixes)
            suffixes.append(suffix)
        dense += struct.pack("<HH", base_ids[base], suffix_ids[suffix])
    checked(len(bases), 0, 65534, "base count")
    checked(len(suffixes), 0, 65534, "suffix count")
    base_offset = STYLE_HEADER.size + len(dense)
    table_offset = base_offset + len(bases) * RECORD.size
    data_offset = table_offset + (len(suffixes) + 1) * 4
    offsets, end = [], data_offset
    for suffix in suffixes:
        offsets.append(end)
        end += len(suffix)
    offsets.append(end)
    checked(end, 0, STYLE_LIMIT, "style companion bytes")
    header = STYLE_HEADER.pack(len(bases), len(suffixes), STYLE_HEADER.size,
                               base_offset, table_offset, data_offset,
                               ascender, descender, advance_y, 0)
    return (header + dense + b"".join(bases) +
            struct.pack(f"<{len(offsets)}I", *offsets) + b"".join(suffixes))


def decode_style(payload):
    """Strict independent byte reader used for the baker's serialization round trip."""
    if not STYLE_HEADER.size <= len(payload) <= STYLE_LIMIT:
        raise ValueError("Invalid style payload size")
    nb, ns, dense, base, table, data, asc, desc, line, reserved = STYLE_HEADER.unpack_from(payload)
    if (reserved or dense != 28 or base != dense + KEY_COUNT * 4 or
            table != base + nb * 8 or data != table + (ns + 1) * 4 or data > len(payload)):
        raise ValueError("Invalid style section bounds")
    offsets = struct.unpack_from(f"<{ns + 1}I", payload, table)
    if offsets[0] != data or offsets[-1] != len(payload):
        raise ValueError("Invalid suffix endpoints")
    suffixes = []
    for start, end in zip(offsets, offsets[1:]):
        if not data <= start < end <= len(payload):
            raise ValueError("Invalid suffix span")
        count = payload[start]
        if count > 5 or end - start != 1 + count * 8:
            raise ValueError("Invalid suffix count")
        suffixes.append(tuple(RECORD.unpack_from(payload, start + 1 + i * 8) for i in range(count)))
    result = []
    for key in range(KEY_COUNT):
        bid, sid = struct.unpack_from("<HH", payload, dense + key * 4)
        if bid == sid == 0xFFFF:
            result.append(None)
        elif bid >= nb or sid >= ns:
            raise ValueError("Invalid recipe index")
        else:
            result.append((RECORD.unpack_from(payload, base + bid * 8),) + suffixes[sid])
    return result, (asc, desc, line)


def finish_style(model, raster):
    glyphs = {glyph.code_point: glyph for glyph, _ in raster.all_glyphs}
    asc, desc = raster.ascender, raster.descender
    ink_top, ink_bottom = 0, 0
    for recipe in model.recipes:
        for cp, advance, x, y in recipe:
            if cp not in glyphs:
                raise ValueError(f"Recipe references absent CPFont glyph U+{cp:04X}")
            glyph = glyphs[cp]
            if glyph.width and glyph.height:
                # Runtime placement rounds the y-up offset once (fp4::toPixel).
                top = glyph.top + (y + 8) // 16
                asc = max(asc, top)
                desc = min(desc, top - glyph.height)
                ink_top = max(ink_top, top)
                ink_bottom = min(ink_bottom, top - glyph.height)
    line = max(raster.advanceY, ink_top - ink_bottom)
    model.metrics = asc, desc, line
    model.payload = serialize_style(model.recipes, asc, desc, line)
    decoded, metrics = decode_style(model.payload)
    reverse = {cp: gid for gid, cp in model.codepoints.items()}
    restored = [tuple((reverse[cp], advance, x, y) for cp, advance, x, y in recipe)
                for recipe in decoded]
    if restored != model.oracle or metrics != model.metrics:
        raise ValueError("Thai recipe serialization differs from HarfBuzz oracle")
    nb, ns = struct.unpack_from("<HH", model.payload)
    model.counts = {"dense_keys": len(model.recipes), "base_records": nb,
                    "suffix_sequences": ns, "payload_bytes": len(model.payload),
                    "alternate_glyphs": len(model.alternates)}
    return raster._replace(ascender=asc, descender=desc, advanceY=line)


def make_companion(header, toc, packed_sections, models):
    """CRC the CPFont metadata in exact file order, excluding only bitmap sections."""
    if not 1 <= len(models) <= 4 or set(models) != set(packed_sections):
        raise ValueError("Invalid companion styles")
    cpfont_bytes = len(header) + len(toc)
    metrics_crc = zlib.crc32(toc, zlib.crc32(header))
    for style in sorted(packed_sections):
        for section in packed_sections[style][:-1]:
            metrics_crc = zlib.crc32(section, metrics_crc)
        cpfont_bytes += sum(map(len, packed_sections[style]))
    checked(cpfont_bytes, 0, 0xFFFFFFFF, "CPFont file bytes")
    toc_bytes = bytearray()
    offset = 32 + 12 * len(models)
    for style, model in sorted(models.items()):
        checked(style, 0, 3, "style ID")
        checked(len(model.payload), 28, STYLE_LIMIT, "style companion bytes")
        toc_bytes += struct.pack("<B3xII", style, offset, len(model.payload))
        offset += len(model.payload)
    checked(offset, 32, FAMILY_LIMIT, "family companion bytes")
    payload = bytes(toc_bytes) + b"".join(model.payload for _, model in sorted(models.items()))
    return struct.pack("<8sHHIIIII", MAGIC, VERSION, 32, cpfont_bytes,
                       metrics_crc, zlib.crc32(payload), offset, len(models)) + payload


def write_report(path, companion, models, sources, fallbacks, size, force_autohint):
    import freetype
    import uharfbuzz as hb
    report = {"format_version": VERSION, "dpi": 150, "point_size": size,
              "force_autohint": force_autohint,
              "versions": {name: importlib.metadata.version(name)
                           for name in ("fonttools", "freetype-py", "uharfbuzz")},
              "harfbuzz": hb.version_string(), "freetype": freetype.version(),
              "companion_bytes": len(companion),
              "companion_sha256": hashlib.sha256(companion).hexdigest(),
              "metrics_crc32": struct.unpack_from("<I", companion, 16)[0],
              "payload_crc32": struct.unpack_from("<I", companion, 20)[0],
              "styles": {}}
    for style, model in sorted(models.items()):
        entry = {"source_sha256": hashlib.sha256(Path(sources[style]).read_bytes()).hexdigest(),
                 "source": str(sources[style]), **model.counts,
                 "ascender": model.metrics[0], "descender": model.metrics[1],
                 "line_advance": model.metrics[2],
                 "glyph_mapping": [{"glyph_id": gid, "codepoint": cp,
                                     "alternate": cp in model.alternates}
                                    for gid, cp in sorted(model.codepoints.items())]}
        if style in fallbacks:
            entry["fallback_sha256"] = hashlib.sha256(Path(fallbacks[style]).read_bytes()).hexdigest()
        report["styles"][str(style)] = entry
    Path(path).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
