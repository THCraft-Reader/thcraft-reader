"""Build the offline Rust/FreeType/HarfBuzz engine and package the single HTML."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from scripts.setup import EMSCRIPTEN_VERSION, ROOT, TARGET, run, setup


def collect_licenses(paths: dict[str, Path], environment: dict[str, str]) -> None:
    sections = [
        "Fonts Generator third-party notices\n\n"
        "Portions of this software are copyright © The FreeType Project "
        "(www.freetype.org). All rights reserved.\n\n"
        "FreeType is used under the FreeType License (FTL), not the alternate GPL.\n"
        "The following notices are retained from the pinned upstream sources.\n"
    ]
    notices: list[tuple[str, Path]] = [
        ("Fonts Generator — CrossPoint MIT License", ROOT / "LICENSE"),
        ("FreeType 2.13.3", paths["freetype"] / "LICENSE.TXT"),
        ("FreeType License", paths["freetype"] / "docs/FTL.TXT"),
        ("FreeType BDF driver", paths["freetype"] / "src/bdf/README"),
        ("FreeType PCF driver", paths["freetype"] / "src/pcf/README"),
        ("FreeType hash implementation", paths["freetype"] / "src/base/fthash.c"),
        ("FreeType bundled zlib", paths["freetype"] / "src/gzip/zlib.h"),
        ("FreeType auto-hinter integration", paths["freetype"] / "src/autofit/ft-hb.c"),
        ("HarfBuzz 10.4.0", paths["harfbuzz"] / "COPYING"),
        ("Emscripten 4.0.15", paths["emscripten"] / "LICENSE"),
        ("musl libc", paths["emscripten"] / "system/lib/libc/musl/COPYRIGHT"),
    ]
    for library in ("libcxx", "libcxxabi", "libunwind", "compiler-rt", "llvm-libc"):
        notices.append((library, paths["emscripten"] / "system/lib" / library / "LICENSE.TXT"))
    for notice in sorted((paths["harfbuzz"] / "src").rglob("COPYING*")):
        if notice.is_file():
            notices.append((f"HarfBuzz {notice.relative_to(paths['harfbuzz'])}", notice))
    rust_docs = paths["rust"] / "share/doc/rust"
    for required in ("COPYRIGHT-library.html", "licenses/MIT.txt", "licenses/Apache-2.0.txt"):
        if not (rust_docs / required).is_file():
            raise RuntimeError(f"Required Rust standard-library notice is missing: {rust_docs / required}")
    for pattern in ("LICENSE*", "COPYRIGHT-library.html", "licenses/*"):
        for notice in sorted(rust_docs.glob(pattern)):
            if notice.is_file():
                notices.append((f"Rust 1.90.0 {notice.name}", notice))
    metadata = json.loads(subprocess.check_output(
        [str(paths["cargo"]), "metadata", "--format-version", "1", "--locked"],
        cwd=ROOT, env=environment, text=True, encoding="utf-8",
    ))
    for package in sorted(metadata["packages"], key=lambda item: (item["name"], item["version"])):
        directory = Path(package["manifest_path"]).parent
        if directory == ROOT:
            continue
        candidates = set()
        for pattern in ("LICENSE*", "COPYING*", "license*", "copyright*"):
            candidates.update(file for file in directory.glob(pattern) if file.is_file())
        if package.get("license_file"):
            candidates.add(directory / package["license_file"])
        if not candidates:
            raise RuntimeError(f"No upstream license notice found for {package['name']} {package['version']}")
        for notice in sorted(candidates):
            notices.append((f"{package['name']} {package['version']} — {notice.name}", notice))
    for title, notice in notices:
        if not notice.is_file():
            raise RuntimeError(f"Required upstream license is missing: {notice}")
        sections.append(f"\n{'=' * 72}\n{title}\n{'=' * 72}\n\n{notice.read_text(encoding='utf-8')}\n")
    distribution = ROOT / "dist"
    distribution.mkdir(exist_ok=True)
    (distribution / "LICENSES.txt").write_text("".join(sections), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--setup-only", action="store_true", help="Install local checksum-pinned prerequisites without building")
    parser.add_argument("--site-dir", type=Path, help="Also package font-generator/index.html inside this static website")
    parser.add_argument("--jobs", type=int, default=max(1, min(os.cpu_count() or 1, 8)), help="Maximum concurrent native compilation jobs")
    arguments = parser.parse_args()
    if arguments.jobs < 1:
        parser.error("--jobs must be positive")
    paths, environment = setup()
    if arguments.setup_only:
        print(f"Pinned prerequisites are ready in {ROOT / '.cache'}")
        return
    cmake = shutil.which("cmake", path=environment["PATH"])
    ninja = shutil.which("ninja", path=environment["PATH"])
    if not cmake or not ninja:
        raise RuntimeError("CMake 3.20+ and Ninja must be available on PATH")
    build = ROOT / "build"
    native = build / "native"
    toolchain = paths["emscripten"] / "cmake/Modules/Platform/Emscripten.cmake"
    run([
        cmake, "-S", ROOT / "native", "-B", native, "-G", "Ninja",
        f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        f"-DFG_FREETYPE_SOURCE={paths['freetype']}", f"-DFG_HARFBUZZ_SOURCE={paths['harfbuzz']}",
    ], environment)
    run([cmake, "--build", native, "--parallel", str(arguments.jobs)], environment)
    environment["FG_NATIVE_LIB_DIR"] = str(native / "lib")
    environment["FG_EMSCRIPTEN_VERSION"] = EMSCRIPTEN_VERSION
    # Build a static Rust archive then choose the .js output explicitly at final
    # link: a Cargo cdylib's .wasm suffix otherwise requests standalone WASM.
    run([
        paths["cargo"], "rustc", "--release", "--target", TARGET,
        "--lib", "--crate-type", "staticlib", "--locked",
    ], environment)
    archive = ROOT / "target" / TARGET / "release/libfonts_generator.a"
    run([
        sys.executable, paths["emscripten"] / "em++.py", archive,
        "-O3", "--no-entry", "-o", build / "engine.js",
        "-sMODULARIZE=1", "-sEXPORT_NAME=createFontEngine", "-sEXPORT_ES6=0",
        "-sENVIRONMENT=worker,node", "-sALLOW_MEMORY_GROWTH=1",
        "-sINITIAL_MEMORY=33554432", "-sMAXIMUM_MEMORY=2147483648", "-sSTACK_SIZE=8388608",
        "-sFORCE_FILESYSTEM=1", "-sDYNAMIC_EXECUTION=0", "-sDISABLE_EXCEPTION_CATCHING=0",
        '-sEXPORTED_FUNCTIONS=["_convert","_free_result","_malloc","_free"]',
        '-sEXPORTED_RUNTIME_METHODS=["ccall","FS","UTF8ToString"]',
    ], environment)
    collect_licenses(paths, environment)
    package_command: list[str | Path] = [sys.executable, ROOT / "scripts/package.py"]
    if arguments.site_dir is not None:
        package_command.extend(("--site-dir", arguments.site_dir.resolve()))
    run(package_command, environment)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
