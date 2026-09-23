# SD Card Fonts

CrossPoint supports loading additional fonts from the SD card, including fonts
with extended Unicode coverage (CJK, Cyrillic, Greek, etc.).

**Xteink X4 Pro uses native `.ttf`/`.otf` fonts. All other devices use `.cpfont`.**
The Pro does not render `.cpfont` files; the conversion and size-specific CJK
instructions below apply only to non-Pro devices, including X4 Classic and other
ESP32-S3 boards.

## Installing Fonts

There are three ways to install fonts:

### Option 1: Download from device (recommended)

1. Connect your CrossPoint reader to Wi-Fi
2. Go to **Settings > Reader > Manage Fonts**
3. Browse available font families and tap to download
4. Downloaded fonts appear immediately in **Settings > Reader > Font Family**

On X4 Pro this downloads original native font sources from the catalogue shipped
with the firmware; other devices download pre-built `.cpfont` files. See
[Native catalogue and variable fonts](#native-catalogue-and-variable-fonts) for
the Pro's source pins and variation settings.

### Option 2: Upload via web browser

1. Start **File Transfer** and connect through **Join Network** or **Create Hotspot**
2. Open the web interface URL shown on the reader
3. Navigate to the **Fonts** tab
4. On **X4 Pro**, upload `.ttf`/`.otf` files named as described below. On **other
   devices**, upload `.cpfont` files.
5. Select the installed family under **Settings > Reader > Font Family**.

The Fonts tab shows the connected device's accepted formats. Upload styles for
one family at a time, not a mixed-family selection. A new native family must
include its Regular style.

### Option 3: Manual SD card copy

1. On **X4 Pro**, obtain the original TTF/OTF font files and use the native naming
   convention below. On **other devices**, download pre-built `.cpfont` families
   from the [crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts)
   or convert your own fonts using the legacy instructions below.
2. Copy each family folder to one of two locations on your SD card:

   - `/.fonts/` — hidden directory (preferred; keeps the SD root tidy
     when mounted on a desktop)
   - `/fonts/` — visible directory (use this if your OS hides dot-files
     and you'd rather see the folder in your file manager)

   Both roots are always scanned at boot and the results are merged: a
   family installed in `/fonts/` shows up even when `/.fonts/` also
   exists, and vice versa. The two roots only collide if the same family
   name appears in both — in that case the copy in `/.fonts/` wins and
   the duplicate in `/fonts/` is ignored.

   Native X4 Pro example:

       .fonts/
       └── MyFont/
           ├── MyFont-Regular.ttf
           ├── MyFont-Bold.ttf
           ├── MyFont-Italic.ttf
           └── MyFont-BoldItalic.ttf

3. Safely eject the SD card, insert it, and restart the reader.
4. Select **Settings > Reader > Font Family**.

## X4 Pro: Native Fonts

### Naming and supported formats

Use a family folder/name of **1–31 ASCII letters, digits, hyphens or underscores**.
The filename must match that name exactly, followed by `-Regular`, `-Bold`,
`-Italic` or `-BoldItalic`, then `.ttf` or `.otf`. Style suffixes are
case-sensitive; extensions are case-insensitive and uploads normalize them to
lowercase. For example, `/.fonts/MyFont/MyFont-Regular.otf` is valid. Do not add
a point-size suffix, install two files for the same style, or rename `.cpfont`
files to `.ttf`.

Regular is required; the other three styles are optional. Missing styles use
the regular face with synthetic bold or a 12-degree oblique. Fonts must contain
scalable SFNT TrueType or CFF outlines and a Unicode character map. The reader
validates the font itself, not just its extension: TTC collections, WOFF files,
bitmap-only fonts, corrupt files and non-font data are not supported. Incomplete
`.part` files are not discovered as fonts.

One native family supplies all reading sizes **12, 14, 16 and 18 pt**, plus UI
fallback at **8, 10 and 12 pt**. No separate files, conversion or Unicode interval
presets are needed for those sizes.

### Uploads, replacement and migration

Native web uploads stage and validate the entire selection before publishing it.
Uploading one or more styles replaces those styles and preserves the family's
untouched styles and their variation settings. A cancelled or rejected upload
does not replace the existing installation. Mixed-family uploads are rejected.

An on-device download instead replaces the **whole native family** as one
transaction, including its variation metadata: obsolete native styles are
removed only after the new files have been downloaded and validated. Downloads
check expected sizes and CRC32 as well as font validity. Cancellation, invalid
data or a source-download failure leaves the previous family in place; detected
publication failures roll back the update. This is not a guarantee against
power loss or a failing SD card. Manual SD copies do not use this transaction:
finish copying the complete family before restarting the reader.

Deleting or replacing a native family on Pro affects recognized native font
files and `native-font.json`, not co-located `.cpfont` or unrelated files.
Keeping legacy fonts on the same SD card is supported.

If a saved `.cpfont` family has no native replacement, Pro uses the configured
bundled Sans/Serif family and shows a migration notice once per boot/selection.
It retains both the saved family name and the legacy files. Install a valid
native family with the same name to restore that selection. An invalid native
font or sidecar rejects the family rather than mixing a partially loaded family
with its old styles.

### Scalable fallback and Thai

In EPUB and TXT, the selected native family is used when it covers the complete
shaping cluster; missing clusters use bundled script fallbacks. A base character
and its combining marks stay together in one face. UI text keeps its built-in
primary face and can use the selected native family for missing clusters,
including CJK when that family covers them. Unlike the legacy whole-string
fallback below, a mixed title need not switch its entire string to the SD font.
Characters absent from both the selected family and bundled fallbacks still
display a replacement character.

Thai fonts and the offline word-segmentation dictionary are bundled for UI,
EPUB and TXT. Neither an SD font pack nor an SD segmentation dictionary is
required. Word segmentation is separate from the optional StarDict dictionaries
used to look up definitions.

### Native catalogue and variable fonts

Pro's download catalogue is compiled into the firmware from the existing
`lib/EpdFont/scripts/sd-fonts.yaml` source recipes. It is not a separate live
native-font service: catalogue changes arrive with firmware updates, while
downloading font bytes still requires a network connection. The generator pins
GitHub sources to immutable commits and records source digests in
`native-font-sources.lock.json`; other HTTPS sources are content-pinned.
Downloads check expected sizes, CRC32 values and native font validity, and
require HTTPS, including redirects.

The checked-in `native-font-catalogue.json` has `version: 1`,
`format: "opentype"`, `scriptGroups` and `families`. Each family's `files` entries
contain `name`, `size`, `crc32`, `url` and optional `axes`. Variable sources are
downloaded as original SFNT files, not pre-rasterized or converted to static
instances.

When a recipe specifies variation axes, the installer writes `native-font.json`
beside the font files. Its schema uses `version: 1` and a `styles` object whose
keys are `regular`, `bold`, `italic` and **`boldItalic`**. Only styles with
explicit axes need entries. For example, a manually installed two-style Inter
family can retain the catalogue's weight/optical-size choices with:

```json
{
  "version": 1,
  "styles": {
    "regular": {
      "file": "Inter-Regular.ttf",
      "axes": { "wght": 400, "opsz": 14 }
    },
    "bold": {
      "file": "Inter-Bold.ttf",
      "axes": { "wght": 700, "opsz": 14 }
    }
  }
}
```

Each `file` must name the matching installed style. Up to eight axes per style
are supported, with four-character tags and finite numeric values. Unknown axes
are rejected; values for supported axes are clamped to the face's range.
Manually copied or uploaded fonts need no sidecar and use default variation
coordinates when none are specified. Uploading a replacement style without axes
resets that style to its defaults; untouched styles retain their settings.

Native font content and variation settings participate in layout-cache identity.
See [File Formats](file-formats.md) for cache details and
[Native Text Licensing](native-text-licensing.md) for engine/font attribution and
redistribution requirements.

## Legacy Devices: cpfont

Everything in this section applies to **non-Pro devices only**. Keep the
size-specific `.cpfont` files together under the family folder, for example:

    .fonts/
    └── Literata/
        ├── Literata_12.cpfont
        ├── Literata_14.cpfont
        ├── Literata_16.cpfont
        └── Literata_18.cpfont

### CJK in the User Interface

The built-in UI fonts are Latin-only, so by default the interface (book titles
in the library, file names in the browser, list rows, headers) shows
replacement boxes for Chinese/Japanese/Korean text even when book *content*
renders correctly with a selected SD-card font.

To avoid shipping a large CJK glyph set in flash, CrossPoint instead reuses the
SD-card font you already selected: when a UI string contains a CJK character
the built-in font cannot draw, that whole string is rendered with your selected
SD-card font instead.

The fallback is **size-matched**. The built-in UI fonts render at 8 pt
(small/author lines), 10 pt (list rows) and 12 pt (book-cover titles, headers),
so CrossPoint loads your SD family at those sizes too and maps each UI font to
its same-size SD font. CJK book names therefore appear at the same size as the
Latin text around them. For this to work the family must contain `.cpfont`
files at sizes **8, 10 and 12** (in addition to the reader sizes 12–18); any UI
size missing from the family simply keeps showing boxes for CJK at that size.

Note that **Settings > Reader > Font Size** lists every size the family ships,
so a family built at 8,10,12,14,16,18 offers all six as reading sizes — the UI
sizes are not hidden from the list. Reading at 8 pt is your call; if you would
rather not see the small sizes there, convert two families (one with the UI
sizes for fallback, one with only the reading sizes you want).

When converting your own font, include the UI sizes:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyCJKFont-Regular.otf \
      --intervals cjk \
      --sizes 8,10,12,14,16,18 \
      --style regular \
      --name MyCJKFont \
      --output-dir ./MyCJKFont/

What this means in practice:

- Select a CJK-capable SD font under **Settings > Reader > Font Family**
  (see [Installing Fonts](#installing-fonts) and the `cjk` / `hangul` presets
  under [Converting Custom Fonts](#converting-custom-fonts)). That single
  selection drives both book content *and* size-matched CJK fallback in the UI.
- Pure-Latin UI strings keep the crisp built-in font; only strings that
  actually contain CJK are routed to the SD font.
- The fallback is per *string*, not per glyph: a mixed title such as
  `三体 Vol.1` renders entirely in the SD font (including the Latin part). If
  that SD font is a `Mono` family, the Latin portion will appear half/full
  width.
- If no SD font is selected (a built-in reading font is active), there is no
  CJK fallback and the UI again shows boxes for CJK — pick a CJK SD font to
  restore it.

### Available Pre-Built Fonts

The current list of pre-built fonts is maintained in the
[crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts).

### Converting Custom Fonts

To convert your own TrueType/OpenType fonts:

#### Prerequisites

    pip install freetype-py fonttools

#### Single font (one style)

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

#### Multi-style font

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

#### Available Unicode interval presets

| Preset | Coverage |
|--------|----------|
| `ascii` | U+0020–U+007E (Basic Latin) |
| `latin1` | U+0080–U+00FF (Latin-1 Supplement) |
| `latin-ext` | European languages (Latin + Extended-A/B + punctuation + ligatures) |
| `greek` | Greek + Extended Greek |
| `cyrillic` | Cyrillic + Supplement |
| `hebrew` | Hebrew + Alphabetic Presentation Forms |
| `arabic` | Arabic + Supplement + Extended-A + Presentation Forms A/B (RTL, contextual shaping) |
| `georgian` | Georgian + Georgian Supplement |
| `armenian` | Armenian |
| `ethiopic` | Ethiopic + Extended |
| `vietnamese` | Vietnamese subset (ơ/ư and combining marks) |
| `ipa-chars` | IPA Extensions + Spacing Modifier Letters (phonetic transcription) |
| `punctuation` | General punctuation (U+2000–U+206F) |
| `cjk` | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth |
| `hangul` | Korean Hangul syllables + Jamo + Compatibility Jamo |
| `cherokee` | Cherokee (historic + supplement block) |
| `tifinagh` | Tifinagh |
| `symbols` | Math, currency, arrows, box-drawing, misc symbols, dingbats |
| `reading` | Literary fiction coverage: Latin, Greek, Cyrillic, math/symbol blocks, supplemental punctuation, and CJK quote marks |
| `builtin` | Matches the firmware's built-in font conversion intervals |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

You can also specify arbitrary Unicode ranges directly:
`--intervals latin-ext,(0x2100-0x214F)`

To list all presets with codepoint counts:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py --list-presets

#### Additional options

`--force-autohint` — force FreeType's auto-hinter instead of the font's native hinting (useful when a font's built-in hints produce poor results at small sizes).

Install custom fonts via the web interface or manual SD card copy.
