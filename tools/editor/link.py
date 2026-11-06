"""Put the formatters `bazel run //:format` runs in .cache/editor-bin/, for the editor's format on save.

    bazel run //tools/editor:link

.vscode/settings.json points each language's formatter there. Most entries are links to the tools in the output base's
external/; dprint, shfmt and rustfmt are wrappers that add the arguments //:format passes, and shfmt and rustfmt also
leave the vendored trees alone, because their editor extensions have no exclude setting of their own.
"""

import argparse
import json
import os
import shlex
from pathlib import Path

from python.runfiles import runfiles

VENDORED = ('.agents', 'kernel/xnu-*', 'third_party', 'translate/arm64e/xnu', 'vendor')
OURS_IN_VENDORED = ('third_party/mesa/bazel',)


def parse_args() -> argparse.Namespace:
    """Return the tools' runfiles paths from the command line (BUILD.bazel's args)."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for tool in ('clang-format', 'ruff', 'buildifier', 'shfmt', 'rustfmt', 'rumdl', 'dprint'):
        parser.add_argument(f'--{tool}', required=True)
    parser.add_argument('--shfmt-flag', action='append', default=[])
    parser.add_argument('--dprint-plugin', action='append', default=[])
    return parser.parse_args()


def external(rlocation: str) -> str:
    """Return the output base's external/ path of a runfile.

    A runfile links to <output base>/external/<repo>/..., which in turn links into the repository contents cache. The
    contents cache entry can be collected and fetched again under a new name; the external/ path stays the same.
    """
    path = runfiles.Create().Rlocation(rlocation) or rlocation
    return os.readlink(path) if os.path.islink(path) else os.path.abspath(path)


def write(path: Path, text: str, *, executable: bool = False) -> None:
    """Write a file in one rename, so an editor never runs or reads it half written."""
    temporary = path.with_name(f'.{path.name}.new')
    temporary.write_text(text)
    temporary.chmod(0o755 if executable else 0o644)
    temporary.replace(path)


def link(path: Path, target: str) -> None:
    """Point the symlink at path to target, in one rename."""
    temporary = path.with_name(f'.{path.name}.new')
    temporary.unlink(missing_ok=True)
    temporary.symlink_to(target)
    temporary.replace(path)


def skip_vendored(subject: str, root: str) -> str:
    """Return a shell `case` that passes stdin through unchanged when subject is a path in a vendored tree."""
    quoted = shlex.quote(root)
    ours = ' | '.join(f'{quoted}/{tree}/*' for tree in OURS_IN_VENDORED)
    vendored = ' | '.join(f'{quoted}/{tree}/*' for tree in VENDORED)
    return f'case {subject} in\n    {ours}) ;;\n    {vendored}) exec cat ;;\nesac'


def script(*lines: str) -> str:
    """Return a POSIX shell script of these lines."""
    return '\n'.join(('#!/bin/sh', *lines)) + '\n'


def dprint_wrapper(dprint: str, plugins: list[str]) -> str:
    """Return dprint with //:format's plugins (dprint.json names none) for every subcommand."""
    tool = shlex.quote(dprint)
    return script(
        'case "$1" in',
        f'    -* | \'\') exec {tool} "$@" ;;',
        'esac',
        f'exec {tool} "$@" --plugins {shlex.join(plugins)}',
    )


def shfmt_wrapper(shfmt: str, flags: list[str], root: str) -> str:
    """Return shfmt with //:format's style, for the .sh files //:format formats (the extension passes --filename)."""
    return script(
        'for arg; do',
        '    case "$arg" in',
        '        --filename=*.sh) file="${arg#--filename=}" ;;',
        '        --filename=*) exec cat ;;',
        '    esac',
        'done',
        skip_vendored('"$file"', root),
        f'exec {shlex.quote(shfmt)} {shlex.join(flags)} "$@"',
    )


def rustfmt_wrapper(rustfmt: str, root: str) -> str:
    """Return rustfmt with the repository's rustfmt.toml, as //:format runs it.

    rust-analyzer gives rustfmt the file on stdin from the file's directory, so the directory is all there is to go on,
    and without --config-path rustfmt would pick up the nearest rustfmt.toml (Mesa's, for third_party/mesa/bazel/).
    """
    config = shlex.quote(str(Path(root) / 'rustfmt.toml'))
    return script(
        skip_vendored('"$(pwd -P)/"', root),
        f'exec {shlex.quote(rustfmt)} --config-path {config} "$@"',
    )


def buildifier_config(root: Path) -> str:
    """Return .buildifier.json with lint off, as //:format runs buildifier.

    The editor formats with this config and no -lint flag; with the repository's lint=warn, buildifier exits 4 on any
    warning and the Bazel extension then drops the formatted text. Its diagnostics pass --lint=warn themselves.
    """
    config = json.loads((root / '.buildifier.json').read_text())
    config['lint'] = 'off'
    return json.dumps(config, indent=2) + '\n'


def main() -> None:
    """Create or refresh every entry of .cache/editor-bin/."""
    args = parse_args()
    root = Path(os.environ['BUILD_WORKSPACE_DIRECTORY']).resolve()
    out = root / '.cache' / 'editor-bin'
    out.mkdir(parents=True, exist_ok=True)
    for name in ('clang-format', 'ruff', 'buildifier', 'rumdl'):
        link(out / name, external(getattr(args, name.replace('-', '_'))))
    write(out / 'buildifier.json', buildifier_config(root))
    plugins = [external(p) for p in args.dprint_plugin]
    write(out / 'dprint', dprint_wrapper(external(args.dprint), plugins), executable=True)
    write(out / 'shfmt', shfmt_wrapper(external(args.shfmt), args.shfmt_flag, str(root)), executable=True)
    write(out / 'rustfmt', rustfmt_wrapper(external(args.rustfmt), str(root)), executable=True)
    print(f'//tools/editor:link: {out}')


if __name__ == '__main__':
    main()
