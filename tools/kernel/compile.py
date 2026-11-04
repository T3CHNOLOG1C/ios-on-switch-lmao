"""Compile one source file for the kernel: with a recipe (recipes.py) as cmake/XnuRecipes.cmake's ioswitch_compile did
(<cc> <recipe args> <flags> -o <object> <source>, run in the record's directory), or with a plain command
(ioswitch_compile_command: <command> <flags> -c <source> -o <object>).

Flags may name {tree} (the kernel tree), {build} (this package's output directory, the CMake flow's tree/build),
{xnu} (the patched XNU source) and {sdk} (the iPhoneOS SDK); compile.py makes them absolute. Every path is
absolute, as in the CMake flow, and -ffile-prefix-map turns the execution root back into the configured path root.

usage: compile.py --xcode-env F --tree DIR --build DIR --xnu DIR --path-root P --bin-dir D --source S --output O
                  (--recipe R.json | --command ARG...) [--flag F]...
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    for name in ("xcode-env", "tree", "build", "xnu", "path-root", "bin-dir", "source", "output"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--recipe")
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--flag", action="append", default=[])
    parser.add_argument("--dry-run", action="store_true", help="print the compile as a compile_commands.json entry")
    args = parser.parse_args()
    here = common.execroot()
    xcode = common.xcode_env(args.xcode_env)
    places = {
        "{tree}": os.path.join(here, args.tree),
        "{build}": os.path.join(here, args.build),
        "{xnu}": os.path.join(here, args.xnu),
        "{sdk}": xcode["IPHONEOS_SDK"],
    }

    def expand(value):
        value = value.replace(common.EXECROOT, here)
        for key, path in places.items():
            value = value.replace(key, path)
        return value

    source = os.path.join(here, args.source)
    output = os.path.join(here, args.output)
    compiled = output
    flags = [expand(f) for f in args.flag]
    env = common.environment(xcode)
    if args.recipe:
        recipe = json.loads(Path(args.recipe).read_text())
        command = [expand(recipe["cc"])] + [expand(a) for a in recipe["args"]] + flags + ["-o", compiled, source]
        cwd = expand(recipe["directory"])
    else:
        # Plain commands (bindings, firmware .incbin, link stubs) get the same path hygiene as the recipes.
        command = [expand(a) for a in args.command] + flags + common.prefix_maps(args.path_root, args.bin_dir, args.xnu)
        command += ["-c", source, "-o", compiled]
        cwd = here
    if args.dry_run:
        print(json.dumps({"directory": cwd, "file": source, "output": compiled, "arguments": command}))
        return
    with tempfile.TemporaryDirectory() as scratch:
        # Files from XNU's object root that name @EXECROOT@ in their contents (the sanitizer ignore lists).
        for i, value in enumerate(command):
            if value.startswith("-fsanitize-ignorelist=") or value.startswith("-fsanitize-blacklist="):
                flag, _, path = value.partition("=")
                text = Path(path).read_text()
                if common.EXECROOT in text:
                    resolved = os.path.join(scratch, "{}-{}".format(i, os.path.basename(path)))
                    Path(resolved).write_text(text.replace(common.EXECROOT, here))
                    command[i] = flag + "=" + resolved
        result = subprocess.run(command, cwd=cwd, env=env)
    if result.returncode:
        sys.exit(result.returncode)
    data = Path(compiled).read_bytes()
    if here.encode() in data:
        at = data.index(here.encode())
        sys.exit("{} names the execution root (a path escaped the prefix maps): {!r}".format(
            args.output, data[max(0, at - 120):at + 200]))


if __name__ == "__main__":
    main()
