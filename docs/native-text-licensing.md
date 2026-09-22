# X4 Pro native text: licensing and relinking

This document accompanies the **X4 Pro** native-text firmware and its matching
`native-text-relink.zip`. Other device profiles do not build or package this
backend. A notice, a source URL, or a copy of `firmware.bin` alone is **not** the
LGPL relinking delivery described here.

## Notices and applicable licenses

- **LibThai 0.1.30 and libdatrie 0.2.14:** LGPL-2.1-or-later. Their complete
  released source trees, license texts, Thai dictionary word sources/alphabet/
  recipe, modified sources, and port patches are supplied. Read each upstream
  `COPYING`, `AUTHORS`, and individual source notice; the project's MIT license
  does not replace those licenses.
- **FreeType 2.14.3:** used under the FreeType Project License (FTL), not its
  alternative GPL license. This software is based in part on the work of the
  FreeType Team. Portions of this software are copyright © 1996–2026 The
  FreeType Project (https://freetype.org). All rights reserved. The original
  `docs/FTL.TXT` and other upstream notices are retained. The port disables
  pathname-based stdio streams while retaining caller-supplied memory/stream
  access; `port/freetype-2.14.3-native.patch` records the upstream source change.
  The separate `port/freetype/` configuration selects supported modules/features.
- **HarfBuzz 14.5.0:** its MIT-style license and per-file notices are retained in
  `vendor/harfbuzz-14.5.0/COPYING` and the original source tree. The allocator and
  feature configuration live separately under `port/harfbuzz/`.
- **Noto Sans, Noto Serif, Noto Sans Hebrew, Noto Sans Arabic, Noto Sans Thai,
  Noto Serif Thai:** SIL Open Font License 1.1, with the original copyright and
  license text for each family. Native subsets retain layout tables and use
  the family names specified in `native-text-assets.json` (THCraft-prefixed
  names), rather than presenting a modified subset as the original font.
- **Ubuntu and its Vietnamese subsets:** Ubuntu Font Licence 1.0. Original
  copyright/license text is retained. Modified native subsets use the
  `Ubuntu derivative THCraft ...` names in that same manifest.
- Reader-owned code remains under the repository's `LICENSE` (MIT). Other
  firmware libraries retain their own licenses. The kit also carries available
  `LICENSE`/`COPYING`/`NOTICE`/`AUTHORS` files from the firmware dependencies; do
  not treat this list as a relicensing of those components or the toolchain.

The LGPL permits modifying the Library and relinking the application with the
modified Library, subject to its terms. Do not impose terms prohibiting the
recipient's modification of the work for their own use or reverse engineering
for debugging those modifications. Preserve the notices, license texts and
source modifications when redistributing. In particular, distributing a
statically linked Pro image requires making the corresponding source and
usable application object/relink materials available with that image through
a license-permitted delivery method. This project stages a matching kit as a
release asset, rather than relying on an unimplemented written offer.

These are engineering delivery measures, not a legal opinion or a claim that
notices alone establish compliance. A distributor remains responsible for
reviewing the applicable licenses, its own modifications, access to matching
materials, and any additional distribution terms.

## What the kit contains

`manifest.json` records the Pro environment, source revision, original ELF/bin
SHA-256 hashes, public PlatformIO platform pin, installed package versions,
compiler target/version, actual argument order, and SHA-256/size of **every**
packaged input. `inputs/` contains the actual linked firmware objects/static
archives (including the non-LGPL application objects), linker scripts and
explicit GCC specs/data inputs. Nothing is reconstructed from a guessed list
of libraries. The native archive contains both the native integration and the
LGPL vendor members; the relinker can replace members without discarding the
remaining firmware objects.

The post-ELF SCons hook expands the original program executor's `LINKCOM` with
its actual sources, `LIBS`, and `LIBPATH`. On Windows it bypasses only SCons'
`TEMPFILE` response wrapper during capture; it does not keep an expired temp
filename. Response-file inputs are expanded before capture. Explicit `-l`
inputs are resolved using the recorded library paths and matching compiler.
Library ordering, repeated archives, and group/whole-archive options remain in
place. Linker-script includes are retained; unsupported file-bearing script
constructs or unrecognized absolute-path arguments fail rather than silently
producing an incomplete bundle. Compiler-internal runtime/startup files remain
provided by the exact matching public toolchain package.

`link.rsp` is a relocatable baseline response file for use from the extracted
kit directory. `native_text_relink.py` verifies the kit and toolchain, constructs
a fresh quoted response file with relocated paths, and executes the compiler
driver without a shell. Replacement archives/objects are explicit opt-ins;
all original inputs are checked first, including the files being replaced.

`sources/` contains the recognized native source/build-tool closure, font
sources and licenses, translation/range inputs required for font generation,
and their manifests. `upstream/` contains the original vendor archives verified
against `vendor-sources.json`. `generated/` retains the embedded dictionary and
font array sources, the full serialized dictionary, sorted word list, and asset
metadata. Port patches and a per-file upstream/modification digest inventory
are included. Unrelated checkout files, local PlatformIO configuration, secrets,
Git metadata, caches and arbitrary build directories are not collected.

This is a **relink kit**, not a replacement for the whole firmware source
checkout. The full application remains available in its source repository;
retained objects permit relinking without recompiling that application. The
source revision is informational: exact packaged bytes are identified by the
per-file manifest, including local native source modifications.

## Produce a kit locally

Use Python **3.12** for native asset generation (Unicode database 15.0.0), a host
C/C++ compiler, and the pinned asset requirements. In the firmware checkout:

```sh
python -m pip install -r lib/EpdFont/scripts/requirements-native-text.txt
pio run -e x4pro -t native-text-relink
```

This builds the normal firmware and writes
`.pio/build/x4pro/native-text-relink.zip`. The same target exists for
`x4pro-gh_release` and `x4pro-gh_release_rc`, and **only** those three profiles.
The RC environment requires its existing `CROSSPOINT_RC_HASH` setting.

After a successful normal Pro build, packaging can also be invoked explicitly:

```sh
python scripts/package_native_text_relink.py --manifest .pio/build/x4pro/native-text-link.json --output build/x4pro-native-text-relink.zip
```

The packager requires the matching ELF, bin, all captured input files, source
recipes, source/license files, and generated assets. Missing/changed inputs
fail; an existing destination ZIP is replaced only after complete success.
Pinned upstream archives are read from `build/native-downloads`, or fetched
from their recorded HTTPS URLs and SHA-256-verified if absent. An alternate
cache can be supplied with `--archive-cache`. No build output or archive is
published by these commands.

Release and release-candidate workflows install the pinned native asset tools
under Python 3.12 for their Pro job, then create and stage this ZIP alongside
the matching Pro firmware. The existing release workflow attaches both when
that workflow is authorized/triggered; the RC workflow stages artifacts only.
Legacy device jobs and the separate legacy font-release workflow are unchanged.

## Obtain the matching public toolchain

Read `manifest.json`, not an assumed compiler version. The currently pinned
platform is pioarduino `55.03.311`; its Xtensa package is
`toolchain-xtensa-esp-elf` **14.2.0+20260121**, GCC **14.2.0**. The manifest records
the package actually used. `compiler.driver` and `compiler.machine` identify
the target selected by the original build. Matching package name/version and
compiler target/version are enforced before relinking; host executable hashes
are intentionally not compared across Windows/Linux/macOS packages.

The public package for this pin can be installed with PlatformIO Core:

```sh
pio pkg install --global --tool https://github.com/pioarduino/registry/releases/download/0.0.1/xtensa-esp-elf-14.2.0_20260121.zip
```

Alternatively, install the packages for the same Pro environment in the matching
firmware checkout (`pio pkg install -e x4pro`). Pass the resulting package
**directory**, not its `bin` directory, to the commands below, normally
`~/.platformio/packages/toolchain-xtensa-esp-elf` or
`%USERPROFILE%\.platformio\packages\toolchain-xtensa-esp-elf` on Windows.
If a later release has another pin, use its manifest's `toolchain.source` and
version instead. Release workflows pin the pioarduino PlatformIO Core archive
to `v6.1.19`; native generation dependencies are pinned in the included
`requirements-native-text.txt` (FontTools 4.65.0 and CMake 4.4.3).

Vendor archive URLs, versions and SHA-256 pins are in
`sources/lib/NativeText/vendor-sources.json`; font/archive/file pins and license
hashes are in `sources/lib/EpdFont/scripts/native-text-assets.json`. Do not replace
those recorded originals when distributing a modified Library; identify your
modification separately.

## Relink unchanged or replace rebuilt inputs

Extract the ZIP anywhere, including a directory containing spaces. From the
extracted `native-text-relink` directory (substitute your package location):

```sh
python native_text_relink.py --bundle . --toolchain /path/to/toolchain-xtensa-esp-elf --output ../relinked-firmware.elf
```

The script validates all original hashes, preserves link order and emits the
new ELF plus its map. The source checkout, original absolute build paths and
original PlatformIO package paths are not needed. For example, replace the
entire native library, or one rebuilt vendor member, respectively:

```sh
python native_text_relink.py --bundle . --toolchain /path/to/toolchain-xtensa-esp-elf --output ../modified.elf --replace libNativeText.a=/path/to/rebuilt/libNativeText.a
python native_text_relink.py --bundle . --toolchain /path/to/toolchain-xtensa-esp-elf --output ../modified.elf --replace-member libNativeText.a:trie.c.o=/path/to/rebuilt/trie.c.o
```

Selectors accept exact `inputs/...` manifest paths or unique basenames. An
ambiguous/missing selector or archive member fails. Keep the original archive
member name (including `.c.o`). Multiple `--replace`/`--replace-member` options
are supported. Other members are preserved in a temporary archive, never
modified in place. Rebuild objects with the matching compiler and compatible
ABI; changing a public ABI may also require rebuilding dependent application
objects from the firmware source.

### Rebuild LibThai/libdatrie directly from the supplied sources

Keep the original extracted kit intact so its hashes can still be verified.
Copy `sources` to a separate directory, for example `../modified-sources`, and
edit the LibThai/libdatrie sources there. Then:

```sh
python native_text_relink.py --bundle . --toolchain /path/to/toolchain-xtensa-esp-elf --output ../modified.elf --rebuild-lgpl ../modified-sources
```

This compiles every C source in the shipped Thai/datrie slices with the actual
vendor objects' captured CPU/ABI/optimization flags, applies the same private
allocator header and no-default-dictionary definition, replaces those members
of the original native archive, and relinks the retained firmware objects.
It intentionally uses only the vendor/port include closure, not unrelated
firmware headers or private settings. New compilation units or changed public
interfaces require rebuilding a full replacement library with the firmware
build; this convenience path replaces the existing supported slice.

The dictionary source and its generator are also available. To regenerate the
complete dictionary/font assets in the copied source tree with a host compiler:

```sh
python -m pip install -r ../modified-sources/lib/EpdFont/scripts/requirements-native-text.txt
cmake -S ../modified-sources/lib/NativeText -B ../asset-build
cmake --build ../asset-build --config Release --target NativeTextAssets
```

Use Python 3.12 for that CMake configure (set `-DPython3_EXECUTABLE=...` if needed).
The generated C++ data can be compiled with the same target compiler and
replaced using `--replace` and the corresponding generated `.cpp.o` input name
in the manifest. A full firmware rebuild performs this automatically. Merely
editing dictionary/font files in the kit does not change retained data objects.
The complete upstream dictionary recipe/data, not a selected word sample, is
included.

Relinking produces an ELF, **not a flash operation**. To obtain an application
image, use the same platform's esptool ELF-to-image command/board flash settings
as the original firmware build (or its normal PlatformIO image step). Bootloader,
partitions, OTA slot limits and hardware compatibility remain unchanged. These
scripts never publish a release, upload firmware, change remotes or flash a
reader. Verify the rebuilt image and retain matching relink materials before
distributing it.
