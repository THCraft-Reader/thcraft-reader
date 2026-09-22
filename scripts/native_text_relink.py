"""Pro-only SCons link capture and portable LGPL object relinker (Python 3.12+)."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

PRO_ENVIRONMENTS = {"x4pro", "x4pro-gh_release", "x4pro-gh_release_rc"}
SCHEMA = 1


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def json_write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def unquote(value):
    value = str(value)
    return value[1:-1] if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'" else value


class ExpandWithoutTempfile:
    """Expand the real LINKCOM, bypassing only SCons' ephemeral response wrapper."""

    def __init__(self, command, *args, **kwargs):
        self.command = command

    def __call__(self, target, source, env, for_signature):
        return self.command


def expanded_command(env, command, target, source):
    clone = env.Clone()
    clone.Replace(TEMPFILE=ExpandWithoutTempfile)
    lines = clone.subst_list(command, target=target, source=source)
    if len(lines) != 1 or not lines[0]:
        raise RuntimeError("Expected exactly one expanded SCons command")
    return [unquote(token) for token in lines[0]]


def expand_responses(arguments, cwd, seen=None):
    seen = set() if seen is None else seen
    result = []
    for argument in arguments:
        if argument.startswith("@"):
            path = (Path(cwd) / unquote(argument[1:])).resolve()
            if path in seen:
                raise ValueError("Recursive linker response file: " + str(path))
            # GCC response syntax, not shell execution. The wrapper itself never runs.
            result.extend(expand_responses(shlex.split(path.read_text(encoding="utf-8"), posix=True),
                                           cwd, seen | {path}))
        else:
            result.append(argument)
    return result


def run_output(command, cwd=None):
    return subprocess.check_output(command, cwd=cwd, text=True, encoding="utf-8").strip()


def compiler_identity(compiler):
    return {"machine": run_output([str(compiler), "-dumpmachine"]),
            "version": run_output([str(compiler), "-dumpfullversion", "-dumpversion"])}


class LinkInputs:
    """Resolve every explicit link input, preserving argument order and grouping."""

    def __init__(self, argv, cwd, roots):
        self.cwd = Path(cwd).resolve()
        self.roots = [(Path(path).resolve(), name) for name, path in roots.items()]
        self.roots.sort(key=lambda entry: len(str(entry[0])), reverse=True)
        self.argv = expand_responses(argv, self.cwd)
        self.compiler = (self.cwd / self.argv[0]).resolve()
        self.files = {}
        self.directories = set()
        self.search = []
        self.output = None
        self.script_seen = set()
        # Treat -Wl payloads as individual ld options; -Xlinker preserves precisely
        # the same option boundaries without inventing a library list.
        self.tokens = []
        arguments = iter(self.argv[1:])
        for argument in arguments:
            if argument.startswith("-Wl,"):
                self.tokens.extend((token, True) for token in expand_responses(argument[4:].split(","), self.cwd))
            elif argument == "-Xlinker":
                following = next(arguments, None)
                if following is None:
                    raise ValueError("Missing argument after -Xlinker")
                self.tokens.extend((token, True) for token in expand_responses([following], self.cwd))
            else:
                self.tokens.append((argument, False))
        i = 0
        while i < len(self.tokens):
            token = self.tokens[i][0]
            if token == "-L":
                i += 1
                self.search.append(self.absolute(self.tokens[i][0]))
            elif token.startswith("-L"):
                self.search.append(self.absolute(token[2:]))
            i += 1

    def absolute(self, path):
        return (self.cwd / unquote(path)).resolve()

    def relocated(self, path):
        path = Path(path).resolve()
        for root, name in self.roots:
            try:
                return "inputs/" + name + "/" + path.relative_to(root).as_posix()
            except ValueError:
                pass
        raise ValueError("Link input is outside recognized project/package roots: " + str(path))

    def directory(self, path):
        relocated = self.relocated(path)
        self.directories.add(relocated)
        return relocated

    def file(self, path, kind):
        path = Path(path).resolve()
        if not path.is_file():
            raise FileNotFoundError("Missing link input: " + str(path))
        relocated = self.relocated(path)
        self.files[relocated] = {"path": relocated, "source": str(path), "kind": kind,
                                 "size": path.stat().st_size, "sha256": digest(path)}
        return relocated

    def resolve(self, name, compiler_search=False, local=None):
        candidates = ([Path(local) / name] if local else []) + [self.cwd / name]
        candidates += [directory / name for directory in self.search]
        for candidate in candidates:
            if candidate.is_file():
                return candidate.resolve()
        if compiler_search:
            candidate = Path(run_output([str(self.compiler), "-print-file-name=" + name]))
            if candidate.is_file():
                return candidate.resolve()
        raise FileNotFoundError("Unresolved linker input: " + name)

    def script(self, name, local=None):
        path = self.resolve(name, local=local)
        relocated = self.file(path, "linker-script")
        self.directory(path.parent)
        if path not in self.script_seen:
            self.script_seen.add(path)
            text = re.sub(r"/\*.*?\*/", "", path.read_text(encoding="utf-8"), flags=re.S)
            # Do not silently miss a script-supplied archive/search path.
            if re.search(r"\b(?:INPUT|GROUP|SEARCH_DIR)\s*\(", text):
                raise ValueError("Unsupported file-bearing linker script directive: " + str(path))
            for include in re.findall(r'\bINCLUDE\s+("[^"]+"|[^\s;]+)', text):
                name = unquote(include)
                if Path(name).is_absolute() or ".." in Path(name).parts:
                    raise ValueError("Nonrelocatable linker script INCLUDE: " + name)
                self.script(name, path.parent)
        return relocated

    def translate(self):
        result = []
        i = 0
        while i < len(self.tokens):
            token, linker = self.tokens[i]
            following = None
            if token in ("-o", "-L", "-T", "--script", "-l", "-Map", "--Map", "--version-script",
                         "--retain-symbols-file", "--just-symbols", "-specs", "--specs"):
                i += 1
                if i >= len(self.tokens):
                    raise ValueError("Missing argument after " + token)
                following = unquote(self.tokens[i][0])
            value = following
            if token == "-o":
                if self.output is not None:
                    raise ValueError("Multiple linker outputs")
                self.output = self.absolute(value)
                translated = ["-o", "{output}"]
            elif token == "-L" or token.startswith("-L"):
                translated = ["-L" + self.directory(self.absolute(value if value is not None else token[2:]))]
            elif token in ("-T", "--script") or token.startswith(("-T", "--script=")):
                value = value if value is not None else token.split("=", 1)[1] if "=" in token else token[2:]
                translated = ["-T", self.script(value)]
            elif token == "-l" or token.startswith("-l"):
                name = value if value is not None else token[2:]
                name = name[1:] if name.startswith(":") else "lib" + name + ".a"
                translated = [self.file(self.resolve(name, compiler_search=True), "archive")]
            elif token in ("-Map", "--Map") or token.startswith(("-Map=", "--Map=")):
                translated = ["-Map={map}"]
            elif token in ("-specs", "--specs") or token.startswith(("-specs=", "--specs=")):
                value = value if value is not None else token.split("=", 1)[1]
                translated = ["--specs=" + self.file(self.resolve(value, compiler_search=True), "specs")]
            elif token in ("--version-script", "--retain-symbols-file", "--just-symbols") or token.startswith(
                    ("--version-script=", "--retain-symbols-file=", "--just-symbols=")):
                value = value if value is not None else token.split("=", 1)[1]
                translated = [token.split("=", 1)[0] + "=" + self.file(self.resolve(value), "linker-data")]
            elif token.endswith((".o", ".a", ".obj", ".ld")) and not token.startswith("-"):
                translated = [self.file(self.resolve(token), "archive" if token.endswith(".a") else "object")]
            else:
                if following is not None or re.search(r"(?:[A-Za-z]:[\\/]|^/|=[/\\])", token):
                    raise ValueError("Unrecognized path-bearing linker argument: " + token)
                translated = [token]
            for item in translated:
                if linker:
                    result.extend(["-Xlinker", item])
                else:
                    result.append(item)
            i += 1
        if self.output is None or not self.output.is_file():
            raise ValueError("Capture requires a successfully linked ELF output")
        return result


def native_compile_flags(elf):
    """Read code-generation flags from the actual vendor object executors."""
    found = {}
    visited = set()

    def visit(node):
        identity = str(node)
        if identity in visited:
            return
        visited.add(identity)
        executor = node.get_executor(create=0)
        if executor is None:
            return
        sources = executor.get_all_sources()
        if identity.endswith((".o", ".obj")) and len(sources) == 1:
            source = str(sources[0]).replace("\\", "/")
            marker = "/NativeText/"
            if marker in source and ("/vendor/libthai-" in source or "/vendor/libdatrie-" in source):
                relative = source.split(marker, 1)[1]
                build_env = executor.get_build_env()
                flags = expanded_command(build_env, "$CCFLAGS $CFLAGS", [node], sources)
                # No project definitions or include paths are needed by these C
                # slices. Retain actual target/ABI/optimization flags; their only
                # port definition and headers are specified in rebuild_lgpl.
                found[relative] = [flag for flag in flags if flag.startswith(("-m", "-f", "-O", "-g", "-std="))]
        for child in node.children():
            visit(child)

    visit(elf)
    return found


def capture_link(env, target, source):
    elf = target[0]
    executor = elf.get_executor()
    build_env = executor.get_build_env()
    sources = executor.get_all_sources()
    root = Path(env.subst("$PROJECT_DIR")).resolve()
    platform = env.PioPlatform()
    roots = {"project": str(root)}
    packages = {}
    for name in platform.packages:
        directory = platform.get_package_dir(name)
        if not directory:
            continue
        roots["packages/" + name] = directory
        metadata = Path(directory) / "package.json"
        if metadata.is_file():
            data = json.loads(metadata.read_text(encoding="utf-8"))
            packages[name] = {key: data[key] for key in ("name", "version", "repository", "license") if key in data}
            specification = platform.packages[name].get("version")
            if isinstance(specification, str) and specification.startswith("https://"):
                packages[name]["source"] = specification
    argv = expanded_command(build_env, "$LINKCOM", target, sources)
    # SCons can resolve LINK through its private PATH rather than the process PATH.
    compiler = build_env.WhereIs(argv[0])
    if not compiler:
        raise FileNotFoundError("Cannot locate SCons linker: " + argv[0])
    argv[0] = str(Path(compiler).resolve())
    inputs = LinkInputs(argv, root, roots)
    relocated = inputs.translate()
    toolchain = Path(platform.get_package_dir("toolchain-xtensa-esp-elf"))
    manifest = {"schema": SCHEMA, "environment": env.subst("$PIOENV"), "project": str(root),
                "argv": inputs.argv, "relocated_argv": relocated, "directories": sorted(inputs.directories),
                "files": sorted(inputs.files.values(), key=lambda item: item["path"]),
                "elf": {"path": str(inputs.output), "sha256": digest(inputs.output)},
                "compiler": {"driver": Path(compiler).name.removesuffix(".exe"),
                             **compiler_identity(compiler)},
                "toolchain": packages["toolchain-xtensa-esp-elf"], "packages": packages,
                "platform": env.GetProjectOption("platform"),
                "vendor_cflags": native_compile_flags(elf)}
    # The executable comes from this exact public package, not a PATH substitute.
    Path(compiler).resolve().relative_to(toolchain.resolve())
    json_write(Path(env.subst("$BUILD_DIR")) / "native-text-link.json", manifest)


def response_text(arguments):
    # GCC response quoting works on Windows and Unix. Never execute via a shell.
    return "".join('"' + argument.replace("\\", "\\\\").replace('"', '\\"') + '"\n'
                   for argument in arguments)


def contained(root, relative):
    path = (Path(root) / relative).resolve()
    path.relative_to(Path(root).resolve())
    return path


def verify_bundle(bundle):
    manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA:
        raise ValueError("Unsupported relink manifest schema")
    for item in manifest["files"]:
        path = contained(bundle, item["path"])
        if not path.is_file() or path.stat().st_size != item["size"] or digest(path) != item["sha256"]:
            raise ValueError("Missing or changed bundle input: " + item["path"])
    return manifest


def find_input(manifest, name):
    matches = [item for item in manifest["files"] if item["kind"] in ("archive", "object") and
               (item["path"] == name or Path(item["path"]).name == name)]
    if len(matches) != 1:
        raise ValueError("Input name must identify exactly one archive/object: " + name)
    return matches[0]["path"]


def tool_path(toolchain, name):
    path = toolchain / "bin" / (name + (".exe" if os.name == "nt" else ""))
    if not path.is_file():
        raise FileNotFoundError("Missing matching toolchain executable: " + str(path))
    return path


def replace_members(bundle, manifest, replacements, work, ar):
    result = {}
    for selector, object_path in replacements:
        archive_name, member = selector.rsplit(":", 1)
        archive = find_input(manifest, archive_name)
        if not member or Path(member).name != member:
            raise ValueError("Invalid archive member: " + member)
        original = contained(bundle, archive)
        members = run_output([str(ar), "t", str(original)]).splitlines()
        if members.count(member) != 1:
            raise ValueError("Archive must contain exactly one member named " + member)
        if archive not in result:
            destination = work / "archives" / archive
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original, destination)
            result[archive] = str(destination)
        staged = work / "members" / member
        staged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(object_path, staged)
        subprocess.run([str(ar), "r", result[archive], str(staged)], check=True)
    return result


def rebuild_lgpl(bundle, manifest, sources, work, compiler):
    native = sources / "lib/NativeText"
    include = work / "include/datrie"
    include.mkdir(parents=True, exist_ok=True)
    for header in (native / "vendor/libdatrie-0.2.14/datrie").glob("*.h"):
        shutil.copyfile(header, include / header.name)
    replacements = []
    for relative, flags in manifest["vendor_cflags"].items():
        source = contained(native, relative)
        output = work / "lgpl" / (source.name + ".o")
        output.parent.mkdir(parents=True, exist_ok=True)
        command = [str(compiler), *flags, "-I" + str(native), "-I" + str(include.parent),
                   "-I" + str(native / "vendor/libthai-0.1.30/include"),
                   "-I" + str(native / "vendor/libthai-0.1.30/src"),
                   "-include", str(native / "port/NativeThaiAllocator.h")]
        if relative.startswith("vendor/libthai-"):
            command.append("-DNATIVE_TEXT_THAI_NO_DEFAULT_DICTIONARY=1")
        command.extend(["-c", str(source), "-o", str(output)])
        subprocess.run(command, check=True)
        replacements.append((manifest["native_archive"] + ":" + output.name, output))
    return replacements


def relink(args):
    bundle = args.bundle.resolve()
    manifest = verify_bundle(bundle)
    toolchain = args.toolchain.resolve()
    metadata = json.loads((toolchain / "package.json").read_text(encoding="utf-8"))
    if any(metadata.get(key) != manifest["toolchain"][key] for key in ("name", "version")):
        raise ValueError("Toolchain package name/version does not match the build manifest")
    driver = tool_path(toolchain, manifest["compiler"]["driver"])
    if compiler_identity(driver) != {key: manifest["compiler"][key] for key in ("machine", "version")}:
        raise ValueError("Compiler target/version does not match the original link")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    replacements = {}
    for item in args.replace:
        name, value = item.split("=", 1)
        path = Path(value).resolve(strict=True)
        key = find_input(manifest, name)
        if key in replacements:
            raise ValueError("Duplicate replacement: " + name)
        replacements[key] = str(path)
    with tempfile.TemporaryDirectory(prefix="native-relink-", dir=output.parent) as temporary:
        work = Path(temporary)
        members = []
        for item in args.replace_member:
            selector, value = item.split("=", 1)
            members.append((selector, Path(value).resolve(strict=True)))
        prefix = manifest["compiler"]["driver"].removesuffix("g++").removesuffix("gcc")
        if args.rebuild_lgpl:
            members.extend(rebuild_lgpl(bundle, manifest, args.rebuild_lgpl.resolve(), work,
                                        tool_path(toolchain, prefix + "gcc")))
        patched = replace_members(bundle, manifest, members, work, tool_path(toolchain, prefix + "ar"))
        if replacements.keys() & patched.keys():
            raise ValueError("Cannot replace an archive and its members in the same invocation")
        replacements.update(patched)
        arguments = []
        for argument in manifest["argv"]:
            if argument == "{output}":
                argument = str(output)
            elif "{map}" in argument:
                argument = argument.replace("{map}", str(output.with_suffix(".map")))
            else:
                # Relative paths also appear attached to -L and --specs=.
                prefix_arg = ""
                value = argument
                for possible in ("-L", "--specs=", "--version-script=", "--retain-symbols-file=", "--just-symbols="):
                    if argument.startswith(possible):
                        prefix_arg, value = possible, argument[len(possible):]
                        break
                if value.startswith("inputs/"):
                    argument = prefix_arg + replacements.get(value, str(contained(bundle, value)))
            arguments.append(argument.replace("\\", "/"))
        for directory in manifest["directories"]:
            contained(bundle, directory).mkdir(parents=True, exist_ok=True)
        response = work / "link.rsp"
        response.write_text(response_text(arguments), encoding="utf-8")
        subprocess.run([str(driver), "@" + str(response)], cwd=bundle, check=True)
    if not output.is_file():
        raise RuntimeError("Linker did not produce the requested ELF")
    print("Relinked " + str(output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--toolchain", type=Path, required=True, help="Matching PlatformIO toolchain package directory")
    parser.add_argument("--output", type=Path, required=True, help="Output firmware ELF; does not flash")
    parser.add_argument("--replace", action="append", default=[], metavar="INPUT=FILE",
                        help="Replace one linked archive/object (relative manifest path or unique basename)")
    parser.add_argument("--replace-member", action="append", default=[], metavar="ARCHIVE:MEMBER=OBJECT",
                        help="Replace one archive member while retaining all other firmware objects")
    parser.add_argument("--rebuild-lgpl", type=Path, metavar="SOURCE_ROOT",
                        help="Compile modified LibThai/libdatrie sources and replace their native archive members")
    relink(parser.parse_args())


def register_scons(env):
    if env.subst("$PIOENV") not in PRO_ENVIRONMENTS:
        raise RuntimeError("Native relink hook must only be attached to X4 Pro environments")
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", capture_link)

    def package(target, source, env):
        # An up-to-date ELF does not run post-actions, so refresh the manifest
        # explicitly for the package target using the ELF's original executor.
        elf = env.File(env.subst("$BUILD_DIR/${PROGNAME}.elf"))
        capture_link(env, [elf], [])
        subprocess.run([sys.executable, str(Path(env.subst("$PROJECT_DIR")) / "scripts/package_native_text_relink.py"),
                        "--manifest", env.subst("$BUILD_DIR/native-text-link.json"),
                        "--output", env.subst("$BUILD_DIR/native-text-relink.zip")], check=True)

    env.AddCustomTarget("native-text-relink", ["$BUILD_DIR/${PROGNAME}.bin"], package,
                        title="Package native text relink kit",
                        description="Build X4 Pro and retain LGPL sources, notices and exact link inputs")


if "Import" in globals():
    Import("env")
    register_scons(env)
elif __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit("native-text-relink: " + str(error))
