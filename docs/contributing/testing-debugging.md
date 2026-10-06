# Testing and Debugging

CrossPoint runs on real hardware, so debugging usually combines local build checks and on-device logs.

## Local checks

Make sure `clang-format` 21+ is installed and available in `PATH` before running the formatting step.
If needed, see [Getting Started](./getting-started.md).

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

## Cold images and SD-font metrics

Run the focused production-linked host checks from the repository root. On
Windows, use an x64 Visual Studio developer shell so MSVC's standard-library
headers and linker environment are available.

```sh
cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release
cmake --build build/test --config Release --target ImagePreparationTest SdCardFontTest InflateStreamTest FontCacheManagerTest ChapterHtmlSlimParserTest ThaiRenderProbe
ctest --test-dir build/test -C Release -R 'ImagePreparationTest|SdCardFontTest|InflateStreamTest|FontCacheManagerTest|ChapterHtmlSlimParserTest' --output-on-failure
./build/test/image_preparation/ImagePreparationTest --smoke-output build/image-preparation-smoke
pio run -e gh_release -e x4pro-gh_release -e papermono-gh_release
```

Use `ImagePreparationTest.exe` on Windows. First configuration fetches the pinned
GoogleTest and JPEGDEC dependencies. The fixture helper extracts the JPEG already
in `test/epubs/test_jpeg_images.epub`; it does not regenerate that EPUB.

The image executable uses real JPEG decoding, inflation, Page/ImageBlock drawing,
pixel-cache I/O and framebuffer loans. It denies inflater heap allocation during
cold extraction and checks both 792×528 and 800×480 buffers in all four orientations.
Smoke output contains cold/warm PGM pairs and `result.txt`; all BW/MSB/LSB planes
must match, with a complete 15,004-byte 200×300 pixel cache and restored ownership.
The grayscale-square source values are 0/96/160/255. Existing screen-relative Bayer
dithering makes the exact origin-zero center samples 0/1/1/3, not 0/1/2/3; checks
cover the surrounding 4×4 tiles to verify all four levels without changing dithering.
Inspect the PGM pairs for correct rotation, four grayscale squares and no extraction
scribbles. Failure cases cover truncated input, partial-source removal, invalid
caches, retry suppression/recovery and successful direct drawing when cache writes fail.

Metric tests include constrained allocations, repeated raw/shaped text, conservative
multi-style sizing, the exact 4,096/4,097 boundary, reverse-order ligature closure,
mixed Thai/Latin/CJK metrics, empty requests and OOM/read-error recovery. These are
focused suites, not the exhaustive Thai font/layout matrix.

### Physical-device acceptance

Host buffers do not establish ESP heap headroom, SD behavior or refresh-waveform
correctness. Test X3, original X4 and one available PSRAM board; record actual boards,
not inferred coverage from builds. Paper Mono additionally needs a cold-image turn
after an asynchronous text refresh, grayscale output and the next page turn.

Use the reporter's exact EPUB/font/size when available; otherwise use
`test_jpeg_images.epub`, `test_png_images.epub`, `test_mixed_images.epub` and
`test_thai_reading.epub` with the Thai SD font. Back up the selected book cache and
progress. Test removal of only that book's source-image/`.pxc` caches, then a separate
fresh book cache. Select Thai before boot/open, visit the cover/first image, turn
away/back, close/reopen and switch fonts. Require cold images without font switching,
no ZIP/metric OOM, intact mixed-text shaping and no re-extraction on warm visits.
Check all orientations and AA on/off on a representative mixed page.

With temporary instrumentation, record internal free heap/largest block before and
after reclamation, extraction, decode and font prewarm; do not treat host heap values
as device measurements. Compare cold-page elapsed time on a PSRAM board because
reclamation may add font reload work. Check home covers separately: this change does
not alter their buffer-allocation policy. Remove temporary telemetry before shipping.

## Flash and monitor

Flash firmware:

```sh
pio run --target upload
```

Open serial monitor:

```sh
pio device monitor
```

Optional enhanced monitor:

```sh
python3 -m pip install pyserial colorama matplotlib
python3 scripts/debugging_monitor.py
```

## Useful bug report contents

- Firmware version and build environment
- Exact steps to reproduce
- Expected vs actual behavior
- Serial logs from boot through failure
- Whether issue reproduces after clearing `.crosspoint/` cache on SD card

## Common troubleshooting references

- [User Guide troubleshooting section](../../USER_GUIDE.md#7-troubleshooting-issues--escaping-bootloop)
- [Webserver troubleshooting](../troubleshooting.md)
