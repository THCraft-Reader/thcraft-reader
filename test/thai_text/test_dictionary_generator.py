#!/usr/bin/env python3
"""Offline generator regressions; run directly with Python's unittest runner."""
from __future__ import annotations

from bisect import bisect_right
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("generate_thai_dictionary", ROOT / "scripts/generate_thai_dictionary.py")
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class DictionaryGeneratorTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.txt"
        self.extra = self.root / "extra.txt"
        self.source.write_text("ก\nกข\nก\nLatin\n\nก่\n", encoding="utf-8")
        self.extra.write_text("ประเทศไทย\n", encoding="utf-8")

    def generate(self, name="one"):
        return generator.generate(self.source, self.extra, self.root / name / "data.h", self.root / name / "report.json")

    def test_deterministic_output_and_report(self):
        first = self.generate("one")
        second = self.generate("two")
        self.assertEqual(first, second)
        self.assertEqual((self.root / "one/data.h").read_bytes(), (self.root / "two/data.h").read_bytes())
        self.assertEqual((self.root / "one/report.json").read_bytes(), (self.root / "two/report.json").read_bytes())
        self.assertEqual(first["word_count"], 4)
        self.assertEqual(first["rejected_source_entries"], 2)
        self.assertEqual(first["source"]["duplicates"], 1)
        self.assertEqual(json.loads((self.root / "one/report.json").read_text()), first)
        self.assertEqual(first["flash_bytes"],
                         first["data_bytes"] + first["offset_bytes"] + first["first_symbol_index_bytes"])

    def test_round_trip_blocks_preserve_marks_and_terminal_prefixes(self):
        words = sorted({"ก", "ก่", "กข"} | {"ข" + chr(0x0E01 + i) for i in range(35)})
        data, offsets = generator.encode(words)
        decoded = []
        for block in range(len(offsets) - 1):
            position = offsets[block]
            previous = b""
            while position < offsets[block + 1]:
                prefix, suffix = data[position:position + 2]
                position += 2
                current = previous[:prefix] + data[position:position + suffix]
                position += suffix
                decoded.append("".join(chr(0x0E00 + cp) for cp in current))
                previous = current
            self.assertEqual(position, offsets[block + 1])
        self.assertEqual(decoded, words)
        self.assertEqual(offsets[-1], len(data))
        self.assertEqual(len(offsets), (len(words) + 15) // 16 + 1)

    def test_first_symbol_ranges_include_full_search_predecessor(self):
        # Symbol 3 starts inside block 0; the terminal symbol starts exactly at block 2.
        words = ([chr(0x0E01) + chr(0x0E01 + i) for i in range(15)] +
                 [chr(0x0E03) + chr(0x0E01 + i) for i in range(17)] + [chr(0x0E5B)])
        leaders = words[::generator.BLOCK_WORDS]
        directory = generator.first_symbol_blocks(words)
        self.assertEqual(len(directory), 93)
        self.assertEqual(directory[0], 0)
        self.assertEqual(directory[2], 1)  # absent symbol
        self.assertEqual(directory[3], 1)  # transition inside preceding block
        self.assertEqual(directory[91], 2)  # exact leader transition
        self.assertEqual(directory[92], len(leaders))
        queries = words + [word + "กข" for word in words]
        for symbol in range(1, 92):
            queries.extend([chr(0x0E00 + symbol), chr(0x0E00 + symbol) + "กข"])
        for query in queries:
            with self.subTest(query=query):
                symbol = ord(query[0]) - 0x0E00
                low = max(0, directory[symbol] - 1)
                high = directory[symbol + 1]
                self.assertEqual(bisect_right(leaders, query, low, high) - 1,
                                 bisect_right(leaders, query) - 1)

    def test_empty_first_symbol_ranges(self):
        self.assertEqual(generator.first_symbol_blocks([]), [0] * 93)

    def test_first_symbol_block_capacity_rejects_before_writing(self):
        words = [chr(0x0E01 + i // (91 * 91)) + chr(0x0E01 + i // 91 % 91) +
                 chr(0x0E01 + i % 91) for i in range(65536)]
        self.source.write_text("\n".join(words), encoding="utf-8")
        self.extra.write_bytes(b"")
        with patch.object(generator, "BLOCK_WORDS", 1):
            self.assertEqual(generator.first_symbol_blocks(words[:-1])[-1], 65535)
            with self.assertRaisesRegex(ValueError, "^dictionary block count exceeds uint16 first-symbol index$"):
                self.generate()
        self.assertFalse((self.root / "one/data.h").exists())
        self.assertFalse((self.root / "one/report.json").exists())

    def test_identity_covers_supplement_and_offsets(self):
        report = self.generate()
        source, _, _ = generator.load_words(self.source, local=False)
        extra, _, _ = generator.load_words(self.extra, local=True)
        data, offsets = generator.encode(sorted(source | extra))
        crc = zlib.crc32(data + b"".join(struct.pack("<I", item) for item in offsets))
        self.assertEqual(report["data_crc32"], f"{crc:08x}")
        self.extra.write_text("ประเทศไทย\nข\n", encoding="utf-8")
        self.assertNotEqual(report["data_crc32"], self.generate("changed")["data_crc32"])

    def test_malformed_local_entries_report_line_and_leave_no_output(self):
        for payload, reason in [("ก\nLatin\n".encode(), "non_thai"), ("ก\n\n".encode(), "empty"),
                                ("ก\n".encode() + b"\xff\n", "malformed UTF-8")]:
            with self.subTest(reason=reason):
                self.extra.write_bytes(payload)
                with self.assertRaisesRegex(ValueError, "extra.txt:2:.*" + reason):
                    self.generate()
                self.assertFalse((self.root / "one/data.h").exists())
                self.assertFalse((self.root / "one/report.json").exists())

    def test_overlong_thai_source_and_local_entries_fail_instead_of_truncating(self):
        for local in (False, True):
            with self.subTest(local=local):
                path = self.extra if local else self.source
                original = path.read_bytes()
                path.write_text("ก\n" + "ก" * 71 + "\n", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, r":2: 71 codepoints exceeds 70"):
                    self.generate()
                path.write_bytes(original)

    def test_seventy_codepoints_and_empty_dictionary_are_representable(self):
        self.source.write_text("ก" * 70 + "\n", encoding="utf-8")
        self.extra.write_bytes(b"")
        self.assertEqual(self.generate()["maximum_word_codepoints"], 70)
        self.source.write_bytes(b"")
        report = self.generate("empty")
        self.assertEqual(report["word_count"], 0)
        self.assertEqual(report["data_bytes"], 0)
        self.assertEqual(report["offset_count"], 1)

    def test_malformed_source_utf8_is_not_silently_replaced(self):
        self.source.write_bytes("ก\n".encode() + b"\xe0\xb8")
        with self.assertRaisesRegex(ValueError, "source.txt:2: malformed UTF-8"):
            self.generate()


if __name__ == "__main__":
    unittest.main()
