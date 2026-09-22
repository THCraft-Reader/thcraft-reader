"""Generate pinned Pro assets with a host compiler, never the ESP32 compiler."""

from pathlib import Path
import os
import shutil
import subprocess
import sys


def build_native_text_assets(project_root):
    root = Path(project_root).resolve()
    build = root / "build" / "native-text-assets"
    suffix = ".exe" if os.name == "nt" else ""
    sibling_cmake = Path(sys.executable).parent / ("cmake" + suffix)
    cmake = shutil.which("cmake") or (str(sibling_cmake) if sibling_cmake.is_file() else None)
    if not cmake:
        raise RuntimeError("NativeText assets require host CMake and a C/C++ compiler; install scripts' pinned Python dependencies")
    if not (build / "CMakeCache.txt").is_file():
        subprocess.run([
            cmake, "-S", str(root / "lib/NativeText"), "-B", str(build),
            "-DPython3_EXECUTABLE=" + sys.executable,
        ], check=True)
    subprocess.run([cmake, "--build", str(build), "--config", "Release", "--target", "NativeTextAssets"], check=True)
    generated = build / "generated"
    for name in ("NativeFontAssets.generated.cpp", "NativeThaiDictionary.generated.cpp"):
        if not (generated / name).is_file():
            raise RuntimeError("NativeText asset generation did not produce " + name)
    return generated
