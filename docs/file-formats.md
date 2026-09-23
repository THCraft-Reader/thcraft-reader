# File Formats

EPUB cache files live under `/.crosspoint/epub_<hash>/`; native TXT cache files
use the separate per-book directory described below.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8.

## `book.bin`

### Version 10

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 10
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

### Version 48

Version 48 combines the two version-47 layouts: a `u8 representation` before
every TextBlock (`0 = legacy`, `1 = native`), a little-endian
`u64 textLayoutFingerprint` after the Section header's Focus Reading flag,
then `s8 characterSpacing` and `u8 wordSpacingPercent`. The header is **51 bytes**.
Finalized sections use version 48; suspended partial sections use
`0xFE - (48 - 28) = 0xEA`, with the same header and Page representation.
Version 0 remains an incomplete, unreadable build. All trailing header offsets
and page-count seeks are relative to the revised header size.
Both version-47 formats and all older complete/partial caches are rejected.
The fingerprint starts at byte 19, spacing at bytes 27 and 28, pageCount at
byte 29, and the five `u32` offsets at bytes 31, 35, 39, 43 and 47.

The fingerprint is zero for the legacy backend. Native fingerprints use stable
FNV-1a-64 over canonical engine/resource identity: `native-text-v1`, pinned
FreeType/HarfBuzz and dictionary revisions/digests, selected font-source content,
all fallback faces, variation/synthetic-style configuration, DPI/load/metric
policy, segmentation revision, and native record policy (`record=overflow-clip-spacing-v3`;
`tracking=visual-clusters`; `word-spacing=unicode-space-advance`).
It is calculated from resource identities
at load/change, not by rereading fonts for every page. Both finalized and partial
loads compare it before exposing pages. A mismatch retains the prior cache until
a replacement is committed; EPUBs, book metadata, progress and bookmarks are unchanged.

Native TextBlocks store the original logical UTF-8, not glyph IDs or visual-order
strings. There are no inserted Thai dictionary spaces. A deserialized line is
shaped through the same engine and warmed without painting. The field-by-field
record, immediately after representation 1, is:

| Field | Encoding |
|---|---|
| textBytes, spanCount, wordCount, gapCount, rubyCount | five `u16` values |
| paragraphLevel | `s8`, resolved paragraph level 0 or 1 |
| lineHeight, baseline, rubyLift | three `s16` values, pixels |
| alignmentX26 | `s32`, horizontal alignment in signed 26.6 pixels |
| syntheticSuffixCp | `u32`, 0 or U+002D; no source span |
| overflowClipWidth | `u16`, 0 for normal lines, otherwise content-box width in pixels (1–32767) |
| characterSpacing | `s8`, cluster-safe tracking in pixels, -2 through 2 |
| wordSpacingPercent | `u8`, real-space advance percentage, 50 through 200 |
| logical text | exactly textBytes UTF-8 bytes, no terminator |
| style/bidi spans | spanCount × (`u16 startByte,endByte`, `u8 style,bidiLevel`) |
| selectable words | wordCount × (`u16 startByte,endByte`, `s32 x26,width26`, `s16 top,height`) |
| expanded real-space/CJK gaps | gapCount × (`u16 byteOffset`, `s32 extraAdvance26`) |
| ruby annotations | rubyCount × (`u16 baseStartByte,baseEndByte,textBytes`, `s32 x26,y26`, `u8 style`, textBytes UTF-8 bytes) |
| BlockStyle | 24 bytes in the field order shown below, including characterSpacing |

Ranges are half-open byte ranges in the original logical text. Words are in
logical source order; their x positions are final visual positions relative to
alignmentX26. Word top/height and the line baseline are relative to the line top.
Ruby x already includes alignment; ruby y is the annotation baseline relative to
the line top. Native focus emphasis lives in style spans, not a second per-word
geometry array. Word styles and NUL-terminated selection strings are reconstructed
from those spans/text on decode. Thai dictionary boundaries have no gap records.
The lineHeight is the minimum ink/decorations/ruby height; layout may add
compressed nominal line spacing but cannot shrink below this minimum.

Only a forced indivisible cluster/group wider than its available line receives
overflowClipWidth. Painting intersects the caller's clip with
`[lineOriginX, lineOriginX + overflowClipWidth)` and restores the caller's clip
afterward, including on failure. Words and links are clipped to that same box
after alignment; source bytes and shaped advances are unchanged. Ordinary lines
retain italic overhang and negative first-line indentation. The fixed native
header is 29 bytes after the representation byte, including overflow width and
both spacing fields. Native rendering reapplies spacing while reshaping the
logical text; gaps remain nonnegative justification deltas, never tracking.

Decoding caps logical text at 16 KiB / 4096 Unicode scalars, each metadata count
at 4096, and combined ruby text at 16 KiB. It checks UTF-8 boundaries, nonoverlapping
source-ordered spans/words/ruby, valid bidi levels, supported spacing ranges,
nonnegative dimensions, bounded coordinate sums, overflow width/word containment and ruby lengths before
allocating the metadata arrays. Native buffers and Pro TextBlock control objects
count against the shared 4 MiB engine budget. Truncated or malformed
records return an invalid page, never partially initialized geometry. Page
element counts are capped at 1024; footnotes at 16 and links at 32. Legacy arena
bytes are unchanged after the representation 0 prefix, with checked lengths,
offsets, strings and scalar reads. Both representations append `characterSpacing`
after BlockStyle's `directionDefined`; for legacy blocks word spacing is already
resolved into cached word positions, while native lines persist both values.

### Version 46

Version 46 keeps the version 45 serialized layout unchanged. It was bumped
because ordered lists now number their items, `list-style-type: none`
suppresses list markers, and `<ul>`/`<ol>` containers contribute their own
margins and padding to child block insets, changing cached word contents and
page layout.

### Version 45

Version 45 keeps the version 44 serialized layout unchanged. It was bumped
because internal EPUB links now preserve CSS superscript and subscript styles,
changing their cached word-style flags and page layout.

### Version 44

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 44 appends the internal-link rectangles produced during text layout to
each serialized page. The reader uses these rectangles for touch navigation;
older caches are rebuilt because they contain no link geometry.

Version 43 keeps the version 42 serialized layout unchanged. It was bumped
because paragraph base direction now excludes direction changes from inline
elements.

Version 42 keeps the version 41 serialized layout unchanged. It was bumped
because closing a block now strips inherited vertical margins and padding.

Version 41 keeps the version 40 serialized layout unchanged. It was bumped
because simple HTML table rows are now laid out as positioned columns rather
than flattened paragraphs with synthetic row/cell labels.

Version 40 keeps the version 39 serialized layout unchanged. It was bumped
because ruby groups now remain intact when large text blocks are soft-flushed.

Version 39 keeps the version 38 serialized layout unchanged. It was bumped
because image top margins are now clamped to keep full-height images within the
page viewport.

Version 38 keeps the version 37 serialized layout unchanged. It was bumped
because Focus Reading now permits line breaks at visible hyphens and dashes
and hyphenates focus-split words as a whole, changing cached page layout.

Version 37 increases the fixed-size footnote href field from 96 to 256 bytes.
This changes each serialized footnote record from 128 to 288 bytes, so older
section caches must be discarded and rebuilt.

Version 36 keeps the version 35 serialized layout unchanged. It was bumped
because ruby and justified text positioning and CJK line breaking now use
corrected word measurements, so version 35 cached page layouts no longer match.

Version 35 adds a header offset and a `uint32_t` entry per page for the
visible-text offset LUT. The other section LUTs remain unchanged.

Version 34 is binary-identical to version 33. The version was bumped because
word-gap suppression was narrowed to tokens glued together in the source: v33
dropped the gap between any two words meeting at a CJK break opportunity, which
collapsed the spaces between Hangul words, so v33 word positions no longer match
what the layout engine now produces.

Version 30 is binary-identical to version 29. The version was bumped because
Arabic contextual shaping changed text measurement (`getTextAdvanceX` now
measures the shaped visual text), so word positions cached by v29 no longer
match what `drawText` renders.

Version 28 introduced serialized word style bits for underline, strikethrough,
superscript, and subscript. The format also includes:

- cache-busting fields for paragraph alignment, hyphenation, embedded CSS,
  image rendering mode, and Focus Reading
- page offset LUT
- per-page visible-text offset LUT (zero-based Unicode codepoints in `<body>`)
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs retained for navigation and legacy sync fallback
- optional per-word Focus Reading split metadata
- per-page footnote entries
- serialized word style bits for underline, strikethrough, superscript, and
  subscript
- flat TextBlock word storage (v29): per-word arrays plus one shared
  NUL-terminated text blob, replacing v28's length-prefixed word strings. The
  on-disk order mirrors the in-RAM arena so the firmware reads a whole block
  payload with a single allocation and a single SD read

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 48
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 256

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageHorizontalRule = 3
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
    s8 characterSpacing;
};

struct LegacyTextData {
    u16 wordCount;
    u8 hasFocus;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasFocus != 0) {
            u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasFocus != 0) {
            u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        }
        char text[textBytes] [[comment("All words back to back, each NUL-terminated")]];
    }

    String rubyTexts[wordCount];
};

struct NativeStyleSpan {
    u16 startByte;
    u16 endByte;
    u8 style;
    u8 bidiLevel;
};

struct NativeWord {
    u16 startByte;
    u16 endByte;
    s32 x26;
    s32 width26;
    s16 top;
    s16 height;
};

struct NativeGap {
    u16 byteOffset;
    s32 extraAdvance26;
};

struct NativeRuby {
    u16 baseStartByte;
    u16 baseEndByte;
    u16 textBytes;
    s32 x26;
    s32 y26;
    u8 style;
    char text[textBytes];
};

struct NativeTextData {
    u16 textBytes;
    u16 spanCount;
    u16 wordCount;
    u16 gapCount;
    u16 rubyCount;
    s8 paragraphLevel;
    s16 lineHeight;
    s16 baseline;
    s16 rubyLift;
    s32 alignmentX26;
    u32 syntheticSuffixCp;
    u16 overflowClipWidth;
    s8 characterSpacing;
    u8 wordSpacingPercent;
    char text[textBytes];
    NativeStyleSpan spans[spanCount];
    NativeWord words[wordCount];
    NativeGap gaps[gapCount];
    NativeRuby ruby[rubyCount];
};

struct TextBlock {
    u8 representation;
    if (representation == 0) {
        LegacyTextData legacy;
    } else if (representation == 1) {
        NativeTextData native;
    } else {
        std::error("Invalid text representation");
    }
    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    String srcPath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
};

struct PageLink {
    char href[FOOTNOTE_HREF_LEN];
    s16 x;
    s16 y;
    s16 width;
    s16 height;
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];
    u16 linkCount;
    PageLink links[linkCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION && version != 0xEA) {
        std::error(std::format("Unsupported version: {}", version));
    }

    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool focusReadingEnabled;
    u64 textLayoutFingerprint;
    s8 characterSpacing;
    u8 wordSpacingPercent;

    u16 pageCount;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;
    u32 visibleTextLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }

    if (visibleTextLutOffset != 0) {
	u32 visibleTextOffset[pageCount] @ visibleTextLutOffset;
    }
    if (version == 0xEB) {
        u32 bytesConsumed @ (visibleTextLutOffset + pageCount * 4);
        u32 totalBytes @ (visibleTextLutOffset + pageCount * 4 + 4);
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## Native TXT cache — X4 Pro

Native TXT and literal Markdown reading use `<Txt::getCachePath()>/native/`.
This directory is separate from the root legacy TXT `index.bin` (version 3)
and four-byte `progress.bin`; native firmware neither overwrites nor deletes
those legacy files. Other devices continue using the legacy formats.

### `native/index.bin` — version 2

Fields are serialized explicitly in little-endian order, with no struct
padding. The file contains a **42-byte header**, serialized `Page` bodies,
then `pageCount` **16-byte lookup records** at `lutOffset`.

| Header offset | Field | Encoding |
|---|---|---|
| 0 | magic | `u32`, `0x4E545854` (bytes `TXTN`) |
| 4 | version | `u8`, 2 complete; 0 incomplete and unreadable |
| 5 | sourceSize | `u32`, original file size in bytes |
| 9 | sourceHash | `u64`, FNV-1a-64 of every original file byte |
| 17 | textLayoutFingerprint | `u64`, native engine/resource and TXT adapter/serializer identity |
| 25 | fontId | `s32`, logical reading font ID |
| 29 | viewportWidth | `u16`, content width in pixels |
| 31 | viewportHeight | `u16`, content height in pixels |
| 33 | alignment | `u8`, existing paragraph-alignment setting |
| 34 | pageCount | `u32`, number of real pages; excludes the end-of-book screen |
| 38 | lutOffset | `u32`, absolute byte offset of the lookup table |

Each lookup record is:

| Record offset | Field | Encoding |
|---|---|---|
| 0 | sourceStart | `u32`, first original source byte consumed by this page |
| 4 | sourceEnd | `u32`, exclusive end of the consumed source range |
| 8 | pageOffset | `u32`, absolute start of the serialized `Page` |
| 12 | pageBytes | `u32`, exact serialized body length |

Source ranges are contiguous, start at zero, and end at `sourceSize`. The
initial UTF-8 BOM belongs to the first page's source range but is not drawn.
LF consumes one source byte; CRLF consumes two. Physical line endings are
not glyphs. Other source spaces and blank lines are preserved; a final line
without a newline is flushed normally. Only zero-byte and BOM-only sources
have zero real pages and show the empty-file surface. Blank-line-only sources
retain their line-height layout.

Bodies use the existing `Page`/`PageLine` serializer and representation-1
native `TextBlock` format above, not a separate TXT glyph format. They retain
logical UTF-8 and completed geometry from sequential paragraph layout,
including resolved bidi context across pages. Loading a backward or skipped
page therefore does not restart dictionary segmentation at an arbitrary
source byte. Font handles, FT/HB pointers and process-local glyph IDs are
never persisted.
Version 2 uses the spacing-aware native TextBlock and 24-byte BlockStyle from
section version 48. Version-1 indexes are rebuilt, preserving native progress.

Opening validates source size **and content hash**, font/fallback/resource
fingerprint, viewport and alignment. A same-length edit invalidates cached
text. The fingerprint includes the TXT adapter/serializer revision as well
as the shared native engine policy. Oriented safe margins, user margins and
status-bar reservation determine the stored content viewport. Changed layout
settings reflow the book without changing its source-byte progress.

Readers reject truncated headers, versions other than 2, overflow/out-of-file ranges,
nonmonotone or overlapping page/source ranges, and malformed or incorrectly
bounded `Page` bodies. Cache corruption is rebuildable; source read errors,
malformed UTF-8, native allocation failure and text-rendering failure are
reader errors, not an empty book.

Indexing holds one pending layout window and one page. It writes Page bodies
to `index.bin.tmp` and lookup records to `index.lut.tmp`, streams the lookup
file into the completed index, patches counts/offsets, and writes version 2
last. Handles close before publication by rename or temporary-file cleanup.
An interrupted or failed build must not expose a partial page count as a
completed book, or replace usable progress with a failed position. The source
hash is accumulated during initial indexing, without a redundant prescan;
cached reopen streams the source to check its hash.

### `native/progress.bin` — version 1

This is an explicitly serialized **21-byte** payload, saved using
`ProgressFile::writeAtomic`:

| Offset | Field | Encoding |
|---|---|---|
| 0 | magic | `u32`, `0x52505854` (bytes `TXPR`) |
| 4 | version | `u8`, 1 |
| 5 | sourceByteOffset | `u32`, first source byte of the successfully displayed page |
| 9 | sourceSize | `u32`, source size at that save |
| 13 | sourceHash | `u64`, complete original-file FNV-1a-64 |

Progress is saved only after successful page, status-bar and enabled
anti-alias rendering. Unchanged saves are skipped. Empty files and
layout/render failures do not overwrite the previous progress.

For matching source content, lookup finds the page containing the saved
byte after font, orientation, viewport or native-fingerprint changes. When
source content itself changes, restoration clamps the old byte to the new
file bounds and chooses the containing/preceding page; this is logged as
best effort, not a semantic text match.

Without valid native progress, the reader first consults the root legacy
progress and a bounds-checked version-3 legacy TXT index. A valid legacy page
entry supplies its source byte; the old font/viewport need not match. If that
index is absent or invalid, the four-byte page-number progress alone cannot
provide an exact source location: its page number is clamped once against the
new page count, and the successfully displayed page's byte position becomes
native progress. Missing progress starts at the beginning. Legacy progress,
EPUB bookmarks and synchronization formats remain unchanged.

## CLX1 — library index (`.crosspoint/library.idx`)

Written by `lib/LibraryIndex/LibraryBuilder.cpp`, read by `LibraryIndexFile`. One
file describing every book on the card, so the shelf can sort and search
thousands of titles without opening any of them.

Format version 2. An index written by another version fails validation on open
and is rebuilt; that is the entire migration mechanism.

### Layout

| Section | Offset | Contents |
|---|---|---|
| Header | 0 | 64 bytes, `ClixHeader` |
| Folders | `folderStart` | length-prefixed paths, one per folder |
| Records | `recordStart` | `bookCount` × 128-byte `ClixRecord` |
| Permutations | `permStart` | `bookCount` u16 author order, then `bookCount` u16 arrival order |
| Name blob | `nameStart` | per record: path hash, name, canonical author, title, source author (see below) |

The arrival permutation runs oldest first, keyed by the record's FAT
modification time (when the file landed on the card); `firstSeen` — the
build-assigned discovery counter — breaks ties and carries books whose
filesystem reports no time. Fold version 3 introduced the timestamp key; a
fold bump rebuilds ranks while preserving `firstSeen`.

Sections are 512-byte aligned so each starts on an SD block boundary.

### Records are exactly 128 bytes

A fixed stride is what lets the reader seek straight to record *n* without an
offset table, and read a screenful in one 4 KB block. `static_assert` enforces it.

Each record carries `fold[96]`, the title normalised for search and sorting —
accents stripped, case dropped, leading articles removed — and `authorKey[12]`,
the author's words folded and sorted so that "Victor Hugo" and "Hugo Victor" group as
one person. `authorKey` is a GROUPING key, not an ordering one: the shelf orders by
surname, derived separately from the display name.

The byte before the folded title records metadata extraction status: not
attempted, extracted, or failed. The final four bytes contain the packed FAT
modification date and time returned by SdFat. A zero timestamp is not trusted.
These fields occupy the alignment and reserved bytes from version 1, so the
record remains exactly 128 bytes.

The header records whether EPUB metadata extraction was enabled for the build.
This prevents a metadata-disabled rebuild from making filename fallbacks look
fresh to a later metadata-enabled build.

### The name blob

Per record, at `nameStart + nameOff`:

```text
[u64 pathHash]    FNV-1a fingerprint of the complete path
[nameLen bytes]  filename, without the directory
[u8][author]     display author, one spelling chosen per authorKey across the library
[u8][title]      the book's own title, or length 0 if it never gave one
[u8][source]     cleaned author spelling before the library-wide spelling vote
```

The filename must stay the first textual field and stay the filename: `readPath`
rebuilds a book's path from it, so writing the display title there makes the book
impossible to open. That was a real defect, and it is why title has its own field.

The source author is separate from the displayed canonical author so a later
rebuild can repeat the spelling vote after books are added or removed. Existing
display reads still stop at the author or title fields and retain their offsets.

### Freshness and unchanged rebuilds

Reconciliation treats the persisted 64-bit complete-path fingerprint as the
book identity. Metadata is reused only when the fingerprint, size, nonzero FAT
timestamp, fold version, metadata mode, and expected extraction status agree.
EPUBs with a zero timestamp or a previous extraction failure are parsed again.

If every current record reuses metadata, the old and new counts agree, and no
unreadable entry was seen, the staging files are discarded and the live index is
left byte-for-byte unchanged. A normal rebuild action is therefore a freshness
check, not a forced metadata reread.

### Header flags

`RANKS_DEGRADED` says one or more orders fell back to walk order because a
checked sort allocation failed. Title and author each use a phase-local
`SortKey[bookCount]` allocation (14 bytes per book, 57,344 bytes at the 4,096-book
format ceiling); the first array is released before the second is requested.
Sorting is therefore best effort through the full format limit rather than
being disabled at an arbitrary library size.

`DEDUP_DEGRADED` says a directory exceeded the fixed 1024-entry duplicate-key
buffer, or that its fallible 8 KiB allocation failed. The walk still indexes
every enumerated book; it only stops remembering additional identities for
duplicate-dirent detection, so a damaged FAT may expose duplicates but cannot
make a real book disappear.

`selfSize` is the expected file size. Comparing it against the real one is a free
truncation guard: a build cut short by a power failure cannot pass.
