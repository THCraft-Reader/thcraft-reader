# Compiling THCraft Reader

Run commands from the repository root. These instructions build firmware only;
they do **not** upload or flash a device.

For Xteink X4 Pro, the release build command is:

```sh
pio run -e x4pro-gh_release
```

Complete the setup below first. **Do not use bare `pio run` for X4 Pro:** this
repository's default environment targets ESP32-C3, not the Pro.

## 1. Install prerequisites

- **Git**, including support for submodules.
- **Python 3.12**. Pro font generation requires its Unicode 15.0.0 database;
  a different Python minor version is not interchangeable.
- A **host C/C++ compiler** for the Pro's Thai dictionary asset builder:
  - **Windows:** Visual Studio Build Tools 2022 or newer, with the
    **Desktop development with C++** workload and a Windows SDK. This is
    separate from the ESP32 cross-compiler. Visual Studio Code alone is not
    sufficient.
  - **Linux:** GCC/G++ and Make, or an equivalent C/C++ toolchain. On Ubuntu
    24.04, install `git`, `build-essential`, `python3.12`, and `python3.12-venv`.
  - **macOS:** Xcode Command Line Tools (`xcode-select --install`) and Python 3.12.
- Internet access for the first PlatformIO platform/toolchain/library downloads.

Use a short checkout path on a drive with ample free space, for example
`D:\src\thcraft-reader`. SDK downloads, unpacked toolchains, build outputs and
relink packages occupy many gigabytes; **30 GB or more free space is recommended**,
with additional space for multiple targets and extracted relink kits.

PlatformIO downloads the ESP32 cross-compilers. Do not install or configure a
separate ESP-IDF checkout for these commands. Host CMake is installed below.

## 2. Clone, or update an existing checkout

For a new machine:

```sh
git clone --branch develop --recurse-submodules https://github.com/THCraft-Reader/thcraft-reader.git
cd thcraft-reader
```

For an existing checkout, commit or stash local work as appropriate, then:

```sh
git pull --ff-only
git submodule sync --recursive
git submodule update --init --recursive
```

If the pull reports a divergent branch, resolve it rather than forcing a reset.
Use your fork's clone URL if applicable. `freeink-sdk` must be present at the
commit recorded by this repository: **do not add `--remote` to the submodule
update command**.

## 3. Set up Python and build storage

The tool versions below match the Windows build verified for the native-text
integration. Linux/macOS commands use the same dependencies, but were not part
of that Windows verification.

### Windows — PowerShell

Create the virtual environment once:

```powershell
py -3.12 -m venv build/native-tools
$Root = (Get-Location).Path
$Tools = Join-Path $Root 'build/native-tools'
$Python = Join-Path $Tools 'Scripts/python.exe'

& $Python -m pip install 'platformio==6.2.0' 'SCons==4.11.1' 'PyYAML==6.0.3' -r lib/EpdFont/scripts/requirements-native-text.txt
```

Then configure the current terminal. **Run this block again in every new
PowerShell session**, from the repository root:

```powershell
$Root = (Get-Location).Path
$Tools = Join-Path $Root 'build/native-tools'
$Python = Join-Path $Tools 'Scripts/python.exe'
$env:PATH = (Join-Path $Tools 'Scripts') + ';' + $env:PATH

$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:PLATFORMIO_CORE_DIR = Join-Path $Root 'build/pio'
$env:IDF_TOOLS_PATH = Join-Path $Root 'build/pio/idf-tools'
$env:TEMP = Join-Path $Root 'build/tmp'
$env:TMP = $env:TEMP
$env:SCONS_LIB_DIR = (& $Python -c "import sysconfig; print(sysconfig.get_path('purelib'))").Trim()

New-Item -ItemType Directory -Force -Path $env:PLATFORMIO_CORE_DIR, $env:TEMP | Out-Null
```

This does not require virtual-environment activation or changing PowerShell's
execution policy. It keeps SDK and temporary storage on the checkout's drive
instead of filling the system drive.

`PYTHONUTF8`/`PYTHONIOENCODING` prevent console encoding errors when the i18n
builder prints language names. `SCONS_LIB_DIR` selects the private SCons
installation consistently even when SDK setup replaces its bundled SCons
package; this avoids the observed `SCons.Tool.FortranCommon` import failure.

### Linux/macOS — Bash or Zsh

Create the virtual environment once:

```sh
python3.12 -m venv build/native-tools
. build/native-tools/bin/activate
python -m pip install 'platformio==6.2.0' 'SCons==4.11.1' 'PyYAML==6.0.3' -r lib/EpdFont/scripts/requirements-native-text.txt
```

Run this block in each new terminal, from the repository root:

```sh
. build/native-tools/bin/activate
export PYTHONUTF8=1
export PYTHONIOENCODING=utf-8
export PLATFORMIO_CORE_DIR="$PWD/build/pio"
export IDF_TOOLS_PATH="$PWD/build/pio/idf-tools"
export TMPDIR="$PWD/build/tmp"
export SCONS_LIB_DIR="$(python -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])')"
mkdir -p "$PLATFORMIO_CORE_DIR" "$TMPDIR"
```

The native requirements file pins FontTools and CMake. PyYAML is needed by the
compiled font catalogue generator. The repository's broader `requirements.txt`
also covers font-conversion, artwork and debugging utilities; it is not required
for this firmware-only setup. If installing those extra tools, keep the native
FontTools pin by including `-r lib/EpdFont/scripts/requirements-native-text.txt`
in the same pip invocation.

## 4. Compile the correct target

After terminal setup, these commands work in either shell:

```sh
# X4 Pro release firmware
pio run -e x4pro-gh_release

# X4 Pro development firmware
pio run -e x4pro
```

Other supported build targets relevant to this integration:

| Target | Hardware / purpose | Command |
|---|---|---|
| `default` | ESP32-C3 Xteink X3/X4 development | `pio run -e default` |
| `x4c` | Xteink X4 Classic development | `pio run -e x4c` |
| `x4pro` | Xteink X4 Pro development | `pio run -e x4pro` |
| `x4pro-gh_release` | Xteink X4 Pro release | `pio run -e x4pro-gh_release` |

To check the full four-target matrix:

```sh
pio run -e x4pro -e x4pro-gh_release -e default -e x4c
```

The first build installs SDK packages and may compile SDK components. Subsequent
builds are incremental. **Do not clean before every build.** A build is successful
only when PlatformIO finishes with `SUCCESS` for the requested environment.

The Pro build automatically generates its native font arrays, complete Thai
dictionary and compiled font catalogue. HTML and i18n headers are also generated
automatically. Do not edit generated files or regenerate legacy bitmap fonts to
build the Pro. Normal native asset generation uses checked-in pinned sources;
it does not refresh the online font catalogue.

### Output files

For the Pro release:

```text
.pio/build/x4pro-gh_release/firmware.bin
.pio/build/x4pro-gh_release/firmware.elf
.pio/build/x4pro-gh_release/firmware.map
```

Other environments use `.pio/build/<environment>/`.

The Pro OTA slot is **`0x640000` / 6,553,600 bytes**. Check both PlatformIO's
size result and the actual `firmware.bin` length. The native-text integration's
verified development image had only 3,664 bytes of spare space; release had
39,488 bytes. Those are historical measurements, not a guarantee for later
commits. Do not bypass the size check, disable required font/dictionary data, or
change partitions simply to make an oversized build succeed.

## 5. Routine workflow after `git pull`

On a machine already set up:

1. Open a terminal in the repository root and run the appropriate terminal
   setup block from section 3.
2. Update the checkout and its pinned SDK submodule:

   ```sh
   git pull --ff-only
   git submodule sync --recursive
   git submodule update --init --recursive
   ```

3. Install the build dependencies again if requirement files changed; repeating
   this command is safe:

   ```sh
   python -m pip install 'platformio==6.2.0' 'SCons==4.11.1' 'PyYAML==6.0.3' -r lib/EpdFont/scripts/requirements-native-text.txt
   ```

4. Build the intended board, for example:

   ```sh
   pio run -e x4pro-gh_release
   ```

Do not copy an old virtual environment or CMake cache to a new machine. Create
the environment again. Keep any existing `platformio.local.ini` under review:
its local overrides can change the selected profile's flags or ports. It is
optional and should not be committed.

## 6. Troubleshooting

- **Missing `freeink-sdk` headers or libraries:** run the submodule sync/update
  commands above. An ordinary source ZIP may omit submodules; prefer Git clone.
- **CMake cannot find a host compiler:** install the C++ workload/toolchain from
  section 1. The ESP32 cross-compiler is not the host dictionary-builder compiler.
- **Wrong Python/Unicode or FontTools version:** use the configured virtual
  environment and reinstall the pinned native requirements. Check `python --version`
  and `python -m pip show fonttools cmake`.
- **`pio`/`cmake` not found:** rerun terminal setup; its virtual-environment
  executable directory must precede other tools on `PATH`.
- **`UnicodeEncodeError` while printing translations:** ensure both Python UTF-8
  environment variables are set before starting PlatformIO.
- **`No module named SCons.Tool.FortranCommon`:** install `SCons==4.11.1` in the
  same environment and set `SCONS_LIB_DIR` as shown above. Start a fresh build
  process after doing so.
- **No space left on the system drive:** stop the failed build and use the
  project-local SDK/temp paths above on a drive with enough free space. Do not
  delete unrelated SDK installations or personal files.
- **Stale host CMake paths after moving the checkout or changing compilers:**
  remove only the generated `build/native-text-assets/` directory, then rebuild.
  Do not delete SD-card reading progress or the entire SDK cache for this issue.
- **Pinned font checksum mismatch:** restore the corresponding checked-in source
  files; do not edit the expected checksum to accept an unknown download.

## 7. Distributing Pro firmware

The Pro statically links LGPL libraries. When redistributing a binary, provide
its matching source/object/relink materials as described in
[Native text licensing and relinking](docs/native-text-licensing.md).

After a successful Pro release build, create its matching kit:

```sh
python scripts/package_native_text_relink.py --manifest .pio/build/x4pro-gh_release/native-text-link.json --output build/x4pro-release-native-text-relink.zip
```

Keep the kit paired with the exact `firmware.bin` that produced the capture.
Changing or rebuilding inputs afterward can invalidate it. A license notice
alone is not a substitute for the relink materials.

Build tools and generated outputs under `build/`, `.pio/`, `.cache/`,
`*.generated.h`, and the generated i18n tables are not source files to commit.
Compilation does not prove behavior on the device; hardware testing remains a
separate step. No upload command is run by this guide.
