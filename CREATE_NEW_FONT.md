# Create a new Thai SD-card font

Agent workflow for adding a user-supplied Thai font to THCraft Reader. This creates
paired bitmap fonts, not a new typeface and not a firmware-embedded font.

## Invoke this workflow

Tell the agent:

> Follow `CREATE_NEW_FONT.md`. Convert `/path/to/Font-Regular.ttf` into the SD-card
> family `MyThaiFont`. Keep all existing families unchanged. Produce the paired
> fonts, license/source notices, conversion reports, a per-family ZIP, and visual
> verification. Do not build or publish firmware unless I also request it.

Default deliverable: a regular face at **8, 10, 12, 14, 16, and 18 pt**, at the
converter's fixed **150 DPI**. Small sizes also support Thai UI fallback. Add
larger sizes or real bold/italic faces when requested; do not manufacture styles.

## Rules for the agent

- Read the repository development rules, [font documentation](docs/sd-card-fonts.md#thai-bitmap-positioning),
  and [Thai rendering guidance](docs/thai-reading-engine.md#optional-positioning).
- Use `lib/EpdFont/scripts/fontconvert_sdcard.py` with **`--thai-shaping`**.
  Every `.cpfont` must stay beside its same-basename `.cpshape`.
- Work under an isolated `build/` directory. Never overwrite the supplied font,
  existing family assets, or the current complete release ZIP while experimenting.
- Treat source coverage, shaping quality, and licensing as separate checks.
  Successful conversion is not proof of correct Thai marks or distribution rights.
- Reuse the existing converter and host renderer. Do not modify firmware,
  global font metrics, Thai line breaking, or unrelated families to fix one font.
- Do not claim device heap usage, speed, or e-ink quality from host measurements.
- No firmware rebuild is needed to install paired fonts on firmware that already
  supports CPSHAPE. Do not run the firmware release workflow automatically.

## 1. Select inputs and prepare desktop tools

Use Python 3.11 or newer in a repository-root shell on macOS/Linux or Windows Git
Bash. Detect the host with `uname -s`. Activate an existing Python environment
with these dependencies (for example, `source .venv/bin/activate` on macOS/Linux);
otherwise create/activate one before installing them:

```bash
python3 -m pip install -r lib/EpdFont/scripts/requirements.txt uharfbuzz Pillow
```

Set these values once, replacing the example font path and family name. Use a
short, filesystem-safe family name and keep `WORK` relative: the production font
loader used by the probe limits font paths to 127 bytes. Use a fresh work folder
for each experiment, not another family's directory.

```bash
export FONT_SOURCE="/absolute/path/to/MyThaiFont-Regular.ttf"
export FONT_FAMILY="MyThaiFont"
export FONT_SIZES="8,10,12,14,16,18"
export WORK="build/font-new/$FONT_FAMILY"
```

For TTC input, explicitly select and extract the intended face before conversion.
For a variable font, inspect its `fvar` axes, then freeze a static face with
FontTools at the intended coordinates. The Thai baker rejects variable faces.
Record the original hash, chosen face/axis values, derivative hash, and command;
point `FONT_SOURCE` at that static derivative. Do not silently choose an arbitrary
face or weight.

## 2. Inspect the source before converting

This records source identity, embedded notices, all 87 standard Thai characters,
OpenType table availability, and variable-font axes. Missing glyphs are reported
for investigation, not silently replaced with another Thai font.

```bash
python3 - <<'PY'
import hashlib
import json
import os
from pathlib import Path
from fontTools.ttLib import TTFont

source = Path(os.environ["FONT_SOURCE"])
work = Path(os.environ["WORK"])
reports = work / "reports"
reports.mkdir(parents=True, exist_ok=True)
with TTFont(source) as font:
    cmap = font.getBestCmap() or {}
    thai = list(range(0x0E01, 0x0E3B)) + list(range(0x0E3F, 0x0E5C))
    report = {
        "source": str(source.resolve()),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "names": {str(i): font["name"].getDebugName(i) for i in (0, 1, 2, 5, 13, 14)},
        "missing_thai": [f"U+{cp:04X}" for cp in thai if cp not in cmap],
        "shaping_tables": [tag for tag in ("GDEF", "GSUB", "GPOS") if tag in font],
        "variable_axes": [
            {"tag": axis.axisTag, "min": axis.minValue,
             "default": axis.defaultValue, "max": axis.maxValue}
            for axis in font["fvar"].axes
        ] if "fvar" in font else [],
    }
(reports / "source-inspection.json").write_text(
    json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
)
print(json.dumps(report, ensure_ascii=False, indent=2))
PY
```

Before continuing:

1. Resolve missing Thai coverage and any variable axes. The baker additionally
   checks all characters required by its shaping contexts; do not suppress those
   errors or weaken its validation.
2. Inspect actual vowel/tone positioning, not just the presence of GPOS/GSUB.
   Legacy fonts may use prepositioned glyphs or nonstandard mappings. The baker
   records HarfBuzz output; it cannot invent correct positioning absent from the
   source. Do not assume `--thai-shaping` repairs every legacy font.
3. If source repair is necessary, keep the original immutable, document the
   exact font-local change in a reproducible script, check modification rights
   and reserved font names, and compare before/after rendering. Do not copy
   another family's anchor offsets or silently add global spacing adjustments.
4. Obtain the actual license and preserve attribution. Metadata alone is not a
   complete license. JS-Jindara is **GPL v2**, per the project owner's clarification;
   Garuda is **GPL-2.0-or-later with a font-embedding exception**. Do not apply
   those licenses to unrelated new fonts by assumption.

## 3. Convert the paired fonts

The following baseline includes only glyphs supplied by the selected source.
Requesting an interval does not guarantee the source covers it.

```bash
python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
  --regular "$FONT_SOURCE" \
  --name "$FONT_FAMILY" \
  --sizes "$FONT_SIZES" \
  --intervals 'latin-ext,ipa-chars,greek,(0x0300-0x036F),punctuation,symbols,thai,arabic' \
  --thai-shaping \
  --output-dir "$WORK/fonts/$FONT_FAMILY"
```

Expect one `.cpfont`, `.cpshape`, and `.cpshape.json` per size. Preserve the JSON
reports as desktop provenance; they record source hashes, fallback order, tool
versions, metrics/payload CRCs, and shaping glyph mappings.

Optional additions, only when appropriate:

- Supply actual style files with `--bold`, `--italic`, and `--bolditalic` in the
  same invocation. Preserve their licenses and validate each supplied style.
  The host probe below loads the regular face; its basic specimen does not
  establish acceptance of every additional style.
- Add repeated `--fallback-regular /path/to/fallback.ttf` arguments in an explicit
  priority order, and corresponding per-style arguments if needed. The primary
  face wins. Record fallback hashes and include all required license notices.
- Use only license-compatible fallbacks. In particular, the existing Garuda
  conversion keeps its GPL font data separate from Noto/OFL fallback glyphs;
  do not merge them without resolving compatibility. The baker requires Thai
  shaping coverage in the primary face, not merely in a fallback.
- Do not enable `--force-autohint` by default. If needed, compare native and
  autohinted output at every supported size before choosing.

## 4. Build the production host probe

CMake and a C++20 compiler are required. A fresh test configuration fetches
GoogleTest and may require network access. Reuse the existing test build directory
and generator when present; do not clean it merely to render fonts.

```bash
cmake -S test -B build/test
cmake --build build/test --target ThaiRenderProbe --config Release --parallel 4
export FONT_PROBE="build/test/thai_render/ThaiRenderProbe"
```

On Windows, set `FONT_PROBE` to `build/test/thai_render/ThaiRenderProbe.exe`.

## 5. Exercise all sizes and orientations

Run the production parser/renderer with stacked marks, Thai/Latin transitions,
SARA AM, punctuation, numerals, and a longer wrapping paragraph. The viewport
sizes below are **synthetic host fixtures**, not claims about any device's usable
reading area. Repeat with the real device viewport when it is known.

```bash
python3 - <<'PY'
import json
import os
from pathlib import Path
import subprocess

work = Path(os.environ["WORK"])
family = os.environ["FONT_FAMILY"]
sizes = [int(value) for value in os.environ["FONT_SIZES"].split(",")]
specimen = work / "reports" / "specimen.xhtml"
specimen.write_text('''<html xmlns="http://www.w3.org/1999/xhtml"><body>
<p>ทั้ง ตั้ง ครั้ง ชั้น นั้น น้ำ กำ จำ สำ ค่ำ ปี่ ปู่ ญู ฐุ ฎุ ฏุ</p>
<p>ภาษาไทย English 123 ๑๒๓ ทดสอบABCภาษาไทย ๆ ฯ</p>
<p>เมื่อเราอ่านหนังสือภาษาไทยควรเห็นสระและวรรณยุกต์ครบถ้วน
ตัวอักษรต้องไม่ชนกันและบรรทัดควรตัดคำอย่างเหมาะสม
ทดสอบการอ่านต่อเนื่องหลายบรรทัดทั้งข้อความภาษาไทยและภาษาอังกฤษ</p>
</body></html>''', encoding="utf-8")
for size in sizes:
    font = work / "fonts" / family / f"{family}_{size}.cpfont"
    if not font.is_file() or not font.with_suffix(".cpshape").is_file():
        raise SystemExit(f"Missing font/shaping pair: {font}")
    for orientation in ("portrait", "inverted", "cw", "ccw"):
        width, height = (480, 800) if orientation in ("portrait", "inverted") else (800, 480)
        output = work / "smoke" / f"{size}-{orientation}"
        subprocess.run([
            os.environ["FONT_PROBE"], "--font", str(font), "--xhtml", str(specimen),
            "--width", str(width), "--height", str(height), "--orientation", orientation,
            "--character-spacing", "0", "--word-spacing-percent", "100",
            "--line-compression", "1.0", "--hyphenation", "off", "--focus", "off",
            "--output", str(output),
        ], check=True)
        report = json.loads((output / "report.json").read_text())
        if not report["settings"]["thai_shape_active"]:
            raise SystemExit(f"Companion was not accepted: {output}")
        if report["measurements"]["thai_analysis_unavailable"]:
            raise SystemExit(f"Thai analysis fell back: {output}")
print(f"Passed {len(sizes) * 4} host rendering scenarios; visual review still required")
PY
```

### Visual acceptance is mandatory

Open the generated `.pbm` (black/white) and `.pgm` (grayscale) pages. The probe
exports physical panel coordinates: portrait images can look sideways and CW
images upside down until rotated for reading. This alone is not a font defect.
Tools that need PNG can convert the grayscale pages with Pillow:

```bash
python3 - <<'PY'
import os
from pathlib import Path
from PIL import Image
work = Path(os.environ["WORK"])
for source in sorted((work / "smoke").rglob("*.pgm")):
    with Image.open(source) as image:
        image.save(source.with_suffix(".png"))
PY
```

Inspect the smallest UI sizes and all reader sizes, plus all four orientations.
Check missing boxes, lost vowels/tones, overlapping marks, clipped ascenders or
descenders, word gaps, and line spacing. Distinguish a font problem from a layout
problem. An active companion and a zero exit status do not prove visual quality.

If extended fallbacks were included, add coverage-specific specimens such as
`/ˈθɪŋk/`, `← → ⇒`, `∞ ≠ ✓ ★`, and `مرحبا بالعالم`; do not claim support for
unrequested or uncovered scripts. For non-regular faces, add styled XHTML
specimens and inspect those faces too. For repaired fonts, retain before/after
images of the exact affected clusters and confirm unaffected glyphs remain intact.

## 6. Preserve notices and package the family

Before packaging, put the actual license texts and source notices under
`$WORK/fonts/$FONT_FAMILY/`. Keep reproduction instructions and any required
source distribution under `$WORK/sources/`. Do not invent a license or declare
public distribution cleared merely because this step produces a ZIP.

This command moves converter reports out of the SD installation directory,
packages the family with its reports/source materials, and writes a checksum.
It does not include temporary smoke images or touch any other family.

```bash
python3 - <<'PY'
import hashlib
import os
from pathlib import Path
import zipfile

work = Path(os.environ["WORK"])
family = os.environ["FONT_FAMILY"]
reports = work / "reports"
for report in (work / "fonts" / family).glob("*.cpshape.json"):
    report.replace(reports / report.name)
archive_path = work.parent / f"{family}-Thai-Font.zip"
with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
    for directory in ("fonts", "reports", "sources"):
        for path in sorted((work / directory).rglob("*")):
            if path.is_file() and path.name != ".DS_Store" and "__MACOSX" not in path.parts:
                archive.write(path, path.relative_to(work).as_posix())
with zipfile.ZipFile(archive_path) as archive:
    if archive.testzip() is not None:
        raise SystemExit("Font ZIP integrity check failed")
with archive_path.open("rb") as stream:
    digest = hashlib.file_digest(stream, "sha256").hexdigest()
checksum = archive_path.with_suffix(".zip.sha256")
checksum.write_text(f"{digest}  {archive_path.name}\n", encoding="ascii")
print(archive_path)
print(checksum.read_text(), end="")
PY
```

The packaging command requires Python 3.11 or newer. Verify the generated manifest
from the ZIP's directory with `shasum -a 256 -c FAMILY-Thai-Font.zip.sha256`, or
`sha256sum -c FAMILY-Thai-Font.zip.sha256` on systems without `shasum`, substituting
the actual family name.

## 7. Install and hand off

1. Extract only `fonts/` to the SD-card root, merging without deleting other fonts
   or books. If this family already exists in `/.fonts/`, update that copy instead:
   the hidden root takes priority over `/fonts/` for duplicate families.
2. Keep every `.cpfont` and matching `.cpshape` together. Restart the reader,
   select the family in **Settings → Reader → Font Family**, and test its sizes.
3. Do not delete progress or book caches simply to install a font. Verify stacked
   marks, page turns, and all orientations on hardware. If monitoring memory,
   confirm internal free heap remains above 50 KB without a downward trend across
   repeated book/font switches. Mark hardware checks unverified if no device is available.
4. Report the source and derivative hashes, styles/sizes, fallback coverage,
   license status, exact ZIP/checksum paths, exercised host scenarios, inspected
   previews, and remaining device checks. Do not label a font visually verified
   until its actual output has been opened and inspected.

### If also asked to update the complete release font pack

Use the current complete `THCraft-Thai-Fonts.zip` as the baseline. Add the new
family under `fonts/`, with its notices and required source materials. Preserve
existing members' uncompressed bytes; compare per-member SHA-256 hashes before
and after. Reject duplicate ZIP paths. Do not regenerate the seven existing
families merely to add one more, and do not replace the complete archive with
this workflow's one-family ZIP.

`RELEASE.MD` currently requires exactly seven named families. When a new family
is approved for inclusion, update its family list, size contract, and validation
set alongside the complete ZIP. Pass that complete archive as `FONT_ZIP` when
running the release workflow; otherwise the added family will be rejected or
left out. Regenerate release checksums only after the final archive is assembled.
Keep the old source archive until the new pack has passed verification. Do not
push, publish, or flash anything without explicit authorization.
