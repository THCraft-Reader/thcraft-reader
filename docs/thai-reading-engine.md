# Thai reading engine

## Baseline and scope

Architecture inspected on `develop`, CrossPoint 1.6.5, revision
`d1509d0735bd0b7832c6765e2aa83e1f0c008eff`. This milestone preserves CPFont v4
and the existing bitmap renderer. The acceptance baseline must be captured before
Thai analysis or glyph-placement changes. Host pixel evidence does not establish
X4 Pro display, latency, internal-heap or PSRAM acceptance.

## Thai Justify

**Thai Justify** is a separate alignment option in Text Settings, the reader
overlay, and `/api/settings` (`paragraphAlignment: 5`). Existing values 0–4 and
the default Justify are unchanged. Its preview deliberately uses Thai text.

Dictionary wrapping still chooses lines. On non-final lines with eligible Thai
slots, spare advance width is shared equally between complete Thai letter
clusters and ordinary space/CJK gaps; integer remainders go left-to-right.
Vowels and tone marks stay with their cluster. Native ligatures, ruby groups,
and focus-split tokens are internally atomic. Numbers, punctuation, malformed
spans, orphan marks, and script transitions do not create Thai slots.
There is no spacing cap or compression; final and overwide lines stay natural.
English-only paragraphs use ordinary Justify.

Thai-bearing paragraphs prepare exact rendered metrics, including native
ligature outputs and kerning. Paired CPSHAPE, absent companions, and shaping
disabled builds use the same expansion rules; fallback does not repair a font's
existing mark design. Failed exact preparation or scratch allocation fails the
section build rather than caching incomplete layout.

Text and source offsets are unchanged. Expanded word/link rectangles include
the internal budget. Resident metadata costs two bytes per existing word only
on lines with expansion, inside TextBlock's existing arena. Layout reuses one
four-byte-per-maximum-line-word scratch allocation and a lazy analyzed-token
provenance bit per token; it does not retain per-cluster records. Unusually long
Thai-bearing tokens (over 210 bytes) also use one checked, token-sized buffer
reused across exact prefix measurements and streaming layout calls. Ordinary
dictionary words use the bounded stack buffer. Section v52 invalidates old
rendered caches without changing progress or font formats.

Host verification uses `ThaiRenderProbe --alignment thai-justify
--cache-roundtrip on --verify-thai-placement on`: glyph-level checks cover
normal/rotated and scaled drawing, cold/full prewarm, chained native ligatures,
and allocation failure. Cache replay must match BW and both grayscale planes.
On-device acceptance also requires checking both six-option menus and the Thai
preview in all orientations, reopening at the same reading position, saving and
rebooting with value 5, expanded selection/link hit boxes, and free heap above
50 KiB without persistent growth across page turns.


## Existing pipeline

EPUB extraction produces chapter XHTML. Expat callbacks in
`lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:1560` feed a 201-byte generic
word buffer (`ChapterHtmlSlimParser.h:24–39`). `ParsedText::addWord`
(`ParsedText.cpp:443–453`) applies NFC before tokenization. ParsedText owns
WordStore text, parallel style/source/link fields, width measurement and line
selection. Completed lines become TextBlock/Page objects, serialized by Section
(`Section.cpp:95–114`). Page renders its elements (`Page.cpp:131–133`);
TextBlock calls GfxRenderer (`blocks/TextBlock.cpp:136`). GfxRenderer measures
and draws through EpdFontFamily/EpdFont, with SdCardFont supplying CPFont metrics
and cached bitmap data (`GfxRenderer.cpp:683,2100,2220`).

CJK tokenization permits selected character boundaries and protects punctuation
(`ParsedText.cpp:69–220`). Hangul preserves source spaces and optionally splits
at a line end; it is not an invisible justification gap. Arabic contextual
joining and visual reordering occur through MiniBidi (`BidiUtils.cpp:136–140`),
not through a general OpenType engine. Vietnamese/Latin decomposed marks use NFC
where supported. Existing mark positioning is real: EpdFont centers/anchors
combining marks and preserves selected native heights
(`EpdFont.cpp:47–58`, `EpdFontData.h:33–109`). Thai signs are not included in the
generic combining predicate (`Utf8.h:99–104`). Their native zero advances and
negative bearings must be judged from actual CPFont output, not from the absence
of a Thai-specific branch.

CPFont v4 stores per-glyph bitmap dimensions/bearings and unsigned 12.4 advances;
kerning uses signed 4.4 values. Measurement and drawing use differential rounding
(`EpdFontData.h:7–30,125–139`). SdCardFont keeps bounded page glyph caches (512
codepoints) and persistent per-style advance caches (768 entries)
(`SdCardFont.h:23,297–310`). Pair kerning adjusts adjacent pen positions; it is
not contextual shaping, mark substitution or stacked-mark attachment. Font
identity currently hashes the header/style TOC (`SdCardFont.h:144–146`).

## Thai analysis contract

Thai cannot be added to `utf8IsCjkBreakable`: breaks require orthographic clusters
and dictionary context. Ordinary Justify does not stretch Thai dictionary gaps.
Reuse TokenBoundary's two bits (`TokenBoundary.h:7–19`): ordinary
space `(false,false)`, breakable zero-space attachment `(true,true)`, protected
attachment `(true,false)`. A separate lazy break kind distinguishes ordinary
space/word/punctuation opportunities from emergency and prohibited boundaries
without replacing these spacing invariants.

Analysis belongs in the shared `lib/ThaiText` library. The parser's bounded
stream retains 768 UTF-8 bytes plus at most 256 eight-byte source/style/link
records (at most 3 KiB including scalar state). It is allocated lazily once per
Thai-bearing parser, not on the small task stack or for English-only books.
The 24-codepoint repair window, 70-codepoint dictionary maximum and 32-codepoint
cluster lookahead require at most 126 Thai codepoints (378 bytes) before a
streaming decision; parser buffer capacities are unchanged. Inline markup must
not split a cluster; marks adopt their base's style/link. Real
block/ruby/table/whitespace transitions finalize pending text. Malformed
pathological mark runs retain their bytes through the documented legacy
fallback; they do not enlarge the analysis window.

The allocation-free cluster primitives implement the ordered pinned TCC grammar
with the explicit `เ + base + ื + tone? + อ + ะ?` correction. Thirteen analyzer
tests pass, covering every byte split, nonterminated/malformed UTF-8, grammar
priority and 1,000 repeated signs. A standalone native smoke run emits
`เรื่อ|ง`, `เพื่อ`, `นํ้า` and `ป|ระ|เท|ศ|ไท|ย`; these are orthographic clusters,
not dictionary tokens. Parser integration passes 19 streaming/transition tests
and 11 token-storage tests, including deferred linked footnotes, ruby/table
transfers, 64 KiB unknown input, allocation failure and resumed analysis after
pathological signs. The complete host suite passes 459 enabled tests after this
cutover. Production probe smoke output retains `กี่` as one bold token across
`<b>ก</b>ี่`, and records population tokens at source offsets 0, 9, 11 and 18
without inserted spaces (`build/thai/parser-smoke/`).

Dictionary data is immutable flash data, generated offline from pinned
[PyThaiNLP CC0 words](https://github.com/PyThaiNLP/pythainlp/blob/4be114097e0cb1d9cfe044691f2aa79bc294925e/pythainlp/corpus/words_th.txt)
plus a reviewed `ประเทศไทย` supplement. Sixteen-word prefix-compressed blocks
use Thai single-byte symbols and uint32 offsets; lookup uses a 71-byte decoder
scratch buffer, no startup copy, mutable global LRU or SD lookup. This follows
the generated constexpr-data convention in `docs/hyphenation-trie-format.md:39–46`,
not Liang hyphenation semantics. Production longest matching includes the
compound `จำนวนมาก`; a legal dictionary-prefix/suffix split permits
`จำนวน|มาก` when the compound must wrap.

Generated data: 60,964 words, maximum 70 codepoints; 367,377 encoded bytes plus
15,248 offset bytes plus a 186-byte first-symbol directory = 382,811 array bytes
before linker padding, CRC32 `4b87877d`. Eight dictionary, twenty-five segmenter
and ten generator tests pass. A standalone production
segmenter emits `ประเทศไทย|มี|ประชากร|จำนวนมาก`; the proper-prefix splitter emits
`จำนวน|มาก`. Invalid dictionary views retain cluster-safe emergency output.

The generated 93-entry uint16 directory narrows the production accessor's
block-leader search by first Thai symbol. Each range includes the preceding
block, which can contain the first matching words even when its leader starts
with an earlier symbol. Empty symbol buckets retain that predecessor. Injected
`DictionaryView` accessors keep the full-range search; neither path allocates.
The directory is derived from the existing lexical payload and is excluded from
`DATA_ID`: vocabulary, cluster checks, DP scoring, streaming thresholds and
`ThaiLayoutId` are unchanged, so this optimization does not invalidate page caches.

Word segmentation uses greedy longest matching with local DP repair. Before
committing a word, it examines complete greedy segments within the next 24
Thai codepoints. Unknown clusters or known one-codepoint words trigger DP over
that bounded window; clean spans keep greedy matching. Repair stops before
punctuation, digits, non-Thai or malformed input, and never cuts a greedy word
at the window edge. Candidate dictionary ends must remain original TCC boundaries.
Paths minimize unknown codepoints, then known one-codepoint words, then segment
count. Exact ties prefer longer first words; the greedy choice changes only for
a strictly better score. The DP arrays occupy 75 stack bytes with no heap
allocation, mutable shared scratch or recursion. A dictionary miss at the current
position emits the existing emergency cluster directly, since there is no
alternative known first edge.

This repairs local greedy dead ends, not linguistic ambiguity in fully known
multi-character words, and cannot invent missing dictionary entries. The
pre-change greedy tokenizer selected `ยิ่ง|ได้มา|ก|เท่าไหร่`, stranding unknown
`ก`; hybrid segmentation emits `ยิ่ง|ได้|มาก|เท่าไหร่`. Style fragments inside
`มาก` remain attached rather than becoming word breaks. Analyzer and line-breaking
identity version 3 invalidates old rendered layouts without changing the section
format or reading progress.

Historical hybrid benchmark before the first-symbol directory: macOS arm64,
Apple C++ Release (`-O3 -DNDEBUG`), median of three
500-iteration runs. Times below are microseconds per corpus pass in parser-like
scalar-stream mode, not per word or ESP32 timings. The executable also measures
whole-input mode and checks whole/stream boundary checksums. The ambiguity and
unknown corpora use a tiny real compressed dictionary; other rows use production
data. Repeated sentences include their trailing separator.

| Corpus per pass | Greedy µs | Hybrid µs | Ratio |
| --- | ---: | ---: | ---: |
| Clean known prose, 8 sentences | 82.64 | 121.36 | 1.47× |
| Requested phrase, 16 repetitions | 93.07 | 433.36 | 4.66× |
| Compound-heavy, 8 repetitions | 69.61 | 88.64 | 1.27× |
| Greedy dead-end fixture, 32 repetitions | 21.49 | 55.79 | 2.60× |
| Unknown run, 384 codepoints | 578.19 | 650.58 | 1.13× |
| Mixed Thai/Latin, 8 repetitions | 35.87 | 40.25 | 1.12× |

All timed loops recorded zero standard C++ allocations (direct libc allocation is
not intercepted). Unknown codepoints per requested phrase and per ambiguity
fixture fell from one to zero; clean, compound, unknown and mixed corpus
checksums were unchanged. The unknown-run pending high-water increased from 306
to 378 bytes within the existing buffer. An isolated ESP32-C3 `-Os -fstack-usage`
compile reports a 256-byte `nextSegment` frame including inlined repair; this
excludes nested dictionary/cluster call frames.

To reproduce against the saved greedy revision without retaining a second
production tokenizer:

```sh
git show 375d223eb9fed7d36ca89aca65f688a2bd77844a:lib/ThaiText/ThaiSegmenter.cpp > /tmp/ThaiSegmenter-greedy.cpp
cmake -S test -B build/test -DTHAI_SEGMENTER_BASELINE_SOURCE=/tmp/ThaiSegmenter-greedy.cpp
cmake --build build/test --target ThaiSegmenterBenchmark
build/test/thai_text/ThaiSegmenterBenchmark 500
cmake -S test -B build/test -DTHAI_SEGMENTER_BASELINE_SOURCE=
rm /tmp/ThaiSegmenter-greedy.cpp
```

Verification: 104 Thai host tests and 10 final/partial cache tests pass; `default`
(ESP32-C3) and `x4pro` firmware builds succeed. A production parser/layout/render
smoke at a 96-pixel logical width keeps `มาก` on one line, including a style change
between `มา` and `ก`, with eight known words and zero unknown clusters across two
copies of the phrase. Device page-turn latency and heap high-water remain hardware
checks; reopen an existing book after flashing to exercise automatic reflow.

Historical native Windows/MSVC host lookup comparison without the directory
(10 iterations, 121,928 common
queries per iteration; background visual capture active, not target timings):

| Representation | Array bytes | Decoder scratch | Lookups/s |
| --- | ---: | ---: | ---: |
| Indexed UTF-8 | 1,736,889 | 0 | 902,247 |
| Indexed Thai symbols | 782,179 | 71 | 794,389 |
| Prefix-compressed blocks | 382,625 | 71 | 585,348 |

All three return the same matched-byte checksum (28,648,860). The generated
flash accessor has no initialization scan/copy; validating an injected compressed
view took 1,348.2 microseconds in this host run. These figures quantify the
storage/lookup tradeoff, not ESP32 speed or heap savings.

First-symbol directory benchmark: macOS arm64, Apple C++ Release, median of five
sequential samples at ten iterations (1,219,280 lookups per sample). The same
executable compares the injected unindexed compressed view with the actual default
production accessor, using identical source/supplement data and timed queries:

| Representation | Array bytes | Median lookup µs | Matched-byte checksum |
| --- | ---: | ---: | ---: |
| `prefix_blocks_16` | 382,625 | 833,489 | 28,648,860 |
| `production_prefix_ranges` | 382,811 | 761,999 | 28,648,860 |

The indexed path takes 8.58% less host lookup time, above the 5% acceptance
threshold. Untimed equivalence checks also cover byte bounds 0, 2, query length
minus one, exact length and length plus three; appended tone marks; and all Thai
first symbols including empty buckets. Both accessors remain valid. Custom input
paths benchmark only their own corpus, without comparing unrelated compiled data.

An isolated throwaway harness linked the current ThaiText sources and called only
the current `nextSegment` with indexed/unindexed accessors. Before timing it
compared every segment's boundaries, break kinds, codepoint count, known/valid
flags, source coverage, unknown/singleton counts and whole/scalar-stream output.
Both accessors also split `จำนวนมาก` at the byte length of `จำนวน`.
Five sequential 500-iteration samples gave these median µs per corpus pass:

| Corpus | Whole unindexed | Whole indexed | Scalar unindexed | Scalar indexed |
| --- | ---: | ---: | ---: | ---: |
| Clean known prose | 90.092 | 84.359 | 119.976 | 114.203 |
| Requested phrase | 400.486 | 324.670 | 421.037 | 345.874 |
| Compound-heavy | 62.289 | 59.421 | 87.467 | 84.661 |
| Ambiguity fixture | 78.461 | 78.270 | 54.677 | 54.922 |
| Long unknown run | 337.258 | 337.582 | 646.850 | 647.604 |
| Mixed Thai/Latin | 35.185 | 31.388 | 39.825 | 36.000 |

The ambiguity/unknown rows use the same tiny fixture for both accessors and are
unchanged controls. All rows meet the maximum regression threshold of 5% plus
1 µs per pass. Timed loops recorded zero standard C++ allocations (not direct
libc allocations); pending high-water was identical, including 378 bytes for
the unknown run. The 105-test Thai host suite, ten generator tests and eight
dictionary tests in a separate `THAI_DICTIONARY=0` build pass.

The `default` ESP32-C3 firmware build succeeds. Its ELF contains exactly one
186-byte `FIRST_SYMBOL_BLOCKS` object in `.flash.rodata`, alongside `DATA` and
`OFFSETS`; the dictionary object has zero-sized `.data`/`.bss` and no heap-allocation
references. The added storage is flash payload, not a startup RAM table or copy.

The `x4pro` ESP32-S3 build also succeeds, with wolfSSL macro-redefinition warnings.
Its ELF likewise contains one 186-byte directory in `.flash.rodata`. After flashing
the X4 Pro firmware, the device owner reported no problems in a reading smoke test
on 2026-10-01. This is user-reported functional evidence, not measured device
latency, heap stability or completion of the hardware acceptance procedure below.

Reproduce lookup equivalence and timing from the repository root:

```sh
cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release -DTHAI_SEGMENTER_BASELINE_SOURCE=
cmake --build build/test --target ThaiTextTest ThaiDictionaryBenchmark
python3 test/thai_text/test_dictionary_generator.py
build/test/thai_text/ThaiTextTest
for sample in 1 2 3 4 5; do
  build/test/thai_text/ThaiDictionaryBenchmark --iterations 10
done
```

These results are not ESP32 latency or device heap evidence. Hardware acceptance
requires identical fonts, books, SD and refresh settings: collect 20 cold opens,
20 warm reopens, 50 turns and five open/turn/exit cycles with `THAI_ENGINE_STATS`.
Compare median/p95 segment, layout, draw and refresh times separately; require
internal free heap above 50 KiB, stable heap across cycles, adequate stack headroom,
identical pages and no cache invalidation caused solely by this directory.

Thai-bearing blocks use the furthest fitting legal space, dictionary-word or
punctuation boundary. A source space does not take priority over a later word
boundary: Thai phrase spacing must not force a short line when more whole words
fit. Cluster emergency splitting applies only when an otherwise empty effective
line cannot fit the word/run. Style attachments, ruby, NBSP and punctuation remain
protected; dictionary boundaries do not become visible or stretchable spaces.
Prefix commits reuse WordStore suffix ownership; no unbounded prefix string is
built. The extra persistent layout metadata is one rank byte per token in
Thai-bearing blocks, plus vector capacity; non-Thai rank vectors stay empty.

Section version 51 uses a 48-byte header, including `thaiLayoutId` immediately
after word spacing and upstream's paragraph indentation. Both final and suspended layouts compare its analyzer,
dictionary and behavior identity. Commits with transient analysis failure set
bit 31; current pages stay readable, but a healthy reopen reflows those sections.
Ten final/partial cache tests pass, including failure → healthy reopen and
preservation of progress, metadata and extracted HTML.

All 487 host tests pass after ranked layout/cache integration. Actual CPFont
smoke output at a measured 231-pixel width starts line two with intact
`โรงพยาบาล`; at the measured 89-pixel **layout** width it splits `จำนวน|มาก`.
The native glyph-only path measures that prefix at 88 pixels; this existing
SD layout/drawing rounding distinction is recorded rather than hidden by an
arbitrary viewport. Active companion placement must unify those modes.
Injected Thai-state allocation failure logs once and renders all population
text through the native fallback (`build/thai/wrap-smoke/`).

`THAI_ENGINE_STATS` compiles out by default. Enabled counters use cumulative
snapshots, never shared resets. A production host smoke measured 78 analyzed
bytes, 18 clusters, four dictionary words, zero unknown clusters and a 78-byte
pending high-water. Its 74 HAL reads/3,251 returned bytes cover font load,
XHTML, prewarm and diagnostic drawing, not physical sectors. Host allocations
exclude libc/Expat and are sampled before font teardown; device heap fields are
null. Device records use INTERNAL|8BIT free/largest/minimum separately from
PSRAM. `refresh_ms` spans HAL refresh/gray-service request through observed
completion, including transfer/settle and deferred observation delay—not an
exact BUSY-edge waveform measurement. Reader-exit samples follow parser teardown;
global loaded fonts remain resident under the existing application policy.

## Native CPFont visual gate

**Failed native gate; companion positioning is required.** The production
probe's Noto Sans Thai 16pt normal glyph sheet at tracking 0 shows merged
tone/vowel marks in `กี่` and tall-consonant cases, and colliding descender/lower
vowels in `ญู` and `ฐุ`. The same frozen source face rendered through desktop
HarfBuzz/FreeType separates those marks. Evidence:
`build/thai/native-smoke/stacked-comparison.png` (native left, reference right),
`comparison-000-2x.png`, `report.json`, and `reference-000.json`.
This is visible positional/substitution failure, not an antialiasing-only
difference or missing coverage.

All twelve pinned Thai CPFonts and matching references have been generated;
source hashes and package versions are in `build/thai/assets/manifest.json`.
The immutable fallback IDs supplied for Google Fonts are Git blob IDs, not tree
revisions; preparation verifies decoded Git blob bytes against the required
SHA-256. The baseline executable is preserved under
`build/thai/baseline-firmware/ThaiRenderProbe.exe`. The complete matrix must finish
before final baseline/candidate acceptance.

CPFont v4 stays unchanged. Native files without companions remain valid; no
device HarfBuzz, FreeType or OpenType parser is introduced by this milestone.

## Optional positioning

The opt-in converter bakes 3,772 finite contexts per style into unchanged CPFont v4
glyph records and a `.cpshape` companion. Unencoded alternates use deterministic
unused BMP PUA slots; native Unicode records remain intact. The device consumes
an allocation-free outer-cluster cursor in bounds, measurement, normal drawing,
rotated drawing and source-ordered glyph prewarming. Both measurement modes use
the same signed 12.4 pen/offset rounding; tracking occurs only between outer
clusters. Unsupported clusters remain wholly native.

Companion v1 is little-endian: 32-byte `CPSHAPE\\0` header, 12-byte style TOC entries,
28-byte style headers, 3,772 dense base/suffix ID pairs, eight-byte glyph records
and counted suffix sequences. CRC32 protects the payload and binds recipes to
CPFont header/TOC, intervals, glyph metadata, kerning and ligatures in file order
(excluding bitmaps). Counts, offsets, glyph coverage and metrics are checked before
publishing any style. Limits are 96 KiB/style and 384 KiB/family.

One RAII-owned family payload prefers PSRAM and is accepted only with more than
50 KiB byte-addressable internal headroom. Cache eviction retains it; font
destruction releases it. Missing, corrupt, incompatible or OOM companions keep
the native font usable, with availability fixed for that load. Font identity
includes native metrics CRC, accepted payload CRC, version and active/disabled
state. Layout/render mini-kerning coverage is checked independently of resident
glyphs, and both descriptors are republished after rebuild/failure.

Observed regular companion file sizes across 12/14/16/18 pt:

| Family | Minimum bytes | Maximum bytes |
| --- | ---: | ---: |
| Noto Sans Thai | 51,173 | 56,846 |
| Noto Serif Thai | 50,203 | 53,005 |
| Sarabun | 25,983 | 27,941 |

Seven offline baker tests pass, including every-key HarfBuzz oracle checks,
determinism, original bitmap preservation and byte-identical conversion without
the option. All 499 host tests pass, including real paired-font prewarm/eviction,
corrupt/OOM/headroom fallback and same-size native metric identity changes.
Actual Noto Sans Thai 16pt glyph sheets match reference ink exactly for all
18 supplied rows after applying the specified 12.4/outer-cluster rounding.
Sarabun 16pt and superscript/subscript smoke output also match reference ink;
Serif 16pt retains 200 differing ink pixels in two word rows without broken marks.
Native no-companion output remains pixel-identical across the three saved
baseline glyph pages. Enlarged inspected output is under `build/thai/shaped-smoke/`.

Prepare paired test assets with:

```text
python scripts/prepare_thai_test_assets.py --output build/thai/assets-shaped --thai-shaping --probe build/test/thai_render/ThaiRenderProbe.exe
```

The reference uses production analyzer cluster spans, desktop HarfBuzz/FreeType,
the converter's quantization and the prescribed cluster rounding. Comparisons
match glyph-sheet source rows across metric-driven repagination, not unrelated
pages with the same page number. Exact desktop typography is not required.

## Acceptance evidence

The baseline host build passes 389 enabled tests. Two desired Thai regressions
were exercised and fail before implementation: hospital-word wrapping and
base-style inheritance across `<b>ก</b>ี่`. The deterministic corpus EPUB hash is
`1a88daa0d00a6f215ed00d6b1b1d45ddc02a679c71b44f488fb68b6bda2dba7f`.
The saved X4 Pro baseline firmware builds successfully: 5,663,898 app bytes of
6,553,600, with a 5,668,912-byte binary. No X4 Pro was enumerated by
`pio device list`; device measurements are unverified. Final acceptance was stopped
at the user's request. Device acceptance requires a physical X4 Pro with
identical SD/font/refresh/prewarm settings: 20 cold opens, 20 warm reopens, at
least 50 turns and five open/turn/exit cycles. Report median/p95 and sample counts;
cold/first-page limit is baseline × 1.10 + 5 ms, warm/control limit baseline ×
1.05 + 5 ms. Internal free heap must remain above 50 KiB. Host allocation and
HAL-request counts must be labelled separately from target heap and physical SD
sectors. Physical refresh is timed separately from software layout/draw.

## Published X4 Pro snapshot

The existing build and installable font pack are included in
[`Releases/x4pro-thai/`](../Releases/x4pro-thai/):

- `firmware.bin`: X4 Pro application image, 6,081,296 bytes.
- `firmware.factory.bin`: X4 Pro merged factory image, 6,146,832 bytes.
- `THCraft-Thai-Fonts.zip`: font pack, 9,202,302 bytes, including licenses and
  installation instructions.
- `SHA256SUMS`: SHA-256 checksums for the three artifacts.

These images target **X4 Pro only**, not ESP32-C3 devices. Use the application
image for firmware updates; the factory image is a separate merged flash image,
not an interchangeable update file.

Extract the font pack's `fonts` directory to the SD root, restart, and select
`THCraft-NotoSansThai` as the reader family. Keep matching `.cpfont` and
`.cpshape` files together. The 8/10/12 pt files provide size-matched UI fallback;
14/16/18 pt files provide additional reader sizes. See
[SD card fonts](sd-card-fonts.md) for details.

Publication status:

- The latest `pio run -e x4pro` succeeded. Reported application usage:
  6,076,286 / 6,553,600 flash bytes and 101,864 / 327,680 static RAM bytes.
  Static RAM is not a runtime free-heap measurement.
- The last full host test run passed 499 tests; subsequent host probe/adapter
  changes were not followed by another full-suite run. The UI fallback probe
  subsequently passed all three packaged families with zero failures.
- The full native baseline capture was stopped at 155,662 / 419,840 runs
  (37.1%), containing 301,264 pages. The full candidate matrix was not run.
- The C3 `default` build could not complete because the local RISC-V toolchain
  installation was incomplete. No C3 firmware is included.
- Device visual acceptance, latency and runtime heap/leak gates remain unverified.
  This snapshot is not a completed Milestone 1 acceptance claim.

The firmware images were rebuilt with the THCraft mascot on boot and default
light/dark sleep screens. The 224×336 pre-dithered bitmap uses 9,408 flash bytes
and no image heap buffer. A host smoke executed the theme's mascot drawing method
and renderer coordinate transform in 32 combinations of panel size, orientation,
boot/sleep status and inversion, checking pixel count and layout bounds. Visual
previews used substitute host fonts, not the device font rasterizer.
The built ESP32-S3 application contains exactly one mascot bitmap; the factory
image's bootloader, partition table and application match the build outputs.
`SHA256SUMS` covers both refreshed images and the unchanged font archive.

### Font coverage refresh

The paired font archive now includes IPA/modifier letters, arrows, mathematical
and decorative symbols, and Arabic (including contextual presentation forms).
The converter uses ordered, hash-verified Noto fallbacks without replacing glyphs
already present in the Thai source. All three families retain all six sizes.

Verification of the regenerated archive:

- All 18 pairs retain every previous glyph bitmap and line metric.
- Each pair includes all 176 IPA/modifier codepoints (U+0250–U+02FF), 112 arrows
  (U+2190–U+21FF), 256 mathematical operators (U+2200–U+22FF), and 1,166 Arabic
  glyphs from the bundled Noto Sans Arabic source.
- The font baker suite passes all 10 tests, including fallback precedence,
  later-fallback PUA collisions, Thai shaping oracles, and multilingual coverage.
- The production host renderer completed 24 smoke runs: all family/size glyph
  sheets, plus 16 pt reader layout and rotated sheets for each family. Mixed
  Thai/IPA/symbol/Arabic reader pages were visually inspected.

These are host checks, not physical-device heap or display acceptance. The font
coverage refresh itself required no firmware changes; install the updated
`.cpfont` and `.cpshape` pairs together as described in [SD card fonts](sd-card-fonts.md).
