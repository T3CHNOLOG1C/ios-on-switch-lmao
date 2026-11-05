"""The kernel toolchain's compile recipes (bazel/toolchains/kernel/recipes.bzl), from XNU's own per-object compile
records: what cmake/XnuRecipes.cmake's ioswitch_recipe took from them, as Starlark the kernel cc_toolchain_config
turns into features (one per recipe).

A recipe is the record's arguments without its compiler, source, -o and dependency outputs, without its -W flags
(unless keep_warnings), without arguments starting with a drop prefix; `-I .` and `-include <file in the record
directory>` made absolute. With strip_includes, -I/-include/-isystem and their values go too (the freestanding recipe
the vendored Linux code uses). The record's -ffile-prefix-map flags go (the toolchain maps paths itself); paths become
placeholders the toolchain fills in: {xnu_obj} (XNU's object root, as the headers-only tree the drivers see), {fakeroot}
(the kernel SDK root), {xnu} (the patched XNU source) and {tree} (the kernel tree). Flags only Apple clang takes, or only
code generation needs, are listed apart (`apple`): the toolchain puts them in the apple_clang_only feature, which
clang-tidy turns off.

usage: recipes.py --objroot DIR --out recipes.bzl [--check] --recipe NAME=RECORD[:keep_warnings][:strip_includes][:drop=P]...
  --check: compare with --out instead of writing it (the recipes test).
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

EXECROOT = "@EXECROOT@"
# Paths below the execution root, as XNU's make wrote them (the build's own configuration directory in the middle).
# The kernel's generated files' package before and after the layout move (bazel/kernel/layout.bzl).
_PACKAGE = r"(?:out/native-a57/root-v1/tree|kernel)"
PLACES = [
    (re.compile(r"@EXECROOT@/bazel-out/[^/]+/bin/" + _PACKAGE + r"/build/xnu/ios-obj"), "{xnu_obj}"),
    (re.compile(r"@EXECROOT@/bazel-out/[^/]+/bin/" + _PACKAGE + r"/\.cache/fakeroot"), "{fakeroot}"),
    (re.compile(r"@EXECROOT@/external/\+http_archive\+xnu"), "{xnu}"),
    (re.compile(r"@EXECROOT@/out/native-a57/root-v1/tree"), "{tree}"),
    # Since the move, the kernel tree's sources are at the repository root: {tree}/x is x.
    (re.compile(r"@EXECROOT@/(?=(?:drivers|kits|diagnostics|overlay)/)"), "{tree}/"),
]
# Apple clang only, or code generation only (r17-lint's list): clang-tidy (upstream clang) runs without them.
APPLE_PREFIXES = ("-fsanitize", "-flto", "-mcpu=", "-march=", "-fptrauth", "-ftrivial-auto-var-init", "-fbounds-safety",
                  "-fexperimental-bounds-safety", "-fc++-static-destructors=", "-mlinker-version=")
APPLE_PAIRS = ("-mllvm", "-Xclang", "-index-store-path", "-arch")


def recipe(objroot, record_name, keep_warnings=False, drop=(), strip_includes=False):
    """(compiler, directory, arguments) of one compile record, as cmake/XnuRecipes.cmake's ioswitch_recipe."""
    record_path = objroot / "DEVELOPMENT_ARM64_BCM2837" / record_name
    record = json.loads(re.sub(r",\s*$", "", record_path.read_text()))
    directory, source, arguments = record["directory"], record["file"], record["arguments"]
    local = objroot / "DEVELOPMENT_ARM64_BCM2837" / Path(record_name).parent
    out, skip, previous = [], False, ""
    for i, value in enumerate(arguments[1:], start=1):
        if skip:
            skip, previous = False, ""
            continue
        if value == source or value in ("-MD", "-MP"):
            continue
        if value in ("-o", "-MF", "-MJ"):
            skip = True
            continue
        if not keep_warnings:
            if value.startswith("-W"):
                continue
            if value == "-Xclang" and i + 1 < len(arguments) and arguments[i + 1].startswith("-W"):
                skip = True
                continue
        if any(value.startswith(prefix) for prefix in drop):
            continue
        if previous == "-I" and value == ".":
            value = directory
        elif previous == "-include" and not value.startswith("/") and not value.startswith("@") and (local / value).exists():
            value = directory + "/" + value
        previous = value
        out.append(value)
    if strip_includes:
        stripped, skip = [], False
        for value in out:
            if skip:
                skip = False
            elif value in ("-I", "-include", "-isystem"):
                skip = True
            else:
                stripped.append(value)
        out = stripped
    return arguments[0], directory, out


def place(value):
    for pattern, name in PLACES:
        value = pattern.sub(name, value)
    if EXECROOT in value:
        sys.exit("recipe argument names an unknown execution-root path: " + value)
    return value


def split(arguments):
    """(portable, apple) argument lists, each in the record's order; the record's prefix maps dropped."""
    portable, apple, i = [], [], 0
    while i < len(arguments):
        value = arguments[i]
        if value.startswith("-ffile-prefix-map=") or value.startswith("-fdebug-prefix-map=") or \
                value.startswith("-fmacro-prefix-map="):
            i += 1
            continue
        if value in APPLE_PAIRS and i + 1 < len(arguments):
            apple += [value, place(arguments[i + 1])]
            i += 2
            continue
        (apple if value.startswith(APPLE_PREFIXES) else portable).append(place(value))
        i += 1
    return portable, apple


def debug_dir(directory):
    """The compilation directory the record's objects carry: its directory as the record's last (catch-all) prefix map
    rewrote it (the execution root becomes /ioswitch)."""
    if not directory.startswith(EXECROOT + "/"):
        sys.exit("record directory outside the execution root: " + directory)
    relative = re.sub(r"^(bazel-out/[^/]+/bin)/kernel/", r"\1/out/native-a57/root-v1/tree/", directory[len(EXECROOT) + 1:])
    return "/ioswitch/" + relative


def render(recipes):
    lines = [
        '"""Generated by tools/kernel/recipes.py (`bazel run //bazel/toolchains/kernel:update_recipes`); checked by',
        "//bazel/toolchains/kernel:recipes_test. Do not edit.",
        "",
        "The compile recipes of code linked into the kernel, from XNU's compile records: the kernel toolchain's features",
        '(bazel/toolchains/kernel/config.bzl). Placeholders: {xnu_obj}, {fakeroot}, {xnu}, {tree}."""',
        "",
        "RECIPES = {",
    ]
    for name, (compiler, directory, arguments) in recipes.items():
        portable, apple = split(arguments)
        lines.append("    {}: struct(".format(json.dumps(name)))
        lines.append("        compiler = {},".format(json.dumps(Path(compiler).name)))
        lines.append("        debug_dir = {},".format(json.dumps(debug_dir(directory))))
        for key, values in (("flags", portable), ("apple", apple)):
            lines.append("        {} = [".format(key))
            lines += ["            {},".format(json.dumps(v)) for v in values]
            lines.append("        ],")
        lines.append("    ),")
    lines.append("}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--objroot", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--recipe", action="append", required=True)
    args = parser.parse_args()
    objroot = Path(args.objroot)
    recipes = {}
    for spec in args.recipe:
        name, _, rest = spec.partition("=")
        record, *options = rest.split(":")
        drop = [o[len("drop="):] for o in options if o.startswith("drop=")]
        recipes[name] = recipe(objroot, record, "keep_warnings" in options, drop, "strip_includes" in options)
    text = render(recipes)
    out = Path(args.out)
    if not args.check and not out.is_absolute() and os.environ.get("BUILD_WORKSPACE_DIRECTORY"):
        out = Path(os.environ["BUILD_WORKSPACE_DIRECTORY"]) / out
    if args.check:
        if out.read_text() != text:
            sys.exit("{} is out of date with XNU's compile records: bazel run //bazel/toolchains/kernel:update_recipes"
                     .format(args.out))
        return
    out.write_text(text)


if __name__ == "__main__":
    main()
