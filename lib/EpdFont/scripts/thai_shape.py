"""Desktop-only Thai CPFont v4 companion baking; never imported by native conversion.

Recipe coordinates are signed y-up 12.4 pixels. A CPSHAPE v2 style is a 12-byte
header, KEY_COUNT 3-byte entries (base ID bits 0-6, mark count bits 7-9, first
mark record bits 10-23), then base records and shared mark records.
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
VERSION = 2
KEY_COUNT = 46 * 82
ENTRY_BYTES = 3
ABSENT_ENTRY = 0xFFFFFF
MAX_BASES = 127
MAX_MARKS = 5
MAX_RECORDS = 16383
# CPFont v4 reserved header bytes that repeat the companion's metrics CRC.
PAIR_TAG_OFFSET = 16
STYLE_LIMIT = 96 * 1024
FAMILY_LIMIT = 384 * 1024
FIRST_SIGNS = (None, 0xE31, *range(0xE34, 0xE3B), 0xE47, 0xE4D)
TERMINALS = (None, *range(0xE48, 0xE4D), 0xE4E)
RECORD = struct.Struct("<HHhh")
STYLE_HEADER = struct.Struct("<HHhhHH")


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


def bake_style(fontfile, size, fallback_fontfiles=()):
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
    for fallback_fontfile in fallback_fontfiles:
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


def pack_marks(sequences):
    """Place mark sequences longest first; one already contained in the records reuses them.

    Ties keep first-appearance order, so output is deterministic and matches the
    browser generator byte for byte.
    """
    records, first, windows = [], {}, {}
    for marks in sorted(sequences, key=lambda m: (-len(m), sequences[m])):
        start = windows.get(marks)
        if start is None:
            start = len(records)
            records.extend(marks)
            for end in range(start, len(records)):
                for begin in range(max(0, end - MAX_MARKS + 1), end + 1):
                    windows.setdefault(tuple(records[begin:end + 1]), begin)
        first[marks] = start
    return records, first


def serialize_style(recipes, ascender, descender, advance_y):
    if len(recipes) != KEY_COUNT:
        raise ValueError(f"Expected {KEY_COUNT} dense keys")
    checked(ascender, -32768, 32767, "ascender")
    checked(descender, -32768, 32767, "descender")
    checked(advance_y, 1, 255, "CPFont line advance")
    bases, base_ids, sequences, entries = [], {}, {}, []
    for recipe in recipes:
        if recipe is None:
            entries.append(None)
            continue
        checked(len(recipe), 1, MAX_MARKS + 1, "recipe glyph count")
        encoded = []
        for cp, advance, x, y in recipe:
            checked(cp, 0, 65535, "CPFont codepoint")
            checked(advance, 0, 65535, "advanceFP")
            checked(x, -32768, 32767, "xOffsetFP")
            checked(y, -32768, 32767, "yOffsetFP")
            encoded.append(RECORD.pack(cp, advance, x, y))
        base, marks = encoded[0], tuple(encoded[1:])
        if base not in base_ids:
            base_ids[base] = len(bases)
            bases.append(base)
        if marks:
            sequences.setdefault(marks, len(sequences))
        entries.append((base_ids[base], marks))
    checked(len(bases), 1, MAX_BASES, "base count")
    records, first = pack_marks(sequences)
    checked(len(records), 0, MAX_RECORDS, "mark record count")
    dense = bytearray()
    for entry in entries:
        if entry is None:
            value = ABSENT_ENTRY
        else:
            base_id, marks = entry
            value = base_id | len(marks) << 7 | (first[marks] if marks else 0) << 10
        dense += value.to_bytes(ENTRY_BYTES, "little")
    payload = (STYLE_HEADER.pack(len(bases), len(records), ascender, descender, advance_y, 0) +
               dense + b"".join(bases) + b"".join(records))
    checked(len(payload), 0, STYLE_LIMIT, "style companion bytes")
    return payload


def decode_style(payload):
    """Strict independent byte reader used for the baker's serialization round trip."""
    dense = STYLE_HEADER.size
    if not dense + KEY_COUNT * ENTRY_BYTES <= len(payload) <= STYLE_LIMIT:
        raise ValueError("Invalid style payload size")
    nb, nr, asc, desc, line, reserved = STYLE_HEADER.unpack_from(payload)
    base = dense + KEY_COUNT * ENTRY_BYTES
    marks = base + nb * RECORD.size
    if (reserved or not 1 <= nb <= MAX_BASES or nr > MAX_RECORDS or not line or
            len(payload) != marks + nr * RECORD.size):
        raise ValueError("Invalid style section bounds")
    result = []
    for key in range(KEY_COUNT):
        value = int.from_bytes(payload[dense + key * ENTRY_BYTES:dense + (key + 1) * ENTRY_BYTES], "little")
        if value == ABSENT_ENTRY:
            result.append(None)
            continue
        bid, count, start = value & 0x7F, value >> 7 & 7, value >> 10
        if bid >= nb or count > MAX_MARKS or start + count > nr or (not count and start):
            raise ValueError("Invalid recipe entry")
        result.append((RECORD.unpack_from(payload, base + bid * RECORD.size),) +
                      tuple(RECORD.unpack_from(payload, marks + (start + i) * RECORD.size)
                            for i in range(count)))
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
    nb, nr = struct.unpack_from("<HH", model.payload)
    model.counts = {"dense_keys": len(model.recipes), "base_records": nb,
                    "mark_records": nr, "payload_bytes": len(model.payload),
                    "alternate_glyphs": len(model.alternates)}
    return raster._replace(ascender=asc, descender=desc, advanceY=line)


def make_companion(header, toc, packed_sections, models):
    """CRC the CPFont metadata in exact file order, excluding only bitmap sections.

    The CRC (with the pair-tag field zeroed) becomes the pair tag: paired_header()
    writes it into the CPFont header, and the reader compares the two fields
    instead of re-reading CPFont metadata.
    """
    if not 1 <= len(models) <= 4 or set(models) != set(packed_sections):
        raise ValueError("Invalid companion styles")
    cpfont_bytes = len(header) + len(toc)
    unpaired = header[:PAIR_TAG_OFFSET] + bytes(4) + header[PAIR_TAG_OFFSET + 4:]
    metrics_crc = zlib.crc32(toc, zlib.crc32(unpaired))
    for style in sorted(packed_sections):
        for section in packed_sections[style][:-1]:
            metrics_crc = zlib.crc32(section, metrics_crc)
        cpfont_bytes += sum(map(len, packed_sections[style]))
    return assemble_companion(cpfont_bytes, metrics_crc,
                              {style: model.payload for style, model in models.items()})


def assemble_companion(cpfont_bytes, metrics_crc, payloads):
    """Family header, style TOC and style payloads (style ID -> serialized style)."""
    checked(cpfont_bytes, 0, 0xFFFFFFFF, "CPFont file bytes")
    if not 1 <= len(payloads) <= 4:
        raise ValueError("Invalid companion styles")
    toc_bytes = bytearray()
    offset = 32 + 12 * len(payloads)
    for style, payload in sorted(payloads.items()):
        checked(style, 0, 3, "style ID")
        checked(len(payload), STYLE_HEADER.size + KEY_COUNT * ENTRY_BYTES, STYLE_LIMIT,
                "style companion bytes")
        toc_bytes += struct.pack("<B3xII", style, offset, len(payload))
        offset += len(payload)
    checked(offset, 32, FAMILY_LIMIT, "family companion bytes")
    payload = bytes(toc_bytes) + b"".join(payload for _, payload in sorted(payloads.items()))
    return struct.pack("<8sHHIIIII", MAGIC, VERSION, 32, cpfont_bytes,
                       metrics_crc, zlib.crc32(payload), offset, len(payloads)) + payload


def paired_header(header, companion):
    """CPFont header carrying the companion's pair tag in its reserved bytes."""
    return header[:PAIR_TAG_OFFSET] + companion[16:20] + header[PAIR_TAG_OFFSET + 4:]


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
        entry["fallback_sha256s"] = [
            hashlib.sha256(Path(fontfile).read_bytes()).hexdigest()
            for fontfile in fallbacks.get(style, ())]
        report["styles"][str(style)] = entry
    Path(path).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
