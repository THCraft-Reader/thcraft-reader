"""Compile only the pinned native slices, with private vendor configuration."""

from pathlib import Path
import sys
from SCons.Script import DefaultEnvironment

Import("env")


def configure_native():
    root = Path(env.subst("$PROJECT_DIR")) / "lib" / "NativeText"
    pro_environments = {"x4pro", "x4pro-gh_release", "x4pro-gh_release_rc"}
    # PlatformIO initializes library builders before applying lib_ignore.
    if env.subst("$PIOENV") not in pro_environments:
        env.Replace(SRC_FILTER=["-<*>"])
        return

    sys.path.insert(0, str(root.parent.parent / "scripts"))
    from build_native_text_assets import build_native_text_assets

    asset_directory = build_native_text_assets(root.parent.parent)
    sources = {source.name: "native" for source in root.glob("*.cpp")}
    for slice_name in ("freetype", "harfbuzz", "thai", "datrie"):
        for line in (root / "port" / (slice_name + "-sources.txt")).read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                sources[line] = slice_name

    # Do not expose libdatrie's root-level "version" file to C++20 include lookup.
    vendor_include = Path(env.subst("$BUILD_DIR")) / "native-vendor-include"
    (vendor_include / "datrie").mkdir(parents=True, exist_ok=True)
    for header in (root / "vendor/libdatrie-0.2.14/datrie").glob("*.h"):
        destination = vendor_include / "datrie" / header.name
        data = header.read_bytes()
        if not destination.exists() or destination.read_bytes() != data:
            destination.write_bytes(data)

    env.Replace(SRC_FILTER=["-<*>"] + ["+<" + source + ">" for source in sources])
    env.Prepend(CPPPATH=[
        str(root / "port/freetype"),
        str(root / "vendor/freetype-2.14.3/include"),
        str(root / "vendor/harfbuzz-14.5.0/src"),
        str(root / "vendor/libthai-0.1.30/include"),
        str(vendor_include),
        str(root),
        str(root.parent / "MiniBidi"),
        str(root.parent.parent / "src"),
        str(root / "vendor/libthai-0.1.30/src"),
        str(asset_directory),
    ])
    # ESP-IDF's include scan and PlatformIO's build scan initialize this library separately.
    project_env = DefaultEnvironment()
    asset_target = str(Path(env.subst("$BUILD_DIR")) / "native-assets")
    registered = project_env.setdefault("NATIVE_TEXT_ASSET_TARGETS", set())
    if asset_target not in registered:
        env.BuildSources(asset_target, str(asset_directory), "+<*.generated.cpp>")
        registered.add(asset_target)

    def configure_vendor(build_env, node):
        relative = Path(node.srcnode().get_abspath()).relative_to(root).as_posix()
        slice_name = sources[relative]
        private = build_env.Clone()
        private.ProcessUnFlags("-fno-lto")
        private.AppendUnique(CCFLAGS=["-flto"])
        if slice_name == "freetype":
            private.Append(CPPDEFINES=[("FT2_BUILD_LIBRARY", 1)])
        elif slice_name == "harfbuzz":
            private.Prepend(CPPPATH=[str(root / "port/harfbuzz")])
            private.Append(CPPDEFINES=[("HAVE_CONFIG_OVERRIDE_H", 1)])
        elif slice_name in ("thai", "datrie"):
            private.Append(CCFLAGS=["-include", str(root / "port/NativeThaiAllocator.h")])
            if slice_name == "thai":
                private.Append(CPPPATH=[str(root / "vendor/libthai-0.1.30/src")])
                private.Append(CPPDEFINES=[("NATIVE_TEXT_THAI_NO_DEFAULT_DICTIONARY", 1)])
        return private.Object(node)

    env.AddBuildMiddleware(configure_vendor)


configure_native()
