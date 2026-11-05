"""hdr_deps.py [--write]: each kernel_driver's hdr_deps, from what its sources include.

    bazel run //tools/kernel:hdr_deps             report the drivers whose hdr_deps differ from their includes (exit 1)
    bazel run //tools/kernel:hdr_deps -- --write  rewrite those hdr_deps in place

A driver's compiles see only its own package's headers and its hdr_deps' (kernel/rules/kernel.bzl), so hdr_deps must
name every other package whose headers its files reach. Starting from the driver's sources (srcs, or its package's
*.cpp and firmware.S), each `#include "..."` is resolved as the compiler would (the including file's directory, then
the driver's -I {tree}/<dir> flags) and followed into the headers it names, so a header that includes another
package's header counts too. A package counts when its BUILD.bazel declares its headers (kernel_headers()); includes
of XNU, the SDK and generated headers ({xnu}, {build}) are not packages and need no hdr_deps, nor does
diagnostics/switch, whose headers every kernel compile has (the kernel SDK's extra headers).
"""

import argparse
import ast
import itertools
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOTS = ('drivers', 'kits', 'diagnostics')
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
SOURCES = ('.c', '.cpp', '.h', '.hpp', '.inc', '.S', '.s', '.m', '.mm')
# Header packages every kernel compile already has (kernel/BUILD.bazel's kernel_sdk extra: the recipes' -I
# {tree}/diagnostics/switch): followed, never needed in hdr_deps.
SDK_PACKAGES = ('diagnostics/switch',)


@dataclass
class Driver:
    """One kernel_driver call: where it is and what it declares."""

    build: Path
    package: str
    name: str
    call: ast.Call
    srcs: list[str] | None
    includes: list[str]
    hdr_deps: list[str] | None
    hdr_deps_node: ast.expr | None = None
    needed: set[str] = field(default_factory=set)


def strings(node: ast.AST) -> list[str]:
    """Return every string constant in an expression, in order (lists, sums, constants' values unknown: skipped)."""
    return [n.value for n in ast.walk(node) if isinstance(n, ast.Constant) and isinstance(n.value, str)]


def include_dirs(values: list[str]) -> list[str]:
    """Return the repository directories of the -I {tree}/<dir> flags among a driver's flags."""
    dirs = []
    for flag, value in itertools.pairwise(values):
        if flag == '-I' and value.startswith('{tree}/'):
            dirs.append(value[len('{tree}/') :])
    return dirs


def drivers_in(root: Path, build: Path) -> list[Driver]:
    """Return the kernel_driver calls of one BUILD.bazel."""
    package = build.parent.relative_to(root).as_posix()
    found = []
    for node in ast.walk(ast.parse(build.read_text(), str(build))):
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == 'kernel_driver'):
            continue
        kw = {k.arg: k.value for k in node.keywords if k.arg}
        if 'name' not in kw or not isinstance(kw['name'], ast.Constant):
            continue
        flags = []
        for key in ('copts', 'c_copts', 'asm_copts', 'src_copts'):
            if key in kw:
                flags += strings(kw[key])
        srcs = strings(kw['srcs']) if 'srcs' in kw else None
        hdr_deps = strings(kw['hdr_deps']) if 'hdr_deps' in kw else None
        found.append(
            Driver(build, package, kw['name'].value, node, srcs, include_dirs(flags), hdr_deps, kw.get('hdr_deps'))
        )
    return found


def package_of(root: Path, path: Path, packages: set[str]) -> str | None:
    """Return the header package (drivers/<X>, kits/<X>, diagnostics/switch) a repository file belongs to."""
    try:
        rel = path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return None
    best = None
    for package in packages:
        if (rel.startswith(package + '/')) and (best is None or len(package) > len(best)):
            best = package
    return best


def start_files(root: Path, driver: Driver) -> list[Path]:
    """Return the files a driver compiles: its srcs, or its package's .cpp files and firmware.S."""
    base = root / driver.package
    if driver.srcs is None:
        files = sorted(base.glob('*.cpp'))
        return files + ([base / 'firmware.S'] if (base / 'firmware.S').is_file() else [])
    files = []
    for src in driver.srcs:
        if src.startswith('//') and ':' in src:
            package, _, name = src[2:].partition(':')
            files.append(root / package / name)
        elif src.startswith('{tree}/'):
            files.append(root / src[len('{tree}/') :])
        elif not src.startswith((':', '@', '/')):
            files.append(base / src)
    return [f for f in files if f.is_file()]


def needed_packages(root: Path, driver: Driver, packages: set[str]) -> set[str]:
    """Return the other header packages a driver's files reach through their includes."""
    todo, seen, needed = start_files(root, driver), set(), set()
    dirs = [root / d for d in driver.includes] + [root / p for p in SDK_PACKAGES]
    while todo:
        path = todo.pop()
        if path in seen or path.suffix not in SOURCES:
            continue
        seen.add(path)
        for include in INCLUDE.findall(path.read_text(errors='replace')):
            for candidate in [path.parent / include] + [d / include for d in dirs]:
                if candidate.is_file():
                    other = package_of(root, candidate, packages)
                    if other and other != driver.package and other not in SDK_PACKAGES:
                        needed.add(other)
                    todo.append(candidate.resolve())
                    break
    return needed


def rendered(deps: list[str], indent: str) -> str:
    """Return a hdr_deps list as buildifier formats it (one entry inline, more one per line)."""
    if len(deps) <= 1:
        return '[' + ''.join(f'"{d}"' for d in deps) + ']'
    return '[\n' + ''.join(f'{indent}    "{d}",\n' for d in deps) + indent + ']'


def rewrite(build: Path, drivers: list[Driver]) -> None:
    """Rewrite the hdr_deps of the given drivers of one BUILD.bazel (bottom-up, so offsets stay valid)."""
    lines = build.read_text().splitlines(keepends=True)
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line))
    text = ''.join(lines)

    def at(lineno: int, col: int) -> int:
        return offsets[lineno - 1] + col

    edits = []
    for driver in drivers:
        deps = sorted(driver.needed)
        call = driver.call
        if driver.hdr_deps_node is not None:
            node = driver.hdr_deps_node
            indent = ' ' * (node.col_offset - len('hdr_deps = '))
            edits.append(
                (at(node.lineno, node.col_offset), at(node.end_lineno, node.end_col_offset), rendered(deps, indent))
            )
            continue
        name = next(k for k in call.keywords if k.arg == 'name')
        if name.value.end_lineno == call.end_lineno:  # kernel_driver(name = "X") on one line
            start, end = at(call.lineno, call.col_offset), at(call.end_lineno, call.end_col_offset)
            new = f'kernel_driver(\n    name = "{driver.name}",\n    hdr_deps = {rendered(deps, "    ")},\n)'
            edits.append((start, end, new))
        else:
            end = at(name.value.end_lineno, name.value.end_col_offset)
            comma = text.index(',', end) + 1
            edits.append((comma, comma, f'\n    hdr_deps = {rendered(deps, "    ")},'))
    for start, end, new in sorted(edits, reverse=True):
        text = text[:start] + new + text[end:]
    build.write_text(text)


def main() -> None:
    """Report, or with --write fix, every kernel_driver's hdr_deps."""
    parser = argparse.ArgumentParser()
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--buildifier', help='the pinned buildifier: formats the files --write rewrites')
    args = parser.parse_args()
    root = Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY', '.'))
    builds = [*sorted(b for top in ROOTS for b in (root / top).rglob('BUILD.bazel')), root / 'kernel/BUILD.bazel']
    packages = {b.parent.relative_to(root).as_posix() for b in builds if 'kernel_headers()' in b.read_text()}
    stale: dict[Path, list[Driver]] = {}
    for build in builds:
        for driver in drivers_in(root, build):
            driver.needed = needed_packages(root, driver, packages)
            declared = set(driver.hdr_deps or [])
            if driver.hdr_deps is None or declared != driver.needed:
                missing, extra = sorted(driver.needed - declared), sorted(declared - driver.needed)
                print(f'//{driver.package}:{driver.name}: missing {missing}, not included {extra}')
                stale.setdefault(build, []).append(driver)
    if not stale:
        print(f"hdr_deps: every kernel_driver's matches its includes ({len(builds)} BUILD files)")
        return
    if not args.write:
        sys.exit('hdr_deps out of date: bazel run //tools/kernel:hdr_deps -- --write')
    for build, drivers in stale.items():
        rewrite(build, drivers)
    if args.buildifier:
        # Argument order as buildifier keeps it, list order untouched (a driver's srcs order is its link order).
        files = [str(b) for b in stale]
        subprocess.run(
            [str(Path(args.buildifier).resolve()), '--lint=off', '-buildifier_disable=listsort', *files], check=True
        )
    print(f'hdr_deps: rewrote {sum(len(d) for d in stale.values())} drivers in {len(stale)} BUILD files')


if __name__ == '__main__':
    main()
