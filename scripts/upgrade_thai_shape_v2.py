#!/usr/bin/env python3
"""Upgrade CPFont + CPSHAPE v1 pairs to CPSHAPE v2 without re-baking.

Each v1 companion is checked against its CPFont, its recipes are re-serialized
by the current baker, and the CPFont header gains the pair tag. Glyph bitmaps,
metrics and recipes are unchanged. Needs no desktop shaping libraries.

    python scripts/upgrade_thai_shape_v2.py SOURCE_DIR OUTPUT_DIR

Every *.cpfont with a same-basename *.cpshape under SOURCE_DIR is written to the
same relative path under OUTPUT_DIR. Other files, including .cpshape.json reports,
are not copied or updated, so upgraded pairs cannot pass package_thai_fonts.py;
re-bake from sources for a packaged release. A failure stops the run and may leave
earlier pairs in OUTPUT_DIR. Copy both files of every pair to the SD card.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "lib/EpdFont/scripts"))
import thai_shape  # noqa: E402

V1_STYLE_HEADER = struct.Struct("<HHIIIIhhHH")
CPFONT_TOC = struct.Struct("<B3xIIBhhHHBBBI4x")
# On-disk element sizes, as SdCardFont::computeStyleFileOffsets reads them.
INTERVAL, GLYPH, KERN_CLASS, LIGATURE = 12, 16, 3, 8


def metrics_crc(font: bytes) -> int:
    """CRC of CPFont header (pair tag zeroed), TOC and per-style metadata, excluding bitmaps."""
    if len(font) < 32 or font[:8] != b"CPFONT\0\0" or struct.unpack_from("<H", font, 8)[0] != 4:
        raise ValueError("Expected CPFont v4")
    styles = font[12]
    toc_end = 32 + styles * CPFONT_TOC.size
    tag = thai_shape.PAIR_TAG_OFFSET
    crc = zlib.crc32(font[32:toc_end], zlib.crc32(font[:tag] + bytes(4) + font[tag + 4:32]))
    ranges = []
    for i in range(styles):
        (_, intervals, glyphs, _, _, _, kern_left, kern_right, left_classes, right_classes, ligatures,
         offset) = CPFONT_TOC.unpack_from(font, 32 + i * CPFONT_TOC.size)
        length = (intervals * INTERVAL + glyphs * GLYPH + (kern_left + kern_right) * KERN_CLASS +
                  left_classes * right_classes + ligatures * LIGATURE)
        ranges.append((offset, length))
    for offset, length in sorted(ranges):
        if offset + length > len(font):
            raise ValueError("Truncated CPFont metadata")
        crc = zlib.crc32(font[offset:offset + length], crc)
    return crc


def decode_v1_style(payload: bytes):
    nb, ns, dense, base, table, data, asc, desc, line, reserved = V1_STYLE_HEADER.unpack_from(payload)
    if (reserved or dense != 28 or base != dense + thai_shape.KEY_COUNT * 4 or table != base + nb * 8
            or data != table + (ns + 1) * 4 or data > len(payload)):
        raise ValueError("Invalid v1 style section bounds")
    offsets = struct.unpack_from(f"<{ns + 1}I", payload, table)
    if offsets[0] != data or offsets[-1] != len(payload):
        raise ValueError("Invalid v1 suffix endpoints")
    suffixes = []
    for start, end in zip(offsets, offsets[1:]):
        count = payload[start]
        if not data <= start < end <= len(payload) or count > 5 or end - start != 1 + count * 8:
            raise ValueError("Invalid v1 suffix")
        suffixes.append(tuple(thai_shape.RECORD.unpack_from(payload, start + 1 + i * 8) for i in range(count)))
    recipes = []
    for key in range(thai_shape.KEY_COUNT):
        bid, sid = struct.unpack_from("<HH", payload, dense + key * 4)
        if bid == sid == 0xFFFF:
            recipes.append(None)
        elif bid >= nb or sid >= ns:
            raise ValueError("Invalid v1 recipe index")
        else:
            recipes.append((thai_shape.RECORD.unpack_from(payload, base + bid * 8),) + suffixes[sid])
    return recipes, (asc, desc, line)


def upgrade_pair(font: bytes, shape: bytes) -> tuple[bytes, bytes]:
    magic, version, header, font_bytes, crc, payload_crc, total, styles = struct.unpack_from("<8sHHIIIII", shape)
    if magic != b"CPSHAPE\0" or version != 1 or header != 32 or total != len(shape) or styles != font[12]:
        raise ValueError("Not a CPSHAPE v1 companion")
    if font_bytes != len(font) or zlib.crc32(shape[32:]) != payload_crc or metrics_crc(font) != crc:
        raise ValueError("Companion does not match its CPFont")
    payloads = {}
    for i in range(styles):
        style, offset, length = struct.unpack_from("<B3xII", shape, 32 + i * 12)
        recipes, metrics = decode_v1_style(shape[offset:offset + length])
        payload = thai_shape.serialize_style(recipes, *metrics)
        if thai_shape.decode_style(payload) != (recipes, metrics):
            raise ValueError("v2 round trip differs from the v1 recipes")
        payloads[style] = payload
    companion = thai_shape.assemble_companion(len(font), crc, payloads)
    return thai_shape.paired_header(font[:32], companion) + font[32:], companion


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if source == output:
        parser.exit(2, "OUTPUT_DIR must differ from SOURCE_DIR\n")
    pairs = 0
    cpfont = source
    try:
        for cpfont in sorted(source.rglob("*.cpfont")):
            companion = cpfont.with_suffix(".cpshape")
            if not companion.is_file():
                continue
            font, shape = upgrade_pair(cpfont.read_bytes(), companion.read_bytes())
            target = output / cpfont.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(font)
            target.with_suffix(".cpshape").write_bytes(shape)
            print(f"{cpfont.relative_to(source)}: {len(companion.read_bytes())} -> {len(shape)} bytes")
            pairs += 1
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"Upgrade failed at {cpfont}: {error}\n")
    if not pairs:
        parser.exit(1, f"No .cpfont/.cpshape pairs found under {source}\n")
    print(f"Upgraded {pairs} pairs into {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
