# Natural Thai Distributed implementation plan

Status: implemented 2026-10-04; host typography verified. Device and Microsoft
Word reference acceptance remain pending. The sections below record the accepted
design and its pre-change evidence; see [Thai reading engine](thai-reading-engine.md#thai-justify)
for the current behavior.

## Goal and release contract

Replace alignment 5's equal, unlimited expansion with natural Thai distribution.
The primary acceptance requirement is to prevent huge justification-added gaps
between Thai units and between words/phrases. Readability takes priority over a
perfectly straight right edge. This is Word-inspired, not a claim of pixel-identical
Microsoft Word compatibility.

Keep alignment value 5, the existing six-option menus, persisted settings, API,
user-over-book-style precedence, and default Justify. Do not add tuning controls
or a second Thai alignment mode. Keep source text, source offsets, dictionary
segmentation, progress, and font file formats unchanged. Replace the old policy;
do not retain it as an alias or fallback.

## Evidence and diagnosis

- `lib/Epub/Epub/ParsedText.cpp:1937–1988` pools ordinary and Thai opportunities,
  divides all spare width equally, and has no expansion limit.
- `lib/ThaiText/ThaiCluster.h:28–42` defines justification in terms of complete
  orthographic clusters. `test/thai_text/ThaiClusterTest.cpp:157–178` makes
  `เพื่อ` indivisible and groups `ภาษาไทย` as `ภา | ษา | ไท | ย`.
- `lib/GfxRenderer/GfxRenderer.cpp:122–175` further rejects opportunities inside
  a consumed shaping span; `179–199` assigns remainder pixels to the first gaps.
  Restrictive opportunities can concentrate expansion at relatively few edges.
- `lib/Epub/Epub/ParsedText.cpp:1358–1386` already selects the latest fitting
  legal non-emergency boundary. Do not resurrect the previously fixed preference
  for an earlier literal space or introduce a paragraph-wide optimizer.
- `lib/EpdFont/ThaiShape.h:7–14` and `ThaiShape.cpp:164–189` expose native glyphs
  and recipe boundaries. `GfxRenderer.cpp:77–118` shares shaped placement across
  measurement and drawing. These are the integration points for finer spacing.
- `lib/Epub/Epub/blocks/TextBlock.h:24–31` stores optional two-byte per-word
  expansion budgets; `Section.cpp:65–83` uses section version 52 with a derived
  partial-cache version.

The user-reported huge gaps are accepted as observed behavior. Their exact page
has not been isolated: this plan fixes unbounded *added* spacing, not all possible
causes of blank space, such as source whitespace, explicit tracking, indentation,
missing glyphs, or font side bearings.

### External typography references

1. [OOXML ST_Jc](https://c-rex.net/samples/ooxml/e1/Part4/OOXML_P4_DOCX_ST_Jc_topic_ID0EXZZ2.html)
   distinguishes equal `distribute` from `thaiDistribute`, which combines word
   spacing with slight character expansion. It does not specify Word's exact
   weights, caps, or line-fitting algorithm.
2. [Microsoft justification opportunities](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_1/ns-dwrite_1-dwrite_justification_opportunity)
   describes priorities, expansion limits, and Thai opportunities inside some
   multi-glyph clusters. Diacritics are not independent spacing opportunities.
   This is design evidence, not proof that Word uses this API or these defaults.
3. [Microsoft Thai shaping](https://learn.microsoft.com/en-us/typography/script-development/thai)
   requires mark-to-base and mark-to-mark attachment and special Sara Am handling.
4. [W3C justification](https://www.w3.org/International/articles/typography/justification)
   describes Thai word-boundary wrapping and glyph-based expansion when spaces
   are absent or would become too large.

**Important distinction:** a safe line-break cluster is not necessarily the
smallest safe spacing unit. Never loosen dictionary/emergency-break boundaries
merely to obtain more justification opportunities.

## Intended spacing policy

### Opportunities

Use three categories rather than one undifferentiated slot count:

- Existing expandable spaces: retain their natural advance and permit bounded
  extra spacing. Do not invent a visible word space at a dictionary boundary.
- Thai spacing opportunities: independent visible base/spacing-letter units,
  with all attached marks moving rigidly with their owner. Leading and trailing
  spacing vowels require explicit fixtures; do not classify every Unicode scalar
  or every emitted glyph as independently movable.
- Protected content: marks, native ligatures, atomic shaping recipes, ruby/focus
  internals, malformed sequences, numbers, and punctuation internals receive no
  new Thai spacing. Preserve existing mixed-script/CJK rules, but never let a
  secondary gap type become an unlimited sink for Thai-line slack.

A base-plus-marks recipe, including reordered/decomposed Sara Am, remains atomic.
Introduce eligible boundaries between independent units *within* a larger TCC,
not inside their mark-attachment recipes. Preserve whole-cluster admission and
fallback for unsupported shaping. Native fallback must enforce the same
attachment safety, though a font-specific ligature can remove an opportunity.

### Bounded allocation

For each non-final Thai-bearing line:

1. Measure the unexpanded line using the exact rendered metrics and user spacing.
2. Count opportunities by category and determine their capacities without
   retaining a per-glyph or per-cluster vector.
3. Distribute positive spare width with weighted, capped allocation. Existing
   spaces receive more expansion per opportunity than Thai letter gaps; when a
   category reaches its cap, redistribute only within remaining capacity.
4. Stop at the total permitted capacity. Leave excess slack at the trailing
   margin. Never force the final pixels into the last gap or fall through to
   ordinary unlimited justification when no safe capacity remains.
5. Preserve natural final lines, overwide atomic content, indentation, and RTL
   anchoring. Do not compress characters, scale glyphs, or split extra words to
   force a full line. English-only paragraphs retain ordinary Justify.

Starting calibration values, **not Microsoft rules**:

- Thai extra-spacing ceiling: 1/24 of the effective font's uncompressed line
  advance, rounded down to whole pixels; no forced one-pixel minimum.
- Existing-space extra ceiling: half its natural rendered space advance before
  user spacing, rounded down. Thus the engine adds at most half a normal space;
  it does not multiply an already large user word-spacing setting.
- Initial per-opportunity weight: space 4, Thai 1. Evaluate against 2:1 and 6:1
  during host visual calibration, then ship one documented set of constants.
- Use the actual resolved font/style and superscript/subscript scale. The font
  has `advanceY`, ascender, and descender, not a guaranteed em metric
  (`EpdFontData.h:177–185`); do not label line advance as em or use the user's
  adjustable line-compression setting as the scale.

These are explicit initial settings for comparison, not validated final values.
Final constants must be selected from side-by-side pages before feature acceptance.
The hard-cap rule itself is not optional. On a line with no spaces, eligible Thai
opportunities alone share the spare width up to their cap.

Use fixed-point/integer arithmetic and checked sums. Spread integer rounding
error across the line and across a token's opportunities instead of putting all
remainder pixels at the left. Respect the integer cap after rounding. Retain a
per-token total budget so replay is deterministic; distribute that budget with
the same cumulative-rounding rule in measurement and drawing. If equivalent
same-style token partitions move an interior origin, allow at most one pixel
of rounding difference, never a cumulative drift or a changed break decision.

## Implementation order

### 1. Establish typographic comparison fixtures

Extend the existing host rendering workflow, not the firmware UI. Include:

- Continuous prose without spaces and prose with phrase spaces.
- Narrow columns, long dictionary words, and short non-final lines with large slack.
- `ภาษาไทย`, `ประเทศไทย`, `เพื่อ`, `เรื่อง`, `กี่`, `น้ำ`, decomposed `นํ้า`,
  below-base marks, repeated/invalid marks, leading/trailing vowels, and ligatures.
- Mixed Thai/English/numbers/CJK/RTL, links, inline style changes, focus and ruby.
- Explicit line breaks versus paragraph endings and streaming-window boundaries.

Use Sarabun, Noto Sans Thai, and Noto Serif Thai at small/medium/large packaged
sizes, both narrow and wide viewports. Record source, font identity, settings,
and build identity with each comparison. Compare left-aligned, old Thai Justify,
and proposed output. Record added spacing separately from native advance/ink gaps.

For actual Word comparison, obtain a reference DOCX plus PDF/screenshots from a
named Word version with Thai Distributed, identical source font/version, text,
and physical text width. Disable unrelated manual tracking; record line spacing
and break settings. Word exports are an external reference requirement, not
currently verified evidence. Do not substitute LibreOffice for Word or use
DirectWrite output as proof of Word equivalence. Without that reference, validate
natural Thai typography but do not claim Word compatibility.

### 2. Separate spacing boundaries from break boundaries

In the existing Thai boundary and shaping files, implement a shared allocation-free
spacing traversal. Keep `nextCluster`, dictionary analysis, and emergency cuts
unchanged. Carry enough source/attachment information through shaped placement to
identify independent units without changing CPFont/CPSHAPE files. Follow native
ligature consumption rather than expanding inside substituted glyphs.

Connect counting, measuring, normal/rotated drawing, and scaled styles to the
same spacing events. Applying expansion only before a whole `placeThaiCluster`
call cannot support interior opportunities; integrate it into the shared placement
traversal. Run LSP references before changing exported cursor/renderer contracts
and migrate every production consumer and test adapter together.

### 3. Replace the line allocator

Replace `positionThaiLine`'s single slot pool with the policy above. Derive word
origins and per-word budgets from the same allocation; preserve source offsets,
styles, link identity, ruby reservations, and exact metric preparation failures.
A bounded/zero-capacity line is successfully handled, not a request for legacy
unlimited fallback. Existing spaces, CJK gaps, and inter-token Thai edges all
require explicit bounded handling in mixed lines.

Keep the latest-legal-fit wrapper and verify it independently. Do not add
compression, multi-line dynamic programming, dictionary changes, or artificial
spaces as part of this cutover.

### 4. Keep cache and interaction geometry consistent

Prefer the current two-byte per-word expansion budget and existing TextBlock
arena. Cached word positions already carry inter-token spacing; the renderer
reconstructs internal spacing from the budget and shared opportunities.

Bump section version from 52 to the next available version (53 if unchanged)
**even if the byte layout is unchanged**: old budgets and origins encode old
spacing semantics. The derived partial version must invalidate suspended builds
as well. Preserve progress, metadata, dictionary analysis identity, and fonts.

Use the new geometry for text, decorations, dictionary selection and link hit
boxes. Keep preview, overlay, settings persistence, and API alignment 5 on the
same path. Update the existing Thai-engine and cache-format documentation after
behavior is verified; no new settings or font repackaging are required.

### 5. Verify behavior and choose final constants

Permanent behavioral regressions must establish:

- Every added Thai/space gap stays within its category's integer cap, including
  across token/style boundaries, zero-slot lines, and mixed-script lines.
- If spare width exceeds total capacity, expansion stops and trailing slack
  remains; no overflow and no hidden unlimited fallback.
- If sufficient capacity exists, allocated extras sum to spare width exactly.
- Final lines and overwide atomic content remain natural; streaming chunking
  does not turn a final line into a stretched line or change word breaks.
- No justification splits a mark attachment or ligature; narrower spacing units
  never become new legal line breaks. Source/progress offsets stay unchanged.
- Count, measure, draw, selection, decoration, and link geometry agree.
- Cache replay matches BW and both grayscale planes; old full/partial caches
  rebuild. Cold/full prewarm, missing shape companions, shaping-disabled builds,
  and allocation failures preserve the existing safety contracts.
- Equivalent text with harmless inline markup does not acquire visible seams;
  rounding differences stay within the stated one-pixel bound.
- English-only and unaffected alignment modes preserve their existing behavior.

Replace tests that require unlimited expansion with bounded-policy assertions.
Delete assertions that merely pin quotient/remainder internals; do not update
magic numbers simply to match a new implementation. Retain mark-safety and
consumer-visible geometry coverage.

Extend `ThaiRenderProbe`'s host report with natural width, allocated extra width,
remaining slack, per-category capacity and maximum actual added gap. Validate
those quantities against observed glyph origins, not only allocator bookkeeping.
Inspect actual rendered pages alongside references: cache equality and filled
line width do not prove good typography.

Run the relevant host suite and production-renderer smoke matrix after integration.
Build the affected C3 and S3 firmware environments once after final code changes;
use the repository formatter wrapper. Then exercise a Thai book on hardware in
all orientations: selection/links, reopening, saved alignment after reboot,
repeated page turns, and cold/cache reads. Require free internal heap above
50 KiB without persistent growth; measure layout latency rather than claiming
an improvement from host timings. Firmware upload requires approval.

## Resource constraints

No retained per-cluster records, paragraph-wide optimization tables, or new
per-glyph allocations. Use existing line scratch and scalar counters, with a
small bounded number of streaming passes. Reuse current per-word budgets and
arena storage. Any necessary scratch growth must be quantified, allocated once
with checked nothrow allocation, and released through the existing lifecycle.
Keep stack locals below the repository's 256-byte guideline. Host report data
must not add firmware telemetry/storage overhead.

## Answer to the reported huge-gap problem

Yes, this design specifically prevents the justification algorithm from creating
unbounded cluster/word gaps. More spacing opportunities may improve texture,
but they are not the guarantee: **hard limits on every participating category,
plus a non-forcing fallback, are the guarantee**.

A very short non-final line cannot simultaneously preserve its words, have tiny
internal gaps, and reach both margins. The chosen behavior is bounded spacing
with a ragged trailing edge. Large source spaces, explicit tracking, or font
bearings remain separate issues and must be distinguished in the comparison
reports rather than silently rewritten.

## Implementation evidence

- Separate allocation-free letter-spacing units now preserve mark recipes while
  retaining the original TCC/dictionary line-break rules.
- Capped weighted distribution uses the original per-word arena budgets and line
  scratch. Section v53 invalidates both full and suspended v52 layouts.
- Mixed-direction smoke exposed missing Thai bidi classes. Adding the UCD Thai
  classes, while keeping Thai marks out of the RTL overlay renderer, fixes the
  reversed Thai runs and their count/draw opportunity mismatch.
- 101 production-renderer configurations passed: 81 font/size/width/weight
  combinations, eight orientation/focus edge cases, and twelve native fallback,
  shape-allocation failure, and shaping-disabled cases.
- The default-policy matrix checked 910 non-final lines. All additions respected
  their caps; total allocation equaled the smaller of spare width and capacity.
  All 27 default cases kept baseline line text and source offsets, and cache
  replay matched BW and both grayscale planes.
- Compared actual pages at weights 2, 4, and 6; retained 4 with the planned caps.
  In the comparison corpus, maximum internal added spacing fell from 218 px
  (old policy) to 4 px (new policy). Maximum added ordinary-space width was 7 px.
- Reproducible XHTML fixtures live in `test/language/Thai/`. Local generated
  reports, before/after images, weight comparisons and `summary.json` are under
  `build/thai/distributed-comparison/`; they are not release artifacts.
- No Word reference export or physical-device reading acceptance was available.
  No firmware upload or release-package replacement was performed.
- All 605 host tests and the `default`/`x4pro` firmware builds passed. The final
  production-renderer edge smoke also passed after the ESP32 integer-type fix.
