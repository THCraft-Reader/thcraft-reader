"""Install checksum-pinned build inputs inside this project; never alter PATH globally."""
from __future__ import annotations

import hashlib
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache"
RUST_VERSION = "1.90.0"
EMSCRIPTEN_VERSION = "4.0.15"
EMSCRIPTEN_REVISION = "b412b6307e541b93dd93f01b61181e15c17302ec"
TARGET = "wasm32-unknown-emscripten"

# Hashes are pinned independently of the downloaded files. A changed upstream
# archive fails closed, including an archive already present in the local cache.
SOURCES = {
    "freetype": (
        "freetype-2.13.3.tar.gz",
        "https://codeload.github.com/freetype/freetype/tar.gz/refs/tags/VER-2-13-3",
        "bc5c898e4756d373e0d991bab053036c5eb2aa7c0d5c67e8662ddc6da40c4103",
        "freetype-VER-2-13-3",
    ),
    "harfbuzz": (
        "harfbuzz-10.4.0.tar.gz",
        "https://codeload.github.com/harfbuzz/harfbuzz/tar.gz/refs/tags/10.4.0",
        "0d25a3f74af4e8744700ac19050af5a80ae330378a5802a5cd71e523bb6fda1f",
        "harfbuzz-10.4.0",
    ),
}
SDK_ARCHIVES = {
    "Windows": ("win", "zip", "726046170075416370af4f13276990dde9560a844c14ece74912fce6fffd03da"),
    "Linux": ("linux", "tar.xz", "c0ed25c30e1d747072de0c9053cde27ca83a8d2424e8bc6b7d39dccf42fcba35"),
}
NODE_ARCHIVES = {
    "Windows": ("win-x64", "zip", "21c2d9735c80b8f86dab19305aa6a9f6f59bbc808f68de3eef09d5832e3bfbbd"),
    "Linux": ("linux-x64", "tar.xz", "f4cb75bb036f0d0eddf6b79d9596df1aaab9ddccd6a20bf489be5abe9467e84e"),
}


def run(arguments: list[str | Path], environment: dict[str, str]) -> None:
    command = [str(argument) for argument in arguments]
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=ROOT, env=environment, check=True)


def download(name: str, url: str, checksum: str) -> Path:
    destination = CACHE / "downloads" / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists():
        temporary = destination.with_suffix(destination.suffix + ".part")
        print(f"Downloading {url}", flush=True)
        try:
            with urllib.request.urlopen(url, timeout=120) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
            with temporary.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            if digest != checksum:
                raise RuntimeError(f"SHA-256 mismatch for {name}: expected {checksum}, received {digest}")
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
    with destination.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    if digest != checksum:
        raise RuntimeError(f"SHA-256 mismatch for cached {destination}; remove that file and run setup again")
    return destination


def extract(archive: Path, destination: Path, archive_root: str, checksum: str) -> Path:
    marker = destination / ".source-sha256"
    if marker.exists() and marker.read_text(encoding="ascii").strip() == checksum:
        return destination
    if destination.exists():
        raise RuntimeError(f"Unrecognized or incomplete dependency directory {destination}; remove it and rerun setup")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="extract-", dir=CACHE) as staging:
        staging_path = Path(staging)
        if zipfile.is_zipfile(archive):
            with zipfile.ZipFile(archive) as package:
                for member in package.infolist():
                    resolved = (staging_path / member.filename).resolve()
                    if not resolved.is_relative_to(staging_path.resolve()):
                        raise RuntimeError(f"Unsafe archive member: {member.filename}")
                package.extractall(staging_path)
        else:
            with tarfile.open(archive) as package:
                package.extractall(staging_path, filter="data")
        extracted = staging_path / archive_root
        if not extracted.is_dir():
            raise RuntimeError(f"Archive {archive.name} does not contain {archive_root}")
        extracted.rename(destination)
    marker.write_text(checksum + "\n", encoding="ascii")
    return destination


def base_environment() -> dict[str, str]:
    environment = os.environ.copy()
    for variable, relative in {
        "RUSTUP_HOME": "rustup", "CARGO_HOME": "cargo", "TMP": "tmp",
        "TEMP": "tmp", "TMPDIR": "tmp", "EMCC_TEMP_DIR": "tmp",
    }.items():
        directory = CACHE / relative
        directory.mkdir(parents=True, exist_ok=True)
        environment[variable] = str(directory)
    environment["CARGO_TARGET_DIR"] = str(ROOT / "target")
    environment["RUSTUP_NO_UPDATE_CHECK"] = "1"
    environment["PYTHONPYCACHEPREFIX"] = str(CACHE / "pycache")
    return environment


def setup() -> tuple[dict[str, Path], dict[str, str]]:
    if sys.version_info < (3, 12):
        raise RuntimeError("Python 3.12 or newer is required for safe archive extraction")
    system = platform.system()
    if system not in SDK_ARCHIVES or platform.machine().lower() not in ("amd64", "x86_64"):
        raise RuntimeError("The pinned toolchain supports Windows x64 and Linux x86_64")
    paths = {}
    environment = base_environment()
    for name, (filename, url, checksum, archive_root) in SOURCES.items():
        archive = download(filename, url, checksum)
        paths[name] = extract(archive, CACHE / "sources" / archive_root, archive_root, checksum)
    sdk_platform, sdk_extension, sdk_checksum = SDK_ARCHIVES[system]
    sdk_name = f"emscripten-{EMSCRIPTEN_VERSION}-{sdk_platform}.{sdk_extension}"
    sdk_url = f"https://storage.googleapis.com/webassembly/emscripten-releases-builds/{sdk_platform}/{EMSCRIPTEN_REVISION}/wasm-binaries.{sdk_extension}"
    sdk_archive = download(sdk_name, sdk_url, sdk_checksum)
    paths["sdk"] = extract(sdk_archive, CACHE / "toolchain" / "install", "install", sdk_checksum)
    paths["emscripten"] = paths["sdk"] / "emscripten"
    node_platform, node_extension, node_checksum = NODE_ARCHIVES[system]
    node_root = f"node-v22.16.0-{node_platform}"
    node_name = f"{node_root}.{node_extension}"
    node_archive = download(node_name, f"https://nodejs.org/dist/v22.16.0/{node_name}", node_checksum)
    paths["node_root"] = extract(node_archive, CACHE / "toolchain" / node_root, node_root, node_checksum)
    paths["node"] = paths["node_root"] / ("node.exe" if system == "Windows" else "bin/node")
    configuration = CACHE / "emscripten.py"
    config_text = (
        f"LLVM_ROOT = {str(paths['sdk'] / 'bin')!r}\n"
        f"BINARYEN_ROOT = {str(paths['sdk'])!r}\n"
        f"NODE_JS = [{str(paths['node'])!r}]\n"
    )
    if not configuration.exists() or configuration.read_text(encoding="utf-8") != config_text:
        configuration.write_text(config_text, encoding="utf-8")
    environment["EM_CONFIG"] = str(configuration)
    environment["EM_CACHE"] = str(paths["emscripten"] / "cache")
    environment["EMSDK_PYTHON"] = sys.executable
    environment["EMSDK_NODE"] = str(paths["node"])
    environment["PATH"] = os.pathsep.join((str(paths["emscripten"]), str(paths["node"].parent), environment.get("PATH", "")))
    rustup = shutil.which("rustup", path=environment["PATH"])
    if not rustup:
        raise RuntimeError("rustup must already be installed; setup uses it with project-local RUSTUP_HOME and CARGO_HOME")
    paths["rustup"] = Path(rustup)
    host = "x86_64-pc-windows-msvc" if system == "Windows" else "x86_64-unknown-linux-gnu"
    paths["rust"] = CACHE / "rustup" / "toolchains" / f"{RUST_VERSION}-{host}"
    # rustup verifies official component checksums and keeps downloads locally.
    rust_binary = paths["rust"] / "bin" / ("rustc.exe" if system == "Windows" else "rustc")
    target_library = paths["rust"] / "lib" / "rustlib" / TARGET / "lib"
    if not rust_binary.exists() or not target_library.is_dir():
        run([rustup, "toolchain", "install", RUST_VERSION, "--profile", "minimal", "--target", TARGET, "--no-self-update"], environment)
    environment["PATH"] = str(paths["rust"] / "bin") + os.pathsep + environment["PATH"]
    environment["RUSTC"] = str(rust_binary)
    paths["cargo"] = paths["rust"] / "bin" / ("cargo.exe" if system == "Windows" else "cargo")
    return paths, environment


if __name__ == "__main__":
    try:
        installed, env = setup()
        run([env["RUSTC"], "--version"], env)
        run([sys.executable, installed["emscripten"] / "emcc.py", "--version"], env)
        run([installed["node"], "--version"], env)
        print(f"Pinned prerequisites installed below {CACHE}")
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
