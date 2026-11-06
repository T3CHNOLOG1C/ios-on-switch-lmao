#!/usr/bin/env python3
"""Show what the caches do, with Bazel's own counters ("INFO: N processes: ... disk cache hit ...").

    bazel run //tools/bench:cache                 # no-op, one driver changed, reverted, fresh output base
    bazel run //tools/bench:cache -- --cold       # also a clean build with no cache at all (10+ minutes)

Scenarios (each prints Bazel's Elapsed/processes lines):
  no-op            build //:boot //:gpu again: nothing runs
  comment added    a comment appended to drivers/SwitchButtons/SwitchButtons.cpp: that object recompiles, comes out
                   byte-identical, and nothing downstream runs (Bazel keys on content, not timestamps)
  code changed     a function added to the same file: that object, the archive, the link, the prelink and the package
                   rerun; nothing else
  code reverted    the function removed: every action of the original build is a disk-cache hit
  fresh base       a brand-new output base (a fresh clone or `bazel clean --expunge`): everything from the disk cache
  cold (--cold)    a brand-new output base with no disk cache: everything runs
The edited source is always restored.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

TARGETS = ['//:boot', '//:gpu']
DRIVER = 'drivers/SwitchButtons/SwitchButtons.cpp'


def build(workspace: Path, label: str, startup: Sequence[str] = (), flags: Sequence[str] = ()) -> None:
    bazel = os.environ.get('BAZEL', 'bazel')
    result = subprocess.run(
        [bazel, *startup, 'build', *flags, *TARGETS], cwd=workspace, capture_output=True, text=True, check=False
    )
    lines = [line for line in result.stderr.splitlines() if line.startswith('INFO: Elapsed') or ' processes' in line]
    print('{:16} {}'.format(label, ' / '.join(line.replace('INFO: ', '') for line in lines)), flush=True)
    if result.returncode:
        sys.exit(result.stderr[-3000:])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cold', action='store_true')
    args = parser.parse_args()
    workspace = Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY') or sys.exit('run with bazel run'))
    build(workspace, 'warm-up')
    build(workspace, 'no-op')
    source = workspace / DRIVER
    original = source.read_bytes()
    try:
        source.write_bytes(original + b'// cache demo\n')
        build(workspace, 'comment added')
        source.write_bytes(
            original + b'extern "C" __attribute__((used)) int switch_cache_demo(void) { return 1104; }\n'
        )
        build(workspace, 'code changed')
    finally:
        source.write_bytes(original)
    build(workspace, 'code reverted')
    scratch = Path(tempfile.mkdtemp(prefix='switch-bench-'))
    try:
        quiet = ['--experimental_convenience_symlinks=ignore']
        build(workspace, 'fresh base', ['--output_base=' + str(scratch / 'warm')], quiet)
        if args.cold:
            build(workspace, 'cold', ['--output_base=' + str(scratch / 'cold')], [*quiet, '--disk_cache='])
    finally:
        for base in ('warm', 'cold'):
            if (scratch / base).exists():
                subprocess.run(
                    [os.environ.get('BAZEL', 'bazel'), '--output_base=' + str(scratch / base), 'clean', '--expunge'],
                    cwd=workspace,
                    capture_output=True,
                    check=False,
                )
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == '__main__':
    main()
