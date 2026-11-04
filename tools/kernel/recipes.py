"""Compile recipes for code linked into the kernel, from XNU's own per-object compile records (cmake/XnuRecipes.cmake's
ioswitch_recipe, which the tree's scripts/build-*.py builders also followed).

A recipe is the record's arguments without its compiler, source, -o and dependency outputs, without its -W flags
(unless --keep-warnings), without arguments starting with a --drop prefix; `-I .` and `-include <file in the record
directory>` made absolute. With --strip-includes, -I/-include/-isystem and their values go too (the freestanding
recipe the vendored Linux code uses). Paths keep the build's @EXECROOT@ placeholder; compile.py fills it in.

usage: recipes.py --objroot DIR --record PATH --out RECIPE.json [--keep-warnings] [--drop PREFIX]... [--strip-includes]
"""
import argparse
import json
import re
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--objroot", required=True)
    parser.add_argument("--record", required=True, help="below the configuration's object directory")
    parser.add_argument("--out", required=True)
    parser.add_argument("--keep-warnings", action="store_true")
    parser.add_argument("--drop", action="append", default=[])
    parser.add_argument("--strip-includes", action="store_true")
    args = parser.parse_args()
    objroot = Path(args.objroot)
    record_path = objroot / "DEVELOPMENT_ARM64_BCM2837" / args.record
    record = json.loads(re.sub(r",\s*$", "", record_path.read_text()))
    directory, source, arguments = record["directory"], record["file"], record["arguments"]
    local = objroot / "DEVELOPMENT_ARM64_BCM2837" / Path(args.record).parent
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
        if not args.keep_warnings:
            if value.startswith("-W"):
                continue
            if value == "-Xclang" and i + 1 < len(arguments) and arguments[i + 1].startswith("-W"):
                skip = True
                continue
        if any(value.startswith(prefix) for prefix in args.drop):
            continue
        if previous == "-I" and value == ".":
            value = directory
        elif previous == "-include" and not value.startswith("/") and not value.startswith("@") and (local / value).exists():
            value = directory + "/" + value
        previous = value
        out.append(value)
    if args.strip_includes:
        stripped, skip = [], False
        for value in out:
            if skip:
                skip = False
            elif value in ("-I", "-include", "-isystem"):
                skip = True
            else:
                stripped.append(value)
        out = stripped
    Path(args.out).write_text(json.dumps({"cc": arguments[0], "directory": directory, "args": out}, indent=1) + "\n")


if __name__ == "__main__":
    main()
