#!/usr/bin/env python3
"""Native CPFont conversion/packaging regressions; no shaping companion is required.

python test/thai_render/test_native_cpfont.py [--assets build/thai/assets]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "lib/EpdFont/scripts"))
sys.path.insert(0, str(ROOT / "scripts"))
import fontconvert_sdcard as converter
from package_thai_fonts import validate_cpfont
from prepare_thai_test_assets import FALLBACK

ASSETS: Path | None = None


def styles(data: bytes) -> dict:
    """Decode native Unicode records and layout sections without a recipe map."""
    result = {}
    for index in range(data[12]):
        (style, intervals, count, line, asc, desc, nl, nr, lc, rc, ligs, offset) = struct.unpack_from(
            "<B3xIIBhhHHBBBI4x", data, 32 + index * 32)
        glyph_start = offset + intervals * 12
        layout_start = glyph_start + count * 16
        bitmap_start = layout_start + (nl + nr) * 3 + lc * rc + ligs * 8
        glyphs = {}
        for interval in range(intervals):
            first, last, start = struct.unpack_from("<III", data, offset + interval * 12)
            for cp in range(first, last + 1):
                record = struct.unpack_from("<BBHhhH2xI", data, glyph_start + (start + cp - first) * 16)
                width, height, advance, left, top, length, bitmap_offset = record
                glyphs[cp] = (width, height, advance, left, top,
                              data[bitmap_start + bitmap_offset:bitmap_start + bitmap_offset + length])
        result[style] = {"glyphs": glyphs, "metrics": (line, asc, desc),
                         "layout": data[layout_start:bitmap_start], "ligatures": ligs}
    return result


class NativeConversionTest(unittest.TestCase):
    def test_regular_conversion_preserves_unicode_and_layout(self):
        intervals = [(0x20, 0x7E), (0xFB00, 0xFB06), (0xFFFD, 0xFFFD)]
        raster = converter.rasterize_font_style(str(FALLBACK), 16, intervals)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "native.cpfont"
            converter.generate_cpfont_multistyle({0: str(FALLBACK)}, 16, intervals, str(output))
            self.assertEqual([output], list(Path(directory).iterdir()))
            validate_cpfont(output)
            data = output.read_bytes()
            self.assertEqual(bytes(19), data[13:32])
            decoded = styles(data)[0]
            expected = {glyph.code_point: (glyph.width, glyph.height, glyph.advance_x,
                                           glyph.left, glyph.top, bitmap)
                        for glyph, bitmap in raster.all_glyphs}
            self.assertEqual(expected, decoded["glyphs"])
            self.assertIn(ord("A"), decoded["glyphs"])
            sections = converter.pack_style_sections(raster)
            self.assertEqual(b"".join(sections[2:6]), decoded["layout"])
            self.assertEqual(len(raster.ligature_pairs), decoded["ligatures"])

    def test_multistyle_native_fonts_remain_independent(self):
        intervals = [(0x41, 0x5A)]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "styles.cpfont"
            converter.generate_cpfont_multistyle({2: str(FALLBACK), 0: str(FALLBACK)},
                                                12, intervals, str(output))
            decoded = styles(output.read_bytes())
            self.assertEqual({0, 2}, set(decoded))
            self.assertEqual(decoded[0], decoded[2])
    def test_native_fallbacks_keep_primary_and_first_fallback_priority(self):
        from fontTools.ttLib import TTFont
        primary = FALLBACK.parent.parent / "NotoSansHebrew/NotoSansHebrew-Regular.ttf"
        second = FALLBACK.parent.parent / "NotoSerif/NotoSerif-Regular.ttf"
        def cmap(path):
            with TTFont(path) as font:
                return set(font.getBestCmap())
        primary_cps, first_cps, second_cps = cmap(primary), cmap(FALLBACK), cmap(second)
        fallback_cp = min(cp for cp in first_cps & second_cps - primary_cps if cp > 0x20)
        primary_cp = min(cp for cp in primary_cps & first_cps & second_cps if cp > 0x20)
        intervals = sorted((cp, cp) for cp in (fallback_cp, primary_cp))
        def records(font, fallbacks=()):
            raster = converter.rasterize_font_style(str(font), 16, intervals,
                                                   fallback_fontfiles=list(map(str, fallbacks)))
            return {glyph.code_point: (glyph, bitmap) for glyph, bitmap in raster.all_glyphs}
        native, first, last = records(primary), records(FALLBACK), records(second)
        combined = records(primary, (FALLBACK, second))
        reverse = records(primary, (second, FALLBACK))
        # Offsets are aggregate-layout details; compare glyph metrics and pixels only.
        def visible(record):
            glyph, bitmap = record
            return glyph.width, glyph.height, glyph.advance_x, glyph.left, glyph.top, bitmap
        for actual in (combined, reverse):
            self.assertEqual(visible(native[primary_cp]), visible(actual[primary_cp]))
        self.assertEqual(visible(first[fallback_cp]), visible(combined[fallback_cp]))
        self.assertEqual(visible(last[fallback_cp]), visible(reverse[fallback_cp]))


    def test_packager_rejects_damaged_native_sections(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "native.cpfont"
            converter.generate_cpfont_multistyle({0: str(FALLBACK)}, 16, [(0x41, 0x5A)], str(output))
            original = output.read_bytes()
            damaged = bytearray(original)
            struct.pack_into("<I", damaged, 64 + 8, 1)  # First interval must begin at glyph zero.
            for data in (original[:63], original[:-1], original + b"\0", bytes(damaged)):
                with self.subTest(length=len(data)):
                    output.write_bytes(data)
                    with self.assertRaises(ValueError):
                        validate_cpfont(output)


@unittest.skipIf(ASSETS is None, "Pass --assets for pinned Thai source coverage")
class PreparedThaiConversionTest(unittest.TestCase):
    def test_thai_fonts_preserve_native_glyphs_and_ordered_fallbacks(self):
        manifest = json.loads((ASSETS / "manifest.json").read_text(encoding="utf-8"))
        families = {entry["family"]: entry for entry in manifest["fonts"]}
        fallback = ASSETS / manifest["fallback"]["path"]
        for family, entry in families.items():
            with self.subTest(family=family), tempfile.TemporaryDirectory() as directory:
                source = ASSETS / entry["regular"]
                intervals = [(0x20, 0x7E), (0xE00, 0xE7F)]
                output = Path(directory) / "thai.cpfont"
                kwargs = {"fallback_style_fonts": {0: [str(fallback)]}}
                converter.generate_cpfont_multistyle({0: str(source)}, 16, intervals, str(output), **kwargs)
                validate_cpfont(output)
                self.assertEqual([output], list(Path(directory).iterdir()))
                decoded = styles(output.read_bytes())[0]["glyphs"]
                raster = converter.rasterize_font_style(str(source), 16, intervals,
                                                       fallback_fontfiles=[str(fallback)])
                expected = {glyph.code_point: (glyph.width, glyph.height, glyph.advance_x,
                                               glyph.left, glyph.top, bitmap)
                            for glyph, bitmap in raster.all_glyphs}
                self.assertEqual(expected, decoded)
                self.assertTrue(all(cp in decoded for cp in (0xE01, 0xE31, 0xE32, 0xE48, ord("A"))))
                self.assertFalse(any(0xE000 <= cp <= 0xF8FF for cp in decoded))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path)
    args, remaining = parser.parse_known_args()
    ASSETS = args.assets.resolve() if args.assets else None
    # unittest decorators run before command-line parsing.
    PreparedThaiConversionTest.__unittest_skip__ = ASSETS is None
    unittest.main(argv=[sys.argv[0], *remaining])
