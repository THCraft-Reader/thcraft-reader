"""Package a captured X4 Pro link with corresponding native sources and notices."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

from native_text_relink import (PRO_ENVIRONMENTS, SCHEMA, contained, digest, json_write,
                                response_text, run_output)


class Bundle:
    def __init__(self, directory):
        self.directory = directory
        self.files = {}

    def add(self, source, relative, kind, expected=None):
        source = Path(source)
        if not source.is_file():
            raise FileNotFoundError("Required distribution input is missing: " + str(source))
        if relative in self.files:
            return
        target = contained(self.directory, relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        item = {"path": relative, "kind": kind, "size": target.stat().st_size, "sha256": digest(target)}
        if expected is not None and any(item[key] != expected[key] for key in ("size", "sha256")):
            raise ValueError("Input changed since the ELF link; rebuild before packaging: " + str(source))
        self.files[relative] = item

    def source(self, root, relative, kind="source"):
        source = contained(root, relative)
        self.add(source, "sources/" + relative, kind)

    def text(self, relative, content, kind):
        path = contained(self.directory, relative)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        self.files[relative] = {"path": relative, "kind": kind, "size": path.stat().st_size, "sha256": digest(path)}


def verified_archive(pin, cache):
    name = pin["url"].rsplit("/", 1)[-1]
    if name != Path(name).name or not pin["url"].startswith("https://"):
        raise ValueError("Invalid pinned source URL")
    path = cache / name
    if not path.is_file():
        cache.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".part")
        try:
            with urllib.request.urlopen(pin["url"], timeout=120) as response, temporary.open("wb") as output:
                if not response.url.startswith("https://"):
                    raise ValueError("Source download redirected away from HTTPS")
                shutil.copyfileobj(response, output)
            if digest(temporary) != pin["sha256"]:
                raise ValueError("Source archive SHA-256 mismatch: " + name)
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)
    if digest(path) != pin["sha256"]:
        raise ValueError("Cached source archive SHA-256 mismatch: " + name)
    return path


def native_sources(bundle, root, cache):
    native = root / "lib/NativeText"
    pins = json.loads((native / "vendor-sources.json").read_text(encoding="utf-8"))
    if {pin["name"] for pin in pins["sources"]} != {"freetype", "harfbuzz", "libthai", "libdatrie"}:
        raise ValueError("Incomplete native vendor pin manifest")
    modifications = []
    for pin in pins["sources"]:
        archive_path = verified_archive(pin, cache)
        bundle.add(archive_path, "upstream/" + archive_path.name, "upstream-archive")
        vendor = contained(native, pin["directory"])
        expected_root = vendor.name
        included = set()
        with tarfile.open(archive_path, "r:*") as archive:
            for member in archive:
                if not member.isfile() and not member.issym() and not member.islnk():
                    continue
                parts = PurePosixPath(member.name).parts
                if not parts or parts[0] != expected_root or ".." in parts:
                    raise ValueError("Unexpected upstream archive member: " + member.name)
                relative = PurePosixPath(*parts[1:]).as_posix()
                source = contained(vendor, relative)
                path = source.relative_to(root).as_posix()
                bundle.source(root, path)
                included.add(relative)
                # Record every deviation from the pinned release, including files
                # added by a port patch. Original release archives remain intact.
                with archive.extractfile(member) as stream:
                    upstream = stream.read()
                current = source.read_bytes()
                if current.replace(b"\r\n", b"\n") != upstream.replace(b"\r\n", b"\n"):
                    modifications.append({"path": path, "upstream_sha256": hashlib.sha256(upstream).hexdigest(),
                                          "distributed_sha256": digest(source)})
        patch = native / "port" / (expected_root + "-native.patch")
        if pin["name"] != "harfbuzz" and not patch.is_file():
            raise FileNotFoundError("Required vendor port patch is missing: " + str(patch))
        if patch.is_file():
            for name in re.findall(r"^\+\+\+ b/([^\t\n]+)", patch.read_text(encoding="utf-8"), re.M):
                prefix = expected_root + "/"
                if not name.startswith(prefix):
                    raise ValueError("Patch modifies another source tree: " + name)
                relative = name[len(prefix):]
                if relative not in included:
                    path = (vendor / relative).relative_to(root).as_posix()
                    bundle.source(root, path)
                    modifications.append({"path": path, "upstream_sha256": None,
                                          "distributed_sha256": digest(contained(root, path))})
                    included.add(relative)
        bundle.source(root, (vendor / pin["licenseFile"]).relative_to(root).as_posix(), "license")
    # Only recognized native source/tooling trees, never the whole checkout,
    # local PlatformIO config, .git, build caches, credentials or random SD files.
    for path in native.iterdir():
        if path.is_file() and (path.suffix in (".h", ".cpp") or
                               path.name in ("CMakeLists.txt", "vendor-sources.json", "library.json", "platformio_build.py")):
            bundle.source(root, path.relative_to(root).as_posix())
    for directory in ("port", "cmake", "tools"):
        for path in (native / directory).rglob("*"):
            if path.is_file() and path.suffix in (".h", ".c", ".cpp", ".json", ".txt", ".patch", ".cmake"):
                bundle.source(root, path.relative_to(root).as_posix())
    return pins, modifications


def font_sources(bundle, root):
    scripts = root / "lib/EpdFont/scripts"
    manifest = json.loads((scripts / "native-text-assets.json").read_text(encoding="utf-8"))
    source_root = (scripts / manifest["source_root"]).resolve()
    source_root.relative_to(root / "lib/EpdFont")
    for font in manifest["fonts"]:
        path = contained(source_root, font["path"])
        if path.stat().st_size != font["size"] or digest(path) != font["sha256"]:
            raise ValueError("Pinned font source does not match manifest: " + font["path"])
        bundle.source(root, path.relative_to(root).as_posix(), "font-source")
    for license in manifest["licenses"].values():
        path = contained(source_root, license["path"])
        actual = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
        if actual != license["sha256_lf"]:
            raise ValueError("Pinned font license does not match manifest: " + license["path"])
        bundle.source(root, path.relative_to(root).as_posix(), "license")
    for name in ("build-native-text-assets.py", "build-native-thai-dictionary.py", "font_ranges.py",
                 "requirements-native-text.txt", "native-text-assets.json", "build-native-font-catalogue.py",
                 "font_sources.py", "sd-fonts.yaml", "native-font-sources.lock.json", "native-font-catalogue.json"):
        bundle.source(root, "lib/EpdFont/scripts/" + name, "tooling")
    for name in ("gen_i18n.py", "build_native_text_assets.py", "native_text_relink.py", "package_native_text_relink.py"):
        bundle.source(root, "scripts/" + name, "tooling")
    for path in (root / "lib/I18n/translations").glob("*.yaml"):
        bundle.source(root, path.relative_to(root).as_posix(), "asset-input")
    # NativeText's host CMake graph references these production helpers even
    # when building only the dictionary/assets target.
    for directory in ("lib/MiniBidi", "lib/Utf8", "lib/Epub/Epub/hyphenation"):
        for path in (root / directory).rglob("*"):
            if path.is_file() and path.suffix in (".h", ".c", ".cpp", ".t"):
                bundle.source(root, path.relative_to(root).as_posix(), "tooling-dependency")
    bundle.source(root, "src/fontIds.h", "tooling-dependency")
    bundle.source(root, "lib/Epub/Epub/CjkBreakPolicy.h", "tooling-dependency")


def expected_vendor_sources(root):
    native = root / "lib/NativeText"
    result = set()
    for name in ("thai", "datrie"):
        for line in (native / "port" / (name + "-sources.txt")).read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.lstrip().startswith("#"):
                result.add(line.strip())
    return result


def package(manifest_path, output, archive_cache=None):
    capture = json.loads(manifest_path.read_text(encoding="utf-8"))
    if capture.get("schema") != SCHEMA or capture.get("environment") not in PRO_ENVIRONMENTS:
        raise ValueError("Only a current X4 Pro capture can be packaged")
    root = Path(capture["project"]).resolve()
    elf = Path(capture["elf"]["path"])
    if not elf.is_file() or digest(elf) != capture["elf"]["sha256"]:
        raise ValueError("ELF is missing or changed since link capture")
    if set(capture["vendor_cflags"]) != expected_vendor_sources(root):
        raise ValueError("Missing vendor compiler recipes: cannot provide a complete LGPL rebuild path")
    native_archives = [item for item in capture["files"] if Path(item["path"]).name == "libNativeText.a"]
    if len(native_archives) != 1:
        raise ValueError("Expected exactly one actual libNativeText.a link input")
    binary = elf.with_suffix(".bin")
    if not binary.is_file():
        raise FileNotFoundError("Build firmware.bin before creating its corresponding relink kit")
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-package-", dir=output.parent) as temporary:
        stage = Path(temporary) / "native-text-relink"
        stage.mkdir()
        bundle = Bundle(stage)
        for item in capture["files"]:
            bundle.add(item["source"], item["path"], item["kind"], item)
        pins, modifications = native_sources(bundle, root, archive_cache or root / "build/native-downloads")
        font_sources(bundle, root)
        generated = root / "build/native-text-assets/generated"
        for name in ("NativeFontAssets.generated.cpp", "NativeFontAssets.generated.h", "NativeFontAssets.metadata.json",
                     "NativeThaiDictionary.generated.cpp", "NativeThaiDictionary.generated.h", "thbrk.metadata.json",
                     "tdict.txt", "thbrk.tri", "NativeFontCatalogue.generated.h", "NativeFontCatalogue.generated.cpp"):
            bundle.add(generated / name, "generated/" + name, "generated-asset")
        bundle.source(root, "LICENSE", "license")
        bundle.source(root, "platformio.ini", "tooling")
        bundle.add(root / "docs/native-text-licensing.md", "NATIVE-TEXT-LICENSING.md", "license")
        bundle.add(root / "scripts/native_text_relink.py", "native_text_relink.py", "tooling")
        # Package only notices from other linked firmware dependencies, not their
        # private configuration or unrelated source/build trees.
        notice_roots = [root / "lib", root / "freeink-sdk", root / ".pio/libdeps" / capture["environment"]]
        for directory in notice_roots:
            if not directory.is_dir():
                continue
            for path in directory.rglob("*"):
                if path.is_file() and re.fullmatch(r"(?:LICENSE|LICENCE|COPYING|NOTICE|AUTHORS)(?:[.\-_].*)?", path.name, re.I):
                    if not any(part in (".git", "__pycache__", ".cache", "node_modules") for part in path.parts):
                        bundle.add(path, "notices/" + path.relative_to(root).as_posix(), "license")
        template = [argument.replace("{output}", "relinked-firmware.elf").replace("{map}", "relinked-firmware.map")
                    for argument in capture["relocated_argv"]]
        bundle.text("link.rsp", response_text(template), "link-response")
        manifest = {"schema": SCHEMA, "environment": capture["environment"],
                    "argv": capture["relocated_argv"], "directories": capture["directories"],
                    "compiler": capture["compiler"], "toolchain": capture["toolchain"],
                    "platform": capture["platform"], "packages": capture["packages"],
                    "vendor_cflags": capture["vendor_cflags"], "native_archive": native_archives[0]["path"],
                    "source_pins": pins, "vendor_modifications": modifications,
                    "original_firmware": {"elf_sha256": capture["elf"]["sha256"], "bin_sha256": digest(binary)},
                    "source_revision": run_output(["git", "rev-parse", "HEAD"], root),
                    "files": sorted(bundle.files.values(), key=lambda item: item["path"])}
        json_write(stage / "manifest.json", manifest)
        # Every copied byte has already been hashed. Zip is created atomically;
        # neither a missing input nor a failed download publishes a partial kit.
        archive = Path(temporary) / "relink.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zipped:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    info = zipfile.ZipInfo("native-text-relink/" + path.relative_to(stage).as_posix())
                    info.compress_type = zipfile.ZIP_DEFLATED
                    info.external_attr = 0o100644 << 16
                    zipped.writestr(info, path.read_bytes())
        archive.replace(output)
    print("Packaged " + str(output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path, help="Successful SCons native-text-link.json capture")
    parser.add_argument("--output", required=True, type=Path, help="Destination ZIP (replaced only after success)")
    parser.add_argument("--archive-cache", type=Path, help="Cache of SHA-256-pinned upstream release archives")
    args = parser.parse_args()
    package(args.manifest, args.output, args.archive_cache)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit("package-native-text-relink: " + str(error))
