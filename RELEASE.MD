# X4 Pro Thai release workflow

Build the firmware locally and assemble the following directory, relative to the repository root:

```text
Release/x4pro-thai/
├── firmware.bin
├── firmware.factory.bin
├── SHA256SUMS
└── THCraft-Thai-Fonts.zip
```

Use **`Release` (singular)** for new output. The existing example is in
`Releases/x4pro-thai/` (plural); do not overwrite that input archive.
This workflow does not upload firmware, push Git changes, or publish a GitHub release.

## Prerequisites

- Run from the repository root on macOS, Linux, or Windows Git Bash.
- Have PlatformIO (`pio`) on `PATH`, Python 3.11 or newer, the project's build
  dependencies, and the `freeink-sdk` submodule installed. See the development
  setup in `README.md`. On a fresh checkout, initialize submodules with
  `git submodule update --init --recursive`.
- Review the version in `[crosspoint]` in `platformio.ini` before building.
  The production environment is **`x4pro-gh_release`**, not the default X4/C3
  environment or the more verbose `x4pro` development environment.
- Have the complete, previously prepared Thai font ZIP available. The default
  input is `Releases/x4pro-thai/THCraft-Thai-Fonts.zip`. These local font assets
  are not recreated by compiling firmware and may need to be supplied separately
  on a fresh checkout. To use a different complete ZIP, set its path first:

  ```bash
  export FONT_ZIP="/absolute/path/to/THCraft-Thai-Fonts.zip"
  ```

## Font archive contract

Keep this SD-card installation layout inside `THCraft-Thai-Fonts.zip`:

```text
fonts/
├── BaiJamjuree/
├── Garuda/
├── JS-Jindara/
├── Mali/
├── THCraft-NotoSansThai/
├── THCraft-NotoSerifThai/
└── THCraft-Sarabun/
```

Every family must include matching `.cpfont` and `.cpshape` files at **8, 10,
12, 14, 16, and 18 pt**. JS-Jindara also includes **20, 22, 24, and 26 pt**.
For example, `fonts/Mali/Mali_16.cpfont` must remain beside
`fonts/Mali/Mali_16.cpshape`. The current pack contains 46 pairs.
Keep all license notices and any accompanying installation/source files.

The workflow validates the archive and copies it **byte-for-byte**; it does not
reconvert fonts, change Thai shaping, strip larger sizes, or alter font licenses.
Do not substitute the output of `scripts/package_thai_fonts.py` alone: that
script builds only THCraft-NotoSansThai, THCraft-NotoSerifThai, and THCraft-Sarabun,
not this complete seven-family collection. After updating any fonts, supply an
updated complete ZIP as `FONT_ZIP` before running the workflow.

## Build and package

Run this entire block from the repository root. It validates the font input,
compiles the X4 Pro production firmware, stages both generated binaries and the
font ZIP, then writes fresh SHA-256 checksums for those three files. A failed
build or incomplete font archive stops packaging; no stale binaries are copied
following a failed build. Existing copies of the four release files are replaced;
unrelated files in the destination cause an error rather than being deleted.

```bash
python3 - <<'PY'
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile
import zipfile

root = Path.cwd()
if not (root / "platformio.ini").is_file():
    raise SystemExit("Run this workflow from the repository root")

environment = "x4pro-gh_release"
build = root / ".pio" / "build" / environment
output = root / "Release" / "x4pro-thai"
font_zip = Path(os.environ.get(
    "FONT_ZIP", "Releases/x4pro-thai/THCraft-Thai-Fonts.zip"
)).expanduser().resolve()
artifacts = ("firmware.bin", "firmware.factory.bin", "THCraft-Thai-Fonts.zip")
families = {
    "BaiJamjuree", "Garuda", "JS-Jindara", "Mali",
    "THCraft-NotoSansThai", "THCraft-NotoSerifThai", "THCraft-Sarabun",
}

with zipfile.ZipFile(font_zip) as archive:
    bad_member = archive.testzip()
    if bad_member is not None:
        raise SystemExit(f"Damaged font ZIP member: {bad_member}")
    members = set(archive.namelist())
    fonts = {PurePosixPath(name) for name in members
             if name.startswith("fonts/") and name.endswith(".cpfont")}
    shapes = {PurePosixPath(name) for name in members
              if name.startswith("fonts/") and name.endswith(".cpshape")}
    if {p.parts[1] for p in fonts} != families:
        raise SystemExit("Font ZIP must contain all seven specified families, and no others")
    if {p.with_suffix(".cpshape") for p in fonts} != shapes:
        raise SystemExit("Every .cpfont must have a matching .cpshape, and vice versa")
    for family in sorted(families):
        sizes = (8, 10, 12, 14, 16, 18)
        if family == "JS-Jindara":
            sizes += (20, 22, 24, 26)
        for size in sizes:
            name = f"fonts/{family}/{family}_{size}.cpfont"
            if name not in members:
                raise SystemExit(f"Missing required font: {name}")
    print(f"Validated {len(families)} families, {len(fonts)} font/shaping pairs", flush=True)

if output.exists():
    unexpected = {p.name for p in output.iterdir()} - set(artifacts) - {"SHA256SUMS"}
    if unexpected:
        raise SystemExit(f"Destination contains unrelated entries: {sorted(unexpected)}")

subprocess.run(["pio", "run", "-e", environment], check=True)
for name in artifacts[:2]:
    if not (build / name).is_file() or (build / name).stat().st_size == 0:
        raise SystemExit(f"Missing or empty build artifact: {build / name}")

output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="x4pro-thai-", dir=output.parent) as temporary:
    stage = Path(temporary)
    for name in artifacts[:2]:
        shutil.copyfile(build / name, stage / name)
    shutil.copyfile(font_zip, stage / artifacts[2])
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
older release bundles.

## Verify and hand off

From the repository root, verify the published files against the new manifest:

```bash
(cd Release/x4pro-thai && shasum -a 256 -c SHA256SUMS)
```

On Linux/Windows Git Bash without `shasum`, use:

```bash
(cd Release/x4pro-thai && sha256sum -c SHA256SUMS)
```

All three entries must report `OK`. Confirm that the output directory contains
exactly the four files shown above. Changing any binary or the font ZIP requires
regenerating `SHA256SUMS`.

Before distribution, test the firmware on an **X4 Pro** and install the ZIP's
`fonts/` directory at the SD-card root. Check that all seven families can be
selected, Thai stacked marks render correctly, and page turns work in all four
orientations. A successful build and checksum verification do not replace
hardware testing.

### Public-distribution licensing gate

**JS-Jindara is GPL v2**, as clarified by the project owner. The existing ZIP's
`SOURCE-NOTICE.txt` reproduces embedded copyright metadata but does not state
that license. Before public distribution, update that notice to identify GPL v2,
retain the original copyright attribution, and include the GPL v2 license text
and corresponding source materials required by the license.

Garuda is GPL-2.0-or-later with a font-embedding exception. The current example ZIP
contains Garuda's converted font pairs but omits its notices/source package.
Before public distribution, retain the applicable notices and provide the
corresponding source as required by that license. The local
`build/thai/garuda-pack/` holds its `COPYING`, `GPL`, source archive, and
conversion information; inspect and include the required material in the complete
font archive before packaging a public release. Preserve the other families'
OFL and source notices as well.
