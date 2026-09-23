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
SCHEMA = 2


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


def driver_name(path):
    name = Path(path).name
    return name[:-4] if name.lower().endswith(".exe") else name


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


def expand_responses(arguments, cwd, seen=None, files=None):
    seen = set() if seen is None else seen
    result = []
    for argument in arguments:
        if argument.startswith("@"):
            path = (Path(cwd) / unquote(argument[1:])).resolve()
            if path in seen:
                raise ValueError("Recursive linker response file: " + str(path))
            if files is not None:
                files.add(path)
            # GCC response syntax, not shell execution. The wrapper itself never runs.
            result.extend(expand_responses(shlex.split(path.read_text(encoding="utf-8"), posix=True),
                                           cwd, seen | {path}, files))
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


def collect_native_commands(elf, project):
    """Read per-object private commands while the SCons build graph is intact."""
    native = project / "lib/NativeText"
    found = {}
    visited = set()

    def visit(node, archives=()):
        identity = node.get_abspath()
        key = (identity, archives)
        if key in visited:
            return
        visited.add(key)
        if identity.endswith(".a"):
            archives = archives + (identity,)
        sources = list(getattr(node, "sources", ()))
        if identity.endswith((".o", ".obj")):
            native_sources = []
            for source in sources:
                path = Path(source.srcnode().get_abspath()).resolve()
                try:
                    relative = path.relative_to(native).as_posix()
                except ValueError:
                    continue
                if relative.startswith(("vendor/libthai-", "vendor/libdatrie-")) and path.suffix == ".c":
                    native_sources.append((source, relative))
            if native_sources:
                if len(native_sources) != 1:
                    raise ValueError("Expected one LGPL C source per object: " + identity)
                source, relative = native_sources[0]
                build_env = node.get_build_env()
                clone = build_env.Clone()
                clone.Replace(TEMPFILE=ExpandWithoutTempfile)
                lines = clone.subst_list("$CCCOM", target=[node], source=[source])
                if len(lines) != 1 or not lines[0]:
                    raise ValueError("Expected one native C compiler command")
                # Match the platform's real GCC response-file escaping, including
                # quoted -D string values and Windows path separators.
                from SCons.Subst import quote_spaces
                escape = build_env.get("TEMPFILEARGESCFUNC", quote_spaces)
                tokens = [str(lines[0][0])] + shlex.split(
                    " ".join(escape(token) for token in lines[0][1:]), posix=True)
                compiler = build_env.WhereIs(unquote(tokens[0]))
                if not compiler:
                    raise FileNotFoundError("Cannot locate native C compiler: " + tokens[0])
                tokens[0] = str(Path(compiler).resolve())
                response_files = set()
                command = expand_responses(tokens, project, files=response_files)
                recipe = {"command": command, "object": identity, "archives": list(archives),
                          "responses": [str(path) for path in sorted(response_files)]}
                if relative in found and found[relative] != recipe:
                    raise ValueError("Ambiguous build recipe for native source: " + relative)
                found[relative] = recipe
            return
        for child in node.children():
            visit(child, archives)

    visit(elf)
    return found


def capture_compile_dependencies(command, inputs, toolchain, directory, relative):
    """Ask the real compiler for all headers, including system-header descendants."""
    dependency_file = directory / (relative + ".d")
    dependency_file.parent.mkdir(parents=True, exist_ok=True)
    response_file = dependency_file.with_suffix(".rsp")
    arguments = []
    i = 1
    while i < len(command):
        token = command[i]
        if token in ("-o", "-MF", "-MT", "-MQ"):
            i += 1
            if i >= len(command):
                raise ValueError("Missing compiler output argument after " + token)
        elif token not in ("-c", "-S", "-E", "-M", "-MM", "-MD", "-MMD", "-MP", "-MG") and not (
                token.startswith(("-MF", "-MT", "-MQ")) and len(token) > 3):
            arguments.append(token)
        i += 1
    # Override only output/dependency mode. Definitions, include order, forced
    # includes, target and language switches are the original private recipe.
    arguments.extend(["-M", "-MF", dependency_file.as_posix(), "-MT", "native-text-dependencies"])
    response_file.write_text(response_text(arguments), encoding="utf-8")
    subprocess.run([command[0], "@" + str(response_file)], cwd=inputs.cwd, check=True)
    text = dependency_file.read_text(encoding="utf-8").replace("\\\n", " ")
    rule = re.split(r":\s+", text, maxsplit=1)
    if len(rule) != 2:
        raise ValueError("Malformed full compiler dependency output: " + str(dependency_file))
    headers = set()
    for dependency in shlex.split(rule[1].split("\n", 1)[0], posix=True):
        path = inputs.absolute(dependency)
        if not path.is_file():
            raise FileNotFoundError("Missing compiler dependency: " + str(path))
        # Standard headers are supplied by the matching public compiler package;
        # SDK headers remain required even when reached through #include_next
        # or a header marked as a system header.
        if not path.is_relative_to(toolchain):
            headers.add(inputs.file(path, "compile-header"))
    if not headers:
        raise ValueError("Compiler did not report the native source dependency")
    return {"method": "gcc-M-v1", "headers": sorted(headers),
            "dependency_file": inputs.file(dependency_file, "compile-dependencies"),
            "response_file": inputs.file(response_file, "compile-response")}


def capture_native_recipes(commands, inputs, project, build_directory, toolchain):
    """Relocate exact compiler argv and retain its complete SDK/header closure."""
    native = project / "lib/NativeText"
    vendor_include = build_directory / "native-vendor-include"
    recipes = {}

    def compile_path(value, directory=False):
        path = inputs.absolute(value)
        for root, marker in ((native, "{sources}/lib/NativeText"), (vendor_include, "{vendor_include}"),
                             (toolchain, "{toolchain}")):
            try:
                suffix = path.relative_to(root).as_posix()
                return marker if suffix == "." else marker + "/" + suffix
            except ValueError:
                pass
        if directory:
            return inputs.directory(path)
        return inputs.file(path, "compile-input")

    for relative, original in sorted(commands.items()):
        object_path = Path(original["object"]).resolve()
        object_input = inputs.relocated(object_path)
        if object_input in inputs.files and inputs.files[object_input]["kind"] == "object":
            replacement = {"input": object_input}
        else:
            archives = [inputs.relocated(path) for path in original["archives"]
                        if inputs.relocated(path) in inputs.files]
            if len(archives) != 1 or inputs.files[archives[0]]["kind"] != "archive":
                raise ValueError("Native object has no unique actual linked input: " + str(object_path))
            replacement = {"input": archives[0], "member": object_path.name}
        command = original["command"]
        arguments = []
        directories = ("-isystem", "-iquote", "-idirafter", "-iprefix", "-isysroot", "--sysroot=", "-I")
        files = ("-include", "-imacros", "--specs=", "-specs=")
        i = 1
        while i < len(command):
            token = command[i]
            if token in ("-o", "-MF", "-MT", "-MQ"):
                i += 1
                if i >= len(command):
                    raise ValueError("Missing compiler output after " + token)
                arguments.extend([token, "{depfile}" if token == "-MF" else "{object}"])
            elif token.startswith(("-MF", "-MT", "-MQ")) and len(token) > 3:
                arguments.append(token[:3] + ("{depfile}" if token.startswith("-MF") else "{object}"))
            else:
                matched = False
                for prefix in directories + files:
                    if token == prefix or token.startswith(prefix):
                        separate = token == prefix
                        if separate:
                            i += 1
                            if i >= len(command):
                                raise ValueError("Missing compiler path after " + token)
                            value = command[i]
                        else:
                            value = token[len(prefix):]
                        path = compile_path(value, prefix in directories)
                        arguments.extend([token, path] if separate else [prefix + path])
                        matched = True
                        break
                if not matched:
                    if token in ("-iwithprefix", "-iwithprefixbefore"):
                        i += 1
                        arguments.extend([token, command[i]])
                    elif not token.startswith("-") and inputs.absolute(token) == native / relative:
                        arguments.append("{sources}/lib/NativeText/" + relative)
                    elif re.search(r"(?:[A-Za-z]:[\\/]|^/|=[/\\])", token):
                        raise ValueError("Unrecognized path-bearing compiler argument: " + token)
                    else:
                        arguments.append(token)
            i += 1
        dependencies = capture_compile_dependencies(command, inputs, toolchain,
                                                    build_directory / "native-text-dependencies", relative)
        response_inputs = [inputs.file(path, "compile-response") for path in original["responses"]]
        recipes[relative] = {"driver": driver_name(command[0]), "argv": arguments,
                             "replacement": replacement, "responses": response_inputs, "dependencies": dependencies}
    return recipes


def capture_link(env, target, source, commands):
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
    recipes = capture_native_recipes(commands, inputs, root,
                                     Path(env.subst("$BUILD_DIR")).resolve(), toolchain.resolve())
    manifest = {"schema": SCHEMA, "environment": env.subst("$PIOENV"), "project": str(root),
                "argv": inputs.argv, "relocated_argv": relocated, "directories": sorted(inputs.directories),
                "roots": roots,
                "files": sorted(inputs.files.values(), key=lambda item: item["path"]),
                "elf": {"path": str(inputs.output), "sha256": digest(inputs.output)},
                "compiler": {"driver": driver_name(compiler),
                             **compiler_identity(compiler)},
                "toolchain": packages["toolchain-xtensa-esp-elf"], "packages": packages,
                "platform": env.GetProjectOption("platform"),
                "vendor_recipes": recipes}
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
    path = toolchain / "bin" / (driver_name(name) + (".exe" if os.name == "nt" else ""))
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


def rebuild_lgpl(bundle, manifest, sources, work, toolchain):
    native = sources / "lib/NativeText"
    include = work / "include/datrie"
    include.mkdir(parents=True, exist_ok=True)
    for header in (native / "vendor/libdatrie-0.2.14/datrie").glob("*.h"):
        shutil.copyfile(header, include / header.name)
    replacements = {}
    members = []
    for relative, recipe in manifest["vendor_recipes"].items():
        if recipe.get("dependencies", {}).get("method") != "gcc-M-v1":
            raise ValueError("LGPL rebuild requires a kit packaged from a full compiler -M dependency capture")
        source = contained(native, relative)
        if not source.is_file():
            raise FileNotFoundError("Missing modified library source: " + str(source))
        output = work / "lgpl" / (relative + ".o")
        output.parent.mkdir(parents=True, exist_ok=True)
        substitutions = {"{sources}": sources, "{toolchain}": toolchain, "{vendor_include}": include.parent,
                         "{object}": output, "{depfile}": output.with_suffix(".d")}
        arguments = []
        for argument in recipe["argv"]:
            for marker, value in substitutions.items():
                argument = argument.replace(marker, value.as_posix())
            # Other include/header paths point into the original, verified kit.
            argument = argument.replace("inputs/", bundle.as_posix() + "/inputs/")
            arguments.append(argument)
        response = output.with_suffix(".rsp")
        response.write_text(response_text(arguments), encoding="utf-8")
        subprocess.run([str(tool_path(toolchain, recipe["driver"])), "@" + str(response)], cwd=work, check=True)
        replacement = recipe["replacement"]
        if "member" in replacement:
            members.append((replacement["input"] + ":" + replacement["member"], output))
        else:
            replacements[replacement["input"]] = str(output)
    return replacements, members


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
    for directory in manifest["directories"]:
        contained(bundle, directory).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-relink-", dir=output.parent) as temporary:
        work = Path(temporary)
        members = []
        for item in args.replace_member:
            selector, value = item.split("=", 1)
            members.append((selector, Path(value).resolve(strict=True)))
        prefix = driver_name(manifest["compiler"]["driver"]).removesuffix("g++").removesuffix("gcc")
        if args.rebuild_lgpl:
            rebuilt, rebuilt_members = rebuild_lgpl(bundle, manifest, args.rebuild_lgpl.resolve(), work, toolchain)
            if replacements.keys() & rebuilt.keys():
                raise ValueError("Cannot both rebuild and explicitly replace the same LGPL object")
            replacements.update(rebuilt)
            members.extend(rebuilt_members)
        patched = replace_members(bundle, manifest, members, work, tool_path(toolchain, prefix + "gcc-ar"))
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
        response = work / "link.rsp"
        response.write_text(response_text(arguments), encoding="utf-8")
        subprocess.run([str(driver), "@" + str(response)], cwd=bundle, check=True)
    if not output.is_file():
        raise RuntimeError("Linker did not produce the requested ELF")
    print("Relinked " + str(output))


def refresh_capture(manifest_path, toolchain):
    """Refresh full header closure from recorded recipes without compiling/linking."""
    capture = json.loads(manifest_path.read_text(encoding="utf-8"))
    if capture.get("schema") != SCHEMA or capture.get("environment") not in PRO_ENVIRONMENTS:
        raise ValueError("Expected an existing X4 Pro link capture")
    project = Path(capture["project"]).resolve()
    toolchain = toolchain.resolve()
    metadata = json.loads((toolchain / "package.json").read_text(encoding="utf-8"))
    if any(metadata.get(key) != capture["toolchain"][key] for key in ("name", "version")):
        raise ValueError("Toolchain package does not match the captured firmware")
    compiler = tool_path(toolchain, capture["compiler"]["driver"])
    if compiler_identity(compiler) != {key: capture["compiler"][key] for key in ("machine", "version")}:
        raise ValueError("Compiler target/version does not match the captured firmware")
    elf = Path(capture["elf"]["path"])
    if not elf.is_file() or digest(elf) != capture["elf"]["sha256"]:
        raise ValueError("Captured ELF is missing or changed")
    for item in capture["files"]:
        path = Path(item["source"])
        if not path.is_file() or path.stat().st_size != item["size"] or digest(path) != item["sha256"]:
            raise ValueError("Captured input is missing or changed: " + str(path))
    roots = capture.get("roots", {"project": str(project)})
    # Older schema-2 captures did not record roots. Recover package roots from
    # their actual input paths, not the caller's current PlatformIO installation.
    for item in capture["files"]:
        parts = item["path"].split("/")
        if len(parts) > 3 and parts[:2] == ["inputs", "packages"]:
            root = Path(item["source"])
            for _ in parts[3:]:
                root = root.parent
            roots["packages/" + parts[2]] = str(root)
    roots["packages/toolchain-xtensa-esp-elf"] = str(toolchain)
    # An unused -I directory may have had no files in the old partial closure.
    # Resolve such roots by recorded package identity/version among installed
    # siblings rather than assuming a package-directory naming convention.
    required_packages = {name for recipe in capture["vendor_recipes"].values() for argument in recipe["argv"]
                         for name in re.findall(r"inputs/packages/([^/]+)/", argument)}
    missing_packages = {name for name in required_packages if "packages/" + name not in roots}
    matches = {name: [] for name in missing_packages}
    if missing_packages:
        for directory in toolchain.parent.iterdir():
            package_file = directory / "package.json"
            if not package_file.is_file():
                continue
            package = json.loads(package_file.read_text(encoding="utf-8"))
            name = package.get("name")
            if name in matches and package.get("version") == capture["packages"][name]["version"]:
                matches[name].append(directory.resolve())
        for name, directories in matches.items():
            if len(directories) != 1:
                raise ValueError("Cannot uniquely recover original compiler include package: " + name)
            roots["packages/" + name] = str(directories[0])
    inputs = LinkInputs(capture["argv"], project, roots)
    inputs.files = {item["path"]: item for item in capture["files"]}
    inputs.directories = set(capture["directories"])
    build_directory = elf.parent.resolve()
    substitutions = {"{sources}": project, "{toolchain}": toolchain,
                     "{vendor_include}": build_directory / "native-vendor-include",
                     "{object}": build_directory / "native-text-dependencies/unused.o",
                     "{depfile}": build_directory / "native-text-dependencies/unused.d"}
    path_prefixes = sorted(("inputs/" + name + "/", Path(path).resolve().as_posix() + "/")
                           for name, path in roots.items())
    for relative, recipe in capture["vendor_recipes"].items():
        arguments = []
        for argument in recipe["argv"]:
            for marker, path in substitutions.items():
                argument = argument.replace(marker, path.as_posix())
            for marker, path in path_prefixes:
                argument = argument.replace(marker, path)
            if "inputs/" in argument or any(marker in argument for marker in substitutions):
                raise ValueError("Cannot restore recorded compiler path: " + argument)
            arguments.append(argument)
        command = [str(tool_path(toolchain, recipe["driver"])), *arguments]
        recipe["dependencies"] = capture_compile_dependencies(
            command, inputs, toolchain, build_directory / "native-text-dependencies", relative)
    capture["roots"] = roots
    capture["files"] = sorted(inputs.files.values(), key=lambda item: item["path"])
    json_write(manifest_path, capture)
    print("Refreshed compiler dependency closure: " + str(manifest_path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--toolchain", type=Path, required=True, help="Matching PlatformIO toolchain package directory")
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--output", type=Path, help="Output firmware ELF; does not flash")
    operation.add_argument("--refresh-capture", type=Path, metavar="MANIFEST",
                           help="Refresh an existing local capture with full compiler -M header closure; no firmware build")
    parser.add_argument("--replace", action="append", default=[], metavar="INPUT=FILE",
                        help="Replace one linked archive/object (relative manifest path or unique basename)")
    parser.add_argument("--replace-member", action="append", default=[], metavar="ARCHIVE:MEMBER=OBJECT",
                        help="Replace one archive member while retaining all other firmware objects")
    parser.add_argument("--rebuild-lgpl", type=Path, metavar="SOURCE_ROOT",
                        help="Compile modified LibThai/libdatrie sources and replace their actual linked inputs")
    args = parser.parse_args()
    if args.refresh_capture:
        if args.replace or args.replace_member or args.rebuild_lgpl:
            parser.error("--refresh-capture cannot be combined with replacement options")
        refresh_capture(args.refresh_capture, args.toolchain)
    else:
        relink(args)


def register_scons(env):
    if env.subst("$PIOENV") not in PRO_ENVIRONMENTS:
        raise RuntimeError("Native relink hook must only be attached to X4 Pro environments")
    # SCons drops completed object executors before the ELF action. Capture the
    # private environments now, after PlatformIO constructed every library node.
    elf = env.File(env.subst("$BUILD_DIR/${PROGNAME}.elf"))
    link_env = elf.get_build_env()
    link_env.Replace(LINKFLAGS=[flag for flag in link_env.get("LINKFLAGS", []) if str(flag) != "-fno-lto"] + ["-flto"])
    commands = collect_native_commands(elf, Path(env.subst("$PROJECT_DIR")).resolve())

    def capture(target, source, env):
        capture_link(env, target, source, commands)

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", capture)

    def package(target, source, env):
        # An up-to-date ELF does not run post-actions, so refresh the manifest
        # explicitly for the package target using the ELF's original executor.
        elf = env.File(env.subst("$BUILD_DIR/${PROGNAME}.elf"))
        capture_link(env, [elf], [], commands)
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
