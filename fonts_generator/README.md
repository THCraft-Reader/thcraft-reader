# THCraft Font Workshop

Browser-only TTF/OTF → CPFont v4 conversion with optional Thai CPSHAPE v1 positioning. Rust, FreeType and HarfBuzz compile into WebAssembly. The website loads a separate, content-hashed WASM asset on demand; the standalone distribution embeds everything in one HTML file. Neither uses a CDN, conversion server API, or font uploads.

## Use

Open `dist/fonts-generator.html` directly in a current desktop browser, or visit the site's `font-generator/` page. Both include a resized copy of the existing THCraft mascot. The website fetches its engine only after **Generate font pack** is clicked; the standalone HTML needs no network.

The website header's **กลับหน้าแรก** link returns to the THCraft homepage.
**EpubCraft ↗** opens `https://epubcraft.com` in a new tab without discarding the
current font settings. The homepage link is hidden in the standalone local file.

1. Choose a regular TTF/OTF and a family name. Add actual bold/italic/bold-italic files if available. The converter does not synthesize styles.
2. Add ordered fallback fonts for each style to fill missing characters. Primary glyphs take precedence; the first fallback containing a character wins. Each style has an independent fallback list.
3. Select sizes and **Additional Unicode coverage**. Base coverage is always requested: Basic Latin U+0020–007E, General Punctuation U+2000–206F, and replacement U+FFFD. Missing glyphs are omitted with counts in warnings/reports, not fabricated.
4. Enable **Bake Thai positioning** for matching `.cpshape` companions. Each primary style must contain all required Thai context characters and must be a static/frozen face. A fallback cannot supply the primary Thai shaping face. Disabling Thai coverage also disables Thai positioning; enabling positioning selects Thai coverage.
5. Generate and download the ZIP. Extract its `fonts` folder to the SD-card root, retain same-basename `.cpfont`/`.cpshape` pairs, restart, and select the family in Settings → Reader → Font Family. Include 8/10/12 pt for size-matched UI fallback.

The bitmap proof decodes the generated files. Each tile is an independent glyph or Thai cluster, not a browser-font approximation or a complete reader layout. Reports include source hashes, settings, engine versions, fallback usage, omissions, and companion checksums. Optional font license files are copied into the ZIP; users remain responsible for font redistribution rights.

Non-fatal conversion messages appear in a collapsed **Warnings (count)** panel.
Expand it to inspect every message; the ZIP download stays available. Starting a
new conversion resets the panel. Fatal conversion errors remain directly visible
and do not publish a download.

### Coverage presets

Reading (Fiction), Default (CrossPoint), Latin Extended, Greek, Cyrillic, Vietnamese, Hebrew, Arabic (Farsi/Urdu), Armenian, Georgian, Ethiopic, Cherokee, Tifinagh, Thai, Hangul, Chinese (Simplified/Traditional), Japanese, Symbols & Arrows, and IPA characters. Additional hexadecimal ranges accept `0300-036F`, `U+2190-U+21FF`, or individual codepoints.

Presets overlap and are merged. The Chinese choices request the same BMP Han/radical/punctuation/Bopomofo blocks: regional glyph forms come from the supplied font, not from conversion or a change of Unicode codepoint. Japanese adds kana coverage. Supplementary characters may be requested with custom ranges; CPFont's kerning/ligature pair fields remain BMP-only. Script coverage does not add new shaping support to firmware; CPSHAPE v1 is Thai-specific.

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

Pinned engine: Rust 1.90.0, Emscripten 4.0.15, FreeType 2.13.3, HarfBuzz 10.4.0, ttf-parser 0.25.1. FreeType's HarfBuzz-assisted auto-hinting is enabled; unencoded Thai alternates need this for reference-compatible rasterization. Emscripten uses the standard library's unwind-compatible panic strategy. The C ABI exports are retained in distribution builds, not native unit-test executables.

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
- `src/thai.rs`: 3,772 contexts, deterministic alternate mapping, companion serialization/oracle roundtrip and CRC binding.
- `src/opentype.rs`: CPFont-compatible kerning and ligature extraction.
- `native/`: FreeType/HarfBuzz C ABI and native build configuration.
- `web/`: interface, generated-bitmap proof, background worker/ZIP writer, and the resized `thcraft-mascot.webp` derived from the site's existing `assets/thcraft-mascot.png`.
- `scripts/setup.py`, `build.py`, `scripts/package.py`: isolated dependencies, compilation and static-site packaging.

The website fetches the same-origin engine on conversion, not page load. A content-hashed filename avoids using stale engine bytes after updates. The worker receives the engine and font bytes, writes temporary MEMFS `/input` files, invokes Rust, copies completed outputs, and creates a stored ZIP. Cancelling aborts any pending engine fetch, terminates the worker, and invalidates pending file reads. Each conversion uses a fresh worker; large font buffers and WASM memory are released when it terminates. These allocations are in the user's browser, not the reader's constrained RAM.

`convert` takes a NUL-terminated JSON string and returns an owned JSON string; release it exactly once with `free_result`. Inputs use `/input/...` paths:

```json
{
  "family": "MyFont",
  "sizes": [8, 10, 12, 14, 16, 18],
  "intervals": [[32, 126], [3584, 3711], [8192, 8303]],
  "thai": true,
  "autohint": false,
  "styles": [{"id": 0, "path": "/input/regular.ttf", "fallbacks": []}]
}
```

Results contain `files` (`path`, `kind`, `size`) and `warnings`, or `error`. The low-level API accepts explicit intervals and always adds U+FFFD; the browser adds the rest of the always-on base coverage. A successful request replaces `/output`; a failed request does not publish a partial pair. CPFont is little-endian with continuous 2-bit packing and 12.4 advances. CPSHAPE binds to CPFont metadata in file order, excluding bitmaps, and enforces 96 KiB/style and 384 KiB/family.

## Verification

Run the pure-Rust behavior/boundary suite:

```text
cargo test --lib --locked
```

Verified on Windows with Chromium/Edge:

- 16 Rust tests: bitmap pitch/packing, fixed-point rounding, kerning classes, aliases, ligature handling, validation, Thai recipes and CRC binding.
- 12 byte-identical CPFont/CPSHAPE outputs against the existing Python converter: Noto Sans Thai at 8/16 pt, four styles at 16 pt, Noto Serif Thai, Sarabun, Latin auto-hinting, and CFF OTF. The Python environment used FreeType 2.13.2/HarfBuzz 14.4.0; the match is evidence for these fixtures, not a claim about every font/version.
- Real site uploads, Hebrew coverage from a fallback, ZIP download/CRC validation, offline file-based conversion with networking disabled, two-size base-only output, custom-range validation, cancellation and regeneration.
- Responsive 390 px layout and actual bitmap proof.
- Separate-asset site delivery: no WASM or license-document request during initial page load; one 1,966,472-byte same-origin WASM request after conversion starts; successful generation and visible missing-asset errors. With the mascot included and notices linked separately, initial HTML is 206,499 bytes instead of the original 4,015,542 bytes. The standalone build retains embedded notices and still converts with networking disabled.
- A browser-generated Thai pair loaded by the firmware's production-linked `ThaiRenderProbe`, with shaping active, placement checks and cache roundtrip, producing two pages.

Device testing is still required: copy a generated pair to SD, restart, check Thai marks and fallback glyphs in books/UI, and verify orientations and heap behavior on the target reader. No firmware code or format version changes are required. Current FreeType upstream CMake emits a compatibility deprecation warning with newer CMake; compilation and packaging succeed.
