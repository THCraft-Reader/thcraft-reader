# THCraft Font Workshop

Browser-only TTF/OTF → native Unicode CPFont v4 conversion. Rust and FreeType compile into WebAssembly; HarfBuzz supports FreeType's auto-hinter, not recipe generation. Thai positioning is handled by firmware without companion files. The website loads a separate, content-hashed WASM asset on demand; the standalone distribution embeds everything in one HTML file. Neither uses a CDN, conversion server API, or font uploads.

## Use

Open `dist/fonts-generator.html` directly in a current desktop browser, or visit the site's `font-generator/` page. Both include a resized copy of the existing THCraft mascot. The website fetches its engine only after **Generate font pack** is clicked; the standalone HTML needs no network.

The website header's **กลับหน้าแรก** link returns to the THCraft homepage.
**EpubCraft ↗** opens `https://epubcraft.com` in a new tab without discarding the
current font settings. The homepage link is hidden in the standalone local file.

1. Choose a regular TTF/OTF and a family name. Add actual bold/italic/bold-italic files if available. The converter does not synthesize styles.
2. Add ordered fallback fonts for each style to fill missing characters. Primary glyphs take precedence; the first fallback containing a character wins. Each style has an independent fallback list.
3. Select sizes and **Additional Unicode coverage**. Base coverage is always requested: Basic Latin U+0020–007E, General Punctuation U+2000–206F, and replacement U+FFFD. Missing glyphs are omitted with counts in warnings/reports, not fabricated.
4. Generate and download the ZIP. Extract its `fonts` folder to the SD-card root, restart, and select the family in Settings → Reader → Font Family. Include 8/10/12 pt for size-matched UI fallback.

The bitmap proof decodes the generated files. Each tile shows one Unicode glyph at its native bearing, including independent zero-advance marks. It does not simulate firmware Thai positioning or reader layout. Reports include source hashes, settings, engine versions, fallback usage, omissions, and CPFont checksums. Optional font license files are copied into the ZIP; users remain responsible for font redistribution rights.

Non-fatal conversion messages appear in a collapsed **Warnings (count)** panel.
Expand it to inspect every message; the ZIP download stays available. Starting a
new conversion resets the panel. Fatal conversion errors remain directly visible
and do not publish a download.

### Coverage presets

Reading (Fiction), Default (CrossPoint), Latin Extended, Greek, Cyrillic, Vietnamese, Hebrew, Arabic (Farsi/Urdu), Armenian, Georgian, Ethiopic, Cherokee, Tifinagh, Thai, Hangul, Chinese (Simplified/Traditional), Japanese, Symbols & Arrows, and IPA characters. Additional hexadecimal ranges accept `0300-036F`, `U+2190-U+21FF`, or individual codepoints.

Presets overlap and are merged. The Chinese choices request the same BMP Han/radical/punctuation/Bopomofo blocks: regional glyph forms come from the supplied font, not from conversion or a change of Unicode codepoint. Japanese adds kana coverage. Supplementary characters may be requested with custom ranges; CPFont's kerning/ligature pair fields remain BMP-only. Script coverage does not add new shaping support to firmware.

## Build

Prerequisites: Python 3.12+, rustup, CMake 3.20+, and Ninja on PATH. Setup supports Windows x64 and Linux x86_64; the Windows build is verified. A working native Rust host linker is required (Visual Studio C++ Build Tools on Windows, standard C/C++ build tools on Linux).

From this folder:

```text
python build.py
```

To also update the existing local static site:

```text
python build.py --site-dir D:/source/thcraft-site
```

The first build downloads the pinned SDK, Rust toolchain, and native sources into `.cache/` (roughly a gigabyte of downloads). Archive SHA-256 hashes are checked. Build downloads are distinct from runtime: standalone users need no network; website users fetch a same-origin WASM asset when starting conversion. Cargo dependencies are locked in `Cargo.lock`; no global Rust configuration is changed.

Pinned engine: Rust 1.90.0, Emscripten 4.0.15, FreeType 2.13.3, HarfBuzz 10.4.0, ttf-parser 0.25.1. FreeType's HarfBuzz-assisted auto-hinting remains enabled for rasterization. No HarfBuzz shaping buffers or per-font recipe tables are generated. Emscripten uses the standard library's unwind-compatible panic strategy. The C ABI exports are retained in distribution builds, not native unit-test executables.

Outputs:

- `build/engine.js`, `build/engine.wasm`: compiled browser/Node engine.
- `dist/fonts-generator.html`: standalone distributable, about 4 MB.
- `dist/LICENSES.txt`: project and bundled third-party notices, embedded only in the standalone HTML.
- `dist/build-manifest.json`: SHA-256 digests of packaged inputs and the standalone output.
- With `--site-dir`: `<site>/font-generator/index.html`, `engine.<content-hash>.wasm`, and a linked `LICENSES.txt`. Deploy all three; retain older hashed engines while cached pages may reference them. License text loads only when its link is opened. No deployment or Git operation occurs.

For HTML/JS-only changes, reuse the compiled engine:

```text
python scripts/package.py --site-dir D:/source/thcraft-site
```

`python build.py --setup-only` prepares dependencies without compiling. `.cache/`, `build/`, `target/`, and `dist/` are ignored; the generated page and hashed WASM in the site repository are its deployable artifacts. The source project has no build-time dependency on the parent firmware checkout.

## Layout and contracts

- `src/lib.rs`: request validation, raster packing, CPFont serialization, reports and staged publication.
- `src/opentype.rs`: CPFont-compatible kerning and ligature extraction.
- `native/`: FreeType rasterization C ABI, font-byte loading and native build configuration.
- `web/`: interface, generated-bitmap proof, background worker/ZIP writer, and the resized `thcraft-mascot.webp` derived from the site's existing `assets/thcraft-mascot.png`.
- `scripts/setup.py`, `build.py`, `scripts/package.py`: isolated dependencies, compilation and static-site packaging.

The website fetches the same-origin engine on conversion, not page load. A content-hashed filename avoids using stale engine bytes after updates. The worker receives the engine and font bytes, writes temporary MEMFS `/input` files, invokes Rust, copies completed outputs, and creates a stored ZIP. Cancelling aborts any pending engine fetch, terminates the worker, and invalidates pending file reads. Each conversion uses a fresh worker; large font buffers and WASM memory are released when it terminates. These allocations are in the user's browser, not the reader's constrained RAM.

`convert` takes a NUL-terminated JSON string and returns an owned JSON string; release it exactly once with `free_result`. Inputs use `/input/...` paths:

```json
{
  "family": "MyFont",
  "sizes": [8, 10, 12, 14, 16, 18],
  "intervals": [[32, 126], [3584, 3711], [8192, 8303]],
  "autohint": false,
  "styles": [{"id": 0, "path": "/input/regular.ttf", "fallbacks": []}]
}
```

Results contain `files` (`path`, `kind`, `size`) and `warnings`, or `error`. The low-level API accepts explicit intervals and always adds U+FFFD; the browser adds the rest of the always-on base coverage. A successful request replaces `/output`; a failed request does not publish a partial pack. CPFont is little-endian with continuous 2-bit packing and 12.4 advances. Each size emits one `.cpfont` plus a desktop provenance report. Ordinary kerning and ligature tables are preserved. Firmware derives Thai positioning from native glyph metrics; no font-specific alternates are baked.

## Verification

Run the pure-Rust behavior/boundary suite:

```text
cargo test --lib --locked
```

The suite covers bitmap pitch/packing, kerning classes, aliases, ligature handling, request validation, and CPFont section layout. The companion-free cutover requires rebuilding the engine and both HTML distributions; do not package new JavaScript with an older compiled engine.

For integration verification, convert native Thai, Latin and CFF fonts with and without fallbacks; validate the ZIP and CPFont output, test cancellation/regeneration and offline standalone conversion, and compare decoded pixels with the Python converter. The proof should show independent glyphs, not claim firmware-equivalent Thai clusters.

Device testing is still required: copy generated CPFonts to SD, restart, check Thai marks and fallback glyphs in books/UI, and verify orientations and heap behavior on the target reader. Use firmware with native Thai shaping support.
