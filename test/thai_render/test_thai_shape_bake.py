#!/usr/bin/env python3
"""Offline baker proof with explicit *native* prepared assets; no font downloads.

python test/thai_render/test_thai_shape_bake.py --assets build/thai/assets
The manifest must retain the saved pre-shaping CPFont hashes and frozen faces.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "lib/EpdFont/scripts"))
import fontconvert_sdcard as converter
import thai_shape as baker

ASSETS = None


def cpfont_sections(data):
    """Read actual v4 file offsets independently of converter section objects."""
    if data[:8] != b"CPFONT\0\0" or struct.unpack_from("<H", data, 8)[0] != 4:
        raise ValueError("Expected real CPFont v4")
    count = data[12]
    crc = zlib.crc32(data[:32 + count * 32])
    styles = {}
    for index in range(count):
        fields = struct.unpack_from("<B3xIIBhhHHBBBI4x", data, 32 + index * 32)
        sid, ni, ng, line, asc, desc, nl, nr, nlc, nrc, nlig, start = fields
        end = start + ni * 12 + ng * 16 + (nl + nr) * 3 + nlc * nrc + nlig * 8
        crc = zlib.crc32(data[start:end], crc)
        glyphs = {}
        for interval in range(ni):
            first, last, offset = struct.unpack_from("<III", data, start + interval * 12)
            for cp in range(first, last + 1):
                record = struct.unpack_from("<BBHhhH2xI", data,
                                            start + ni * 12 + (offset + cp - first) * 16)
                width, height, advance, left, top, length, bitmap_offset = record
                glyphs[cp] = ((width, height, advance, left, top),
                              data[end + bitmap_offset:end + bitmap_offset + length])
        styles[sid] = (glyphs, (asc, desc, line))
    return crc, styles


def oracle(font_path, size):
    import freetype
    import uharfbuzz as hb
    face = freetype.Face(str(font_path))
    face.set_char_size(size << 6, size << 6, 150, 150)
    font = hb.Font(hb.Face(font_path.read_bytes()))
    hb.ot_font_set_funcs(font)
    scale = round(size * 150 / 72 * 64)
    font.scale = (scale, scale)
    font.ppem = (face.size.x_ppem, face.size.y_ppem)
    result = []
    # Deliberately enumerate the format grammar independently of contexts().
    for base in range(0xE01, 0xE2F):
        strings = [chr(base) + first + terminal
                   for first in ["", "\u0e31", *map(chr, range(0xE34, 0xE3B)), "\u0e47", "\u0e4d"]
                   for terminal in ["", *map(chr, range(0xE48, 0xE4D)), "\u0e4e"]]
        strings += [chr(base) + tone + "\u0e33" for tone in ["", *map(chr, range(0xE48, 0xE4C))]]
        for text in strings:
            buffer = hb.Buffer()
            buffer.add_str(text)
            buffer.direction, buffer.script, buffer.language = "ltr", "thai", "th"
            hb.shape(font, buffer)
            recipe = []
            for info, pos in zip(buffer.glyph_infos, buffer.glyph_positions):
                if pos.y_advance != 0:
                    raise AssertionError("Oracle has vertical advance")
                recipe.append((info.codepoint, (pos.x_advance + 2) // 4,
                               (pos.x_offset + 2) // 4, (pos.y_offset + 2) // 4))
            result.append(tuple(recipe))
    return result


class BakerBoundsTest(unittest.TestCase):
    def test_pua_assignment_skips_every_real_cmap_entry(self):
        mapping, extra = baker.allocate_codepoints([9, 3, 8, 3],
                                                    {0xE000: 12, 0xE002: 13, 0xE01: 3},
                                                    {0xE003})
        self.assertEqual(mapping, {3: 0xE01, 8: 0xE001, 9: 0xE004})
        self.assertEqual(extra, {0xE001: 8, 0xE004: 9})
        with self.assertRaisesRegex(ValueError, "exhaust"):
            baker.allocate_codepoints([1], {}, range(0xE000, 0xF900))
        with self.assertRaisesRegex(ValueError, "missing glyph"):
            baker.allocate_codepoints([0], {})

    def test_unsupported_dense_key_preserves_neighbors(self):
        records = [((0xE01, 100, -32, 17),)] * baker.KEY_COUNT
        records[81] = None
        decoded, metrics = baker.decode_style(baker.serialize_style(records, 42, -10, 52))
        self.assertEqual(decoded, records)
        self.assertEqual(metrics, (42, -10, 52))

    def test_rejects_unrepresentable_metrics_and_repertoire(self):
        for record in ((0x10000, 1, 0, 0), (0xE01, -1, 0, 0),
                       (0xE01, 65536, 0, 0), (0xE01, 1, -32769, 0),
                       (0xE01, 1, 0, 32768)):
            with self.subTest(record=record), self.assertRaises(ValueError):
                baker.serialize_style([(record,)] * baker.KEY_COUNT, 30, -10, 40)
        for length in (0, 7):
            with self.subTest(length=length), self.assertRaises(ValueError):
                baker.serialize_style([((0xE01, 1, 0, 0),) * length] * baker.KEY_COUNT, 30, -10, 40)
        with self.assertRaisesRegex(ValueError, "style companion bytes"):
            baker.serialize_style([tuple((0xE01, key, slot, 0) for slot in range(6))
                                   for key in range(baker.KEY_COUNT)], 30, -10, 40)
        with self.assertRaisesRegex(ValueError, "line advance"):
            baker.serialize_style([((0xE01, 100, 0, 0),)] * baker.KEY_COUNT, 300, 0, 300)
        models = {sid: baker.BakedStyle([], {}, {}, [], bytes(baker.STYLE_LIMIT)) for sid in range(4)}
        with self.assertRaisesRegex(ValueError, "family companion bytes"):
            baker.make_companion(bytes(32), bytes(128), {sid: [b""] * 7 for sid in models}, models)

    def test_corrupt_offsets_and_counts_fail_closed(self):
        valid = baker.serialize_style([((0xE01, 100, 0, 0),)] * baker.KEY_COUNT, 30, -10, 40)
        for offset, encoded in ((4, struct.pack("<I", 0xFFFFFFFF)),
                                (28, struct.pack("<HH", 0xFFFF, 0)),
                                (len(valid) - 1, b"\x06")):
            broken = bytearray(valid)
            broken[offset:offset + len(encoded)] = encoded
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                baker.decode_style(broken)
        with self.assertRaises(ValueError):
            baker.decode_style(valid[:-1])

    def test_native_converter_import_does_not_load_harfbuzz(self):
        command = "import fontconvert_sdcard,sys; assert 'uharfbuzz' not in sys.modules"
        subprocess.run([sys.executable, "-c", command], cwd=ROOT / "lib/EpdFont/scripts", check=True)


class FallbackChainTest(unittest.TestCase):
    """Synthetic outlines keep fallback precedence independent of installed fonts."""

    @staticmethod
    def make_font(path, codepoints, width, thai_alternate=False):
        # Match the converter's load order: macOS wheels bundle incompatible
        # HarfBuzz symbols, and feaLib imports uharfbuzz transitively.
        import freetype
        from fontTools.fontBuilder import FontBuilder
        from fontTools.pens.ttGlyphPen import TTGlyphPen
        from fontTools.feaLib.builder import addOpenTypeFeaturesFromString

        cmap = {cp: f"uni{cp:04X}" for cp in codepoints}
        names = [".notdef", *cmap.values()]
        if thai_alternate:
            names.append("thai.alt")
        builder = FontBuilder(1000, isTTF=True)
        builder.setupGlyphOrder(names)
        builder.setupCharacterMap(cmap)
        glyphs = {}
        for name in names:
            pen = TTGlyphPen(None)
            pen.moveTo((0, 0))
            pen.lineTo((width, 0))
            pen.lineTo((width, 600))
            pen.lineTo((0, 600))
            pen.closePath()
            glyphs[name] = pen.glyph()
        builder.setupGlyf(glyphs)
        builder.setupHorizontalMetrics({name: (width + 100, 0) for name in names})
        builder.setupHorizontalHeader(ascent=800, descent=-200)
        builder.setupNameTable({"familyName": path.stem, "styleName": "Regular",
                                "fullName": path.stem, "psName": path.stem})
        builder.setupOS2(sTypoAscender=800, sTypoDescender=-200,
                         usWinAscent=800, usWinDescent=200)
        builder.setupPost()
        builder.setupMaxp()
        if thai_alternate:
            addOpenTypeFeaturesFromString(builder.font,
                "languagesystem thai dflt; feature ccmp { sub uni0E01 by thai.alt; } ccmp;")
        builder.font["head"].created = builder.font["head"].modified = 3400000000
        builder.save(path)

    def test_cli_primary_and_ordered_fallback_rasters(self):
        with tempfile.TemporaryDirectory(prefix="fallback-chain-") as temp:
            root = Path(temp)
            fonts = [root / f"face{index}.ttf" for index in range(3)]
            for path, codepoints, width in zip(fonts, ([65], [65, 66], [65, 66, 67]),
                                               (200, 400, 600)):
                self.make_font(path, codepoints, width)
            references = []
            for index, font in enumerate(fonts):
                output = root / f"reference{index}.cpfont"
                converter.generate_cpfont_multistyle({0: str(font)}, 16, [(65, 67)], str(output))
                references.append(cpfont_sections(output.read_bytes())[1][0])
            self.assertNotEqual(references[0][0][65], references[1][0][65])
            self.assertNotEqual(references[1][0][66], references[2][0][66])
            for fallbacks in ([fonts[1]], fonts[1:], [fonts[2], fonts[1]]):
                with self.subTest(fallbacks=fallbacks):
                    output = root / "chain.cpfont"
                    command = [sys.executable, str(ROOT / "lib/EpdFont/scripts/fontconvert_sdcard.py"),
                               "--regular", str(fonts[0]), "--intervals", "ascii",
                               "--size", "16", "--output", str(output)]
                    for font in fallbacks:
                        command.extend(["--fallback-regular", str(font)])
                    subprocess.run(command, check=True, capture_output=True)
                    glyphs, metrics = cpfont_sections(output.read_bytes())[1][0]
                    self.assertEqual(metrics, references[0][1])
                    self.assertEqual(glyphs[65], references[0][0][65])
                    first = fonts.index(fallbacks[0])
                    self.assertEqual(glyphs[66], references[first][0][66])
                    if fonts[2] in fallbacks:
                        self.assertEqual(glyphs[67], references[2][0][67])
                    else:
                        self.assertNotIn(67, glyphs)

    def test_later_fallback_reserves_pua_and_records_provenance(self):
        with tempfile.TemporaryDirectory(prefix="fallback-shaping-") as temp:
            root = Path(temp)
            primary, first, second = [root / f"{name}.ttf" for name in ("thai", "first", "second")]
            self.make_font(primary, range(0xE01, 0xE5C), 200, thai_alternate=True)
            self.make_font(first, [0xE000], 400)
            self.make_font(second, [0xE001], 600)
            native = baker.bake_style(primary, 16)
            model = baker.bake_style(primary, 16, [first, second])
            self.assertEqual(set(native.alternates), {0xE000})
            self.assertEqual(set(model.alternates), {0xE002})
            self.assertEqual(model.oracle, native.oracle)
            output = root / "shaped.cpfont"
            converter.generate_cpfont_multistyle(
                {0: str(primary)}, 16, [(0xE01, 0xE5B), (0xE000, 0xE002)], str(output),
                fallback_style_fonts={0: [str(first), str(second)]}, thai_shaping=True)
            glyphs, _ = cpfont_sections(output.read_bytes())[1][0]
            for cp, font in ((0xE000, first), (0xE001, second)):
                reference = root / "reference.cpfont"
                converter.generate_cpfont_multistyle({0: str(font)}, 16, [(cp, cp)], str(reference))
                self.assertEqual(glyphs[cp], cpfont_sections(reference.read_bytes())[1][0][0][cp])
            report = json.loads(output.with_suffix(".cpshape.json").read_text(encoding="utf-8"))
            self.assertEqual(report["styles"]["0"]["fallback_sha256s"],
                             [hashlib.sha256(font.read_bytes()).hexdigest() for font in (first, second)])
            self.assertNotIn("fallback_sha256", report["styles"]["0"])


class RealFontBakerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if ASSETS is None:
            raise RuntimeError("Pass --assets with a saved native asset manifest")
        cls.manifest = json.loads((ASSETS / "manifest.json").read_text(encoding="utf-8"))
        cls.fonts = [font for font in cls.manifest["fonts"]
                     if font["family"] in ("noto-sans-thai", "noto-serif-thai", "sarabun")]
        if len(cls.fonts) != 12:
            raise RuntimeError("Expected all three Thai families at 12/14/16/18 pt")
        cls.fallback = ASSETS / cls.manifest["fallback"]["path"]
        for font in cls.fonts:
            for key in ("regular", "cpfont"):
                path = ASSETS / font[key]
                if hashlib.sha256(path.read_bytes()).hexdigest() != font[key + "_sha256"]:
                    raise RuntimeError(f"Saved asset hash mismatch: {path}")
        if hashlib.sha256(cls.fallback.read_bytes()).hexdigest() != cls.manifest["fallback"]["sha256"]:
            raise RuntimeError("Fallback font hash mismatch")

    def assert_alternate_bitmaps(self, font, size, mapping, glyphs):
        import freetype
        face = freetype.Face(str(font))
        face.set_char_size(size << 6, size << 6, 150, 150)
        for item in mapping:
            if not item["alternate"]:
                continue
            cp, gid = item["codepoint"], item["glyph_id"]
            self.assertEqual(face.get_char_index(cp), 0)
            face.load_glyph(gid, freetype.FT_LOAD_RENDER)
            bitmap = face.glyph.bitmap
            raw = bitmap.buffer
            pixels = []
            for row in range(bitmap.rows):
                start = (row if bitmap.pitch >= 0 else bitmap.rows - 1 - row) * abs(bitmap.pitch)
                pixels.extend(raw[start + col] // 64 for col in range(bitmap.width))
            packed = bytearray()
            for offset in range(0, len(pixels), 4):
                value = 0
                for index in range(4):
                    value = (value << 2) | (pixels[offset + index] if offset + index < len(pixels) else 0)
                packed.append(value)
            metadata, bits = glyphs[cp]
            self.assertEqual(metadata, (bitmap.width, bitmap.rows,
                                        (face.glyph.linearHoriAdvance + 2048) >> 12,
                                        face.glyph.bitmap_left, face.glyph.bitmap_top))
            self.assertEqual(bits, bytes(packed))

    def check_pair(self, output, sources, size):
        cpfont = output.read_bytes()
        companion = output.with_suffix(".cpshape").read_bytes()
        magic, version, header_size, font_bytes, crc, payload_crc, total, count = struct.unpack_from(
            "<8sHHIIIII", companion)
        self.assertEqual((magic, version, header_size, font_bytes, total, count),
                         (b"CPSHAPE\0", 1, 32, len(cpfont), len(companion), len(sources)))
        self.assertLessEqual(total, baker.FAMILY_LIMIT)
        computed_crc, styles = cpfont_sections(cpfont)
        self.assertEqual(crc, computed_crc)
        self.assertEqual(payload_crc, zlib.crc32(companion[32:]))
        report = json.loads(output.with_suffix(".cpshape.json").read_text(encoding="utf-8"))
        for index, (sid, source) in enumerate(sorted(sources.items())):
            style, reserved, start, size_bytes = struct.unpack_from("<B3sII", companion, 32 + index * 12)
            self.assertEqual((style, reserved), (sid, bytes(3)))
            self.assertLessEqual(size_bytes, baker.STYLE_LIMIT)
            recipes, metrics = baker.decode_style(companion[start:start + size_bytes])
            entry = report["styles"][str(sid)]
            reverse = {item["codepoint"]: item["glyph_id"] for item in entry["glyph_mapping"]}
            decoded = [tuple((reverse[cp], advance, x, y) for cp, advance, x, y in recipe)
                       for recipe in recipes]
            expected = oracle(source, size)
            self.assertEqual(decoded, expected)
            model = baker.bake_style(source, size, [self.fallback])
            self.assertEqual(model.oracle, expected)
            glyphs, cp_metrics = styles[sid]
            self.assertEqual(metrics, cp_metrics)
            self.assert_alternate_bitmaps(source, size, entry["glyph_mapping"], glyphs)
            for recipe in recipes:
                for cp, advance, x, y in recipe:
                    (width, height, _, _, top), _ = glyphs[cp]
                    if width and height:
                        ink_top = top + (y + 8) // 16
                        self.assertGreaterEqual(metrics[0], ink_top)
                        self.assertLessEqual(metrics[1], ink_top - height)
        return styles

    def test_all_fonts_native_identity_and_every_baked_key(self):
        intervals = converter.resolve_intervals(self.manifest["intervals"])
        with tempfile.TemporaryDirectory(prefix="thai-bake-") as temp:
            for fixture in self.fonts:
                with self.subTest(font=fixture["id"]):
                    source = ASSETS / fixture["regular"]
                    size = fixture["point_size"]
                    output = Path(temp) / (fixture["id"] + ".cpfont")
                    kwargs = dict(fallback_style_fonts={0: [str(self.fallback)]})
                    converter.generate_cpfont_multistyle({0: str(source)}, size, intervals, str(output), **kwargs)
                    native = output.read_bytes()
                    self.assertEqual(hashlib.sha256(native).hexdigest(), fixture["cpfont_sha256"])
                    self.assertFalse(output.with_suffix(".cpshape").exists())
                    _, native_styles = cpfont_sections(native)
                    converter.generate_cpfont_multistyle({0: str(source)}, size, intervals, str(output),
                                                        thai_shaping=True, **kwargs)
                    shaped_styles = self.check_pair(output, {0: source}, size)
                    # Every old Unicode entry retains its exact metrics and bitmap.
                    for cp, record in native_styles[0][0].items():
                        self.assertEqual(shaped_styles[0][0][cp], record)

    def test_pack_covers_ipa_arrows_symbols_and_arabic_forms(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        from package_thai_fonts import INTERVALS, load_sources

        sources, fallbacks = load_sources(ASSETS)
        paths = [str(ASSETS / record["path"]) for record in fallbacks]
        required = set(range(0x0250, 0x0300)) | set(range(0x2190, 0x2300))
        required.update(map(ord, "æðŋβθχ★☆✓✗♪♫Ⅳمرحبا١٢٣َُِّ"))
        # The reader emits contextual forms and collapsed Lam-Alef ligatures.
        required.update((0xFE8F, 0xFE91, 0xFE96, 0xFEF4, 0xFEDB, 0xFE98,
                         0xFE8E, 0xFEF5, 0xFEF6, 0xFEF7, 0xFEF8,
                         0xFEF9, 0xFEFA, 0xFEFB, 0xFEFC))
        with tempfile.TemporaryDirectory(prefix="thai-pack-coverage-") as temp:
            for source in sources:
                with self.subTest(family=source["family"]):
                    output = Path(temp) / (source["family"] + ".cpfont")
                    converter.generate_cpfont_multistyle(
                        {0: str(ASSETS / source["regular"])}, 16,
                        converter.resolve_intervals(INTERVALS), str(output),
                        fallback_style_fonts={0: paths}, thai_shaping=True)
                    _, styles = cpfont_sections(output.read_bytes())
                    missing = required - styles[0][0].keys()
                    self.assertEqual(sorted(missing), [], "Missing packaged Unicode glyphs")

    def test_multistyle_deterministic_pair(self):
        fixture = self.fonts[0]
        source = ASSETS / fixture["regular"]
        sources = {sid: str(source) for sid in range(4)}
        intervals = converter.resolve_intervals("ascii,punctuation,thai")
        with tempfile.TemporaryDirectory(prefix="thai-multistyle-") as temp:
            outputs = [Path(temp) / (name + ".cpfont") for name in ("first", "second")]
            for output in outputs:
                converter.generate_cpfont_multistyle(sources, 16, intervals, str(output),
                                                    fallback_style_fonts={sid: [str(self.fallback)] for sid in sources},
                                                    thai_shaping=True)
            self.assertEqual(outputs[0].read_bytes(), outputs[1].read_bytes())
            self.assertEqual(outputs[0].with_suffix(".cpshape").read_bytes(),
                             outputs[1].with_suffix(".cpshape").read_bytes())
            self.check_pair(outputs[0], {sid: source for sid in sources}, 16)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    ASSETS = args.assets.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
