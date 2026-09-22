#!/usr/bin/env python3
"""Build and embed the full pinned LibThai dictionary without Unix tools.

The Makefile.am TDICT_SRC assignment is the sole word-source list. Input bytes
are not normalized or trimmed; sorting is the bytewise LC_ALL=C sort -u recipe.
The CMake-built host utility performs construction using the pinned libdatrie.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

NATIVE_DIR = Path(__file__).resolve().parents[2] / "NativeText"
HEADER_NAME = "NativeThaiDictionary.generated.h"
SOURCE_NAME = "NativeThaiDictionary.generated.cpp"
OUTPUT_NAMES = ("tdict.txt", "thbrk.tri", HEADER_NAME, SOURCE_NAME, "thbrk.metadata.json")


def dictionary_sources(data_dir):
    """Parse the explicit upstream variable, never glob a different word list."""
    recipe = (data_dir / "Makefile.am").read_text(encoding="ascii")
    assignment = None
    logical = ""
    for physical in recipe.splitlines():
        continued = physical.rstrip().endswith("\\")
        logical += physical.rstrip()[:-1] + " " if continued else physical
        if continued:
            continue
        if re.match(r"^TDICT_SRC\b", logical):
            match = re.fullmatch(r"TDICT_SRC\s*=\s*(.*)", logical)
            if assignment is not None or match is None:
                raise ValueError("expected one explicit TDICT_SRC assignment in data/Makefile.am")
            assignment = match.group(1).split()
        logical = ""
    if logical or not assignment:
        raise ValueError("missing, empty or unterminated TDICT_SRC in data/Makefile.am")
    names = []
    for token in assignment:
        match = re.fullmatch(r"\$\(srcdir\)/(tdict-[A-Za-z0-9-]+\.txt)", token)
        if match is None or match.group(1) in names:
            raise ValueError(f"invalid or duplicate TDICT_SRC member: {token}")
        names.append(match.group(1))
    paths = [data_dir / name for name in names]
    for path in paths:
        if not path.is_file():
            raise ValueError(f"missing TDICT_SRC member: {path}")
    return paths


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def prepare_words(data_dir, paths):
    source_hash = hashlib.sha256()

    def add_source(name, content):
        # Length-prefixed canonical records prevent ambiguous concatenations.
        name_bytes = name.encode("utf-8")
        source_hash.update(len(name_bytes).to_bytes(8, "big"))
        source_hash.update(name_bytes)
        source_hash.update(len(content).to_bytes(8, "big"))
        source_hash.update(content)

    recipe = (data_dir / "Makefile.am").read_bytes()
    alphabet = (data_dir / "thbrk.abm").read_bytes()
    if re.fullmatch(rb"\s*\[0x0e01\s*,\s*0x0e5b\]\s*", alphabet) is None:
        raise ValueError("thbrk.abm must define exactly [0x0e01,0x0e5b]")
    add_source("data/Makefile.am", recipe)
    add_source("data/thbrk.abm", alphabet)
    words = set()
    total_words = 0
    source_files = []
    for path in paths:
        content = path.read_bytes()
        # A missing separator would join two words in upstream's cat pipeline.
        # Reject it, rather than silently changing that recipe or dropping data.
        if not content or not content.endswith(b"\n"):
            raise ValueError(f"empty or non-LF-terminated dictionary source: {path}")
        entries = content[:-1].split(b"\n")
        for line, entry in enumerate(entries, 1):
            text = entry.decode("utf-8", errors="strict")
            if not text or len(text) > 32767 or any(not 0x0E01 <= ord(cp) <= 0x0E5B for cp in text):
                raise ValueError(f"invalid dictionary alphabet or word length: {path}:{line}")
        add_source(f"data/{path.name}", content)
        source_files.append({"path": f"data/{path.name}", "sha256": sha256(content), "word_count": len(entries)})
        total_words += len(entries)
        words.update(entries)
    if not words:
        raise ValueError("the shipped dictionary is empty")
    sorted_words = b"\n".join(sorted(words)) + b"\n"
    return sorted_words, {
        "source_sha256": source_hash.hexdigest(),
        "source_digest_format": "recipe, alphabet, TDICT_SRC order; each UTF-8 path and raw content prefixed by uint64-be byte length",
        "recipe_sha256": sha256(recipe),
        "alphabet_sha256": sha256(alphabet),
        "alphabet": {"first": "0x0e01", "last": "0x0e5b"},
        "source_files": source_files,
        "source_word_count": total_words,
        "unique_word_count": len(words),
        "duplicates_removed": total_words - len(words),
        "sorted_utf8_sha256": sha256(sorted_words),
    }


def archive_pins(manifest_path):
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    pins = []
    for name, version in (("libthai", "0.1.30"), ("libdatrie", "0.2.14")):
        matches = [source for source in manifest["sources"] if source["name"] == name]
        if len(matches) != 1 or matches[0]["version"] != version:
            raise ValueError(f"expected pinned {name} {version} in {manifest_path}")
        source = matches[0]
        if re.fullmatch(r"[0-9a-f]{64}", source["sha256"]) is None:
            raise ValueError(f"invalid archive digest for {name}")
        pins.append({field: source[field] for field in ("name", "version", "url", "sha256")})
    return pins


def fnv1a64(content):
    value = 0xCBF29CE484222325
    for byte in content:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def write_arrays(directory, content, metadata):
    header = """// Generated by build-native-thai-dictionary.py; do not edit.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace native_text::assets {
extern const unsigned char thaiDictionary[];
extern const size_t thaiDictionarySize;
extern const uint64_t thaiDictionaryFingerprint;
extern const char thaiDictionarySourceSha256[];
extern const char thaiDictionarySha256[];
}  // namespace native_text::assets
"""
    with (directory / HEADER_NAME).open("w", encoding="utf-8", newline="\n") as output:
        output.write(header)
    with (directory / SOURCE_NAME).open("w", encoding="utf-8", newline="\n") as output:
        output.write('// Generated by build-native-thai-dictionary.py; do not edit.\n')
        output.write('// Dictionary source: LibThai 0.1.30 (LGPL-2.1-or-later).\n')
        output.write(f'#include "{HEADER_NAME}"\n\nnamespace native_text::assets {{\n')
        output.write('alignas(4) const unsigned char thaiDictionary[] = {\n')
        for offset in range(0, len(content), 16):
            output.write("  " + ", ".join(f"0x{byte:02x}" for byte in content[offset:offset + 16]) + ",\n")
        output.write('};\nconst size_t thaiDictionarySize = sizeof(thaiDictionary);\n')
        output.write(f'const uint64_t thaiDictionaryFingerprint = 0x{metadata["fingerprint_fnv1a64"]}ULL;\n')
        output.write(f'const char thaiDictionarySourceSha256[] = "{metadata["source_sha256"]}";\n')
        output.write(f'const char thaiDictionarySha256[] = "{metadata["serialized_sha256"]}";\n')
        output.write('}  // namespace native_text::assets\n')


def build(args, paths):
    sorted_words, metadata = prepare_words(args.data_dir, paths)
    metadata.update({"format_version": 1, "generator": "native-thai-dictionary-v1",
                     "source_archives": archive_pins(args.vendor_manifest)})
    args.output.mkdir(parents=True, exist_ok=True)
    # Build in the destination filesystem, then publish only completed files.
    # The metadata is replaced last; a failed command never reports success.
    with tempfile.TemporaryDirectory(prefix="native-thai-", dir=args.output) as temp:
        staging = Path(temp)
        (staging / "tdict.txt").write_bytes(sorted_words)
        command = [str(args.builder.resolve()), "--input", str(staging / "tdict.txt"),
                   "--output", str(staging / "thbrk.tri")]
        result = subprocess.run(command, check=True, capture_output=True, text=True, encoding="utf-8")
        report = json.loads(result.stdout)
        if report["word_count"] != metadata["unique_word_count"] or report["retained_count"] != report["word_count"]:
            raise ValueError("builder did not retain every unique source word")
        serialized = (staging / "thbrk.tri").read_bytes()
        if not serialized or len(serialized) != report["serialized_bytes"]:
            raise ValueError("builder reported an incorrect serialized dictionary size")
        metadata.update({"serialized_sha256": sha256(serialized), "serialized_bytes": len(serialized),
                         "fingerprint_fnv1a64": f"{fnv1a64(serialized):016x}"})
        write_arrays(staging, serialized, metadata)
        with (staging / "thbrk.metadata.json").open("w", encoding="utf-8", newline="\n") as output:
            json.dump(metadata, output, ensure_ascii=True, sort_keys=True, indent=2)
            output.write("\n")
        for name in OUTPUT_NAMES:
            os.replace(staging / name, args.output / name)
    print(f"Thai dictionary: {metadata['source_word_count']} source entries, "
          f"{metadata['unique_word_count']} unique entries retained, {len(serialized)} serialized bytes; "
          f"native allocator peak {report['allocator_peak_bytes']} bytes")
    print(f"Source SHA-256: {metadata['source_sha256']}")
    print(f"Dictionary SHA-256: {metadata['serialized_sha256']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=NATIVE_DIR / "vendor/libthai-0.1.30/data")
    parser.add_argument("--vendor-manifest", type=Path, default=NATIVE_DIR / "vendor-sources.json")
    parser.add_argument("--builder", type=Path, help="path to the host NativeThaiDictionaryBuilder executable")
    parser.add_argument("--output", type=Path, help="build output directory (never the source tree)")
    parser.add_argument("--list-inputs", action="store_true", help="print explicit TDICT_SRC paths for CMake dependencies")
    args = parser.parse_args()
    try:
        paths = dictionary_sources(args.data_dir)
        if args.list_inputs:
            for path in paths:
                print(path.resolve().as_posix())
            return 0
        if args.builder is None or args.output is None:
            parser.error("--builder and --output are required unless --list-inputs is specified")
        build(args, paths)
        return 0
    except subprocess.CalledProcessError as error:
        print(f"NativeThaiDictionaryBuilder failed ({error.returncode}): {error.stderr.strip()}", file=sys.stderr)
    except (OSError, ValueError, KeyError, TypeError, MemoryError) as error:
        print(f"NativeThaiDictionaryAssets: {error}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
