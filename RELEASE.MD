# Thai release workflow

Build the firmware locally and assemble the following directory, relative to the repository root:

```text
Release/x4pro-thai/
├── firmware.bin
├── firmware.factory.bin
├── SHA256SUMS
└── THCraft-Thai-Fonts.zip
```

Select the production target with `RELEASE_TARGET` before running the build block:

| Device | `RELEASE_TARGET` / output under `Release/` | PlatformIO environment |
| --- | --- | --- |
| X4 Pro (default) | `x4pro-thai` | `x4pro-gh_release` |
| X4 Classic | `x4classic-thai` | `x4c-gh_release` |
| X4 | `x4-thai` | `gh_release` |
| X3 | `x3-thai` | `gh_release` |

For example, `export RELEASE_TARGET=x4classic-thai` selects X4 Classic.
Each output directory contains the same four filenames shown above. X4 and X3
use the same firmware; their package binaries must match byte-for-byte.

Use **`Release` (singular)** for new output. Leave historical packages under
`Releases/` (plural) unchanged; neither directory is a source of fonts.
This workflow does not upload firmware, push Git changes, or publish a GitHub release.

## Prerequisites

- Run from the repository root on macOS, Linux, or Windows Git Bash.
- Have PlatformIO (`pio`) on `PATH`, Python 3.11 or newer, the project's build
  dependencies, and the `freeink-sdk` submodule installed. See the development
  setup in `README.md`. On a fresh checkout, initialize submodules with
  `git submodule update --init --recursive`.
- Review the version in `[crosspoint]` in `platformio.ini` before building.
  Select the production environment from the table above, not a development
  environment. Production builds use info-level logging.
- Use the repository's **`fontpacks/` library** as the font source. Do not reuse
  a font ZIP from an older release or substitute the three-family packager output.
  Update the library first when changing the release font selection.

## Font archive contract

`fontpacks/` is the authoritative library. Its current eight families are:

```text
fontpacks/
├── BaiJamjuree/
├── ChakraPetch/
├── Garuda/
├── JS-Jindara/
├── Mali/
├── THCraft-NotoSansThai/
├── THCraft-NotoSerifThai/
└── THCraft-Sarabun/
```

Each family contains `.cpfont` files at **8, 10, 12, 14, 16, 18, 20, 22, 24,
and 26 pt**, named `<family>_<size>.cpfont`. The current library has **80 fonts**.
The workflow discovers families from the library, validates their sizes and
CPFont v4 headers, and creates `THCraft-Thai-Fonts.zip` with this installation
layout:

```text
fonts/<family>/<family>_<size>.cpfont
```

Only `.cpfont` files are included. Do not add notice files or warning text to the
font ZIP or firmware. The workflow preserves every font file's bytes and does
not reconvert fonts, select fallbacks, or copy assets from previous releases.
It stages a fresh ZIP before building and replacing output files.

Firmware derives Thai placement from native glyph metrics. No `.cpshape` files
are needed. Glyph coverage is whatever the library's converted files contain;
do not infer coverage from a family name or an older release's reports.

## Build and package

Run this entire block from the repository root. It validates `fontpacks/`,
creates a fresh font ZIP, compiles the selected production firmware, and writes
fresh SHA-256 checksums. A failed build or invalid font library stops packaging;
no stale binaries are copied after a failed build. Existing copies of the four
release files are replaced; unrelated destination files cause an error rather
than being deleted.

```bash
python3 - <<'PY'
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

root = Path.cwd()
if not (root / "platformio.ini").is_file():
    raise SystemExit("Run this workflow from the repository root")

target = os.environ.get("RELEASE_TARGET", "x4pro-thai")
environments = {
    "x4pro-thai": "x4pro-gh_release",
    "x4classic-thai": "x4c-gh_release",
    "x4-thai": "gh_release",
    "x3-thai": "gh_release",
}
if target not in environments:
    raise SystemExit(f"Unknown RELEASE_TARGET: {target}")
environment = environments[target]
build = root / ".pio" / "build" / environment
output = root / "Release" / target
font_root = root / "fontpacks"
artifacts = ("firmware.bin", "firmware.factory.bin", "THCraft-Thai-Fonts.zip")
fonts = sorted(font_root.rglob("*.cpfont"))
if not fonts or any(len(path.relative_to(font_root).parts) != 2 for path in fonts):
    raise SystemExit("Expected fontpacks/<family>/<family>_<size>.cpfont")
families = sorted({path.parent.name for path in fonts})
sizes = (8, 10, 12, 14, 16, 18, 20, 22, 24, 26)
expected = {
    font_root / family / f"{family}_{size}.cpfont"
    for family in families for size in sizes
}
if set(fonts) != expected:
    raise SystemExit("Every fontpacks family must contain exactly the ten documented sizes")
for path in fonts:
    with path.open("rb") as font:
        header = font.read(32)
    if len(header) != 32 or header[:10] != b"CPFONT\0\0\x04\x00":
        raise SystemExit(f"Not a CPFont v4 file: {path}")

if output.exists():
    unexpected = {p.name for p in output.iterdir()} - set(artifacts) - {"SHA256SUMS"}
    if unexpected:
        raise SystemExit(f"Destination contains unrelated entries: {sorted(unexpected)}")

output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix=f"{target}-", dir=output.parent) as temporary:
    stage = Path(temporary)
    font_zip = stage / artifacts[2]
    with zipfile.ZipFile(
        font_zip, "w", compression=zipfile.ZIP_DEFLATED,
        compresslevel=9, strict_timestamps=False,
    ) as archive:
        for path in fonts:
            archive.write(path, "fonts/" + path.relative_to(font_root).as_posix())
    with zipfile.ZipFile(font_zip) as archive:
        bad_member = archive.testzip()
        if bad_member is not None:
            raise SystemExit(f"Damaged font ZIP member: {bad_member}")
    print(f"Staged {len(families)} families, {len(fonts)} fonts from fontpacks", flush=True)

    build_env = os.environ.copy()
    build_env.update(PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
    subprocess.run(["pio", "run", "-e", environment], check=True, env=build_env)
    for name in artifacts[:2]:
        if not (build / name).is_file() or (build / name).stat().st_size == 0:
            raise SystemExit(f"Missing or empty build artifact: {build / name}")
        shutil.copyfile(build / name, stage / name)
    checksums = []
    for name in artifacts:
        with (stage / name).open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        checksums.append(f"{digest}  {name}\n")
    (stage / "SHA256SUMS").write_text("".join(checksums), encoding="ascii", newline="\n")
    output.mkdir(exist_ok=True)
    for name in (*artifacts, "SHA256SUMS"):
        (stage / name).replace(output / name)

print(f"Release ready: {output}")
print((output / "SHA256SUMS").read_text(), end="")
PY
```

`firmware.factory.bin` is generated by the build; do not rename `firmware.bin`
to create it. Copy both from the **same successful build**. `SHA256SUMS` contains
three lines, using the bare artifact filenames, and does not include itself or
older release bundles. When packaging X3 and X4 together, build `gh_release`
once and use those same two binaries for both destinations.

## Verify and hand off

From the repository root, verify the published files against the new manifest:

```bash
(cd "Release/${RELEASE_TARGET:-x4pro-thai}" && shasum -a 256 -c SHA256SUMS)
```

On Linux/Windows Git Bash without `shasum`, use:

```bash
(cd "Release/${RELEASE_TARGET:-x4pro-thai}" && sha256sum -c SHA256SUMS)
```

All three entries must report `OK`. Confirm that the output directory contains
exactly the four files shown above. Changing any binary or the font ZIP requires
regenerating `SHA256SUMS`.

Before distribution, test the firmware on **each target device** and install the ZIP's
`fonts/` directory at the SD-card root. Check that all library families can be
selected, Thai stacked marks render correctly, and page turns work in all four
orientations. A successful build and checksum verification do not replace
hardware testing.

### Public-distribution licensing gate

The current `fontpacks/` library contains converted font files without license
or source materials. Packaging it does not establish redistribution rights.
No notice or warning files are embedded by this workflow.

Before public distribution, arrange the required notices and corresponding
source materials separately. **JS-Jindara is GPL v2**, as clarified by the
project owner. Garuda is GPL-2.0-or-later with a font-embedding exception.
Preserve the applicable copyright attributions, license texts and source
obligations. Preserve the OFL requirements for the other families and any Noto
fallback glyphs as well; those do not relicense Garuda or JS-Jindara.

Hardware testing and licensing clearance remain separate from local packaging.
