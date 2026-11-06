#!/usr/bin/env python3
"""Cache check: action keys do not depend on where the checkout or the output base is. A second worktree of this
commit, built in a fresh output base against the shared caches, must execute nothing: every action is a disk-cache hit
of this checkout's build.

    bazel run //tools/repro:cache_check                    # //:boot //:gpu
    bazel run //tools/repro:cache_check -- //:kernel       # just these
    bazel run //tools/repro:cache_check -- --keep          # keep the second worktree and output base

Build the targets in this checkout first (that fills the disk cache). The check adds this commit as a temporary git
worktree, builds the same targets there in a fresh output base with an execution log, and lists every spawn that was
not a cache hit (mnemonic, first output). An absolute workspace or output-base path in a command line, an environment
variable or a tool's output shows up as an executed action. The working tree must be clean: the second worktree builds
the commit.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterator
from pathlib import Path


def spawns(log: Path) -> Iterator[dict]:
    """The spawns of a --execution_log_json_file (a stream of JSON objects)."""
    text = Path(log).read_text()
    decoder, at = json.JSONDecoder(), 0
    while True:
        while at < len(text) and text[at].isspace():
            at += 1
        if at >= len(text):
            return
        entry, at = decoder.raw_decode(text, at)
        yield entry


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('targets', nargs='*', default=['//:boot', '//:gpu'])
    parser.add_argument('--keep', action='store_true')
    args = parser.parse_args()
    workspace = os.environ.get('BUILD_WORKSPACE_DIRECTORY') or sys.exit('run with bazel run')
    bazel = os.environ.get('BAZEL', 'bazel')
    dirty = subprocess.run(
        ['git', '-C', workspace, 'status', '--porcelain', '--untracked-files=no'],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    if dirty.strip():
        sys.exit('commit or stash first (the second worktree builds the commit):\n' + dirty)
    scratch = Path(tempfile.mkdtemp(prefix='switch-cache-check-'))
    tree, base, log = scratch / 'worktree', scratch / 'output-base', scratch / 'exec.json'
    subprocess.run(['git', '-C', workspace, 'worktree', 'add', '--detach', str(tree), 'HEAD'], check=True)
    try:
        build = subprocess.run(
            [bazel, '--output_base=' + str(base), 'build', '--execution_log_json_file=' + str(log), *args.targets],
            cwd=tree,
            capture_output=True,
            text=True,
        )
        for line in build.stderr.splitlines():
            if line.startswith('INFO: Elapsed') or ' processes' in line:
                print('second worktree:', line)
        if build.returncode:
            sys.exit(build.stderr[-4000:])
        executed = [
            s
            for s in spawns(log)
            if s.get('runner') not in ('disk cache hit', 'remote cache hit') and not s.get('remoteCacheHit')
        ]
        print(f'{len(executed)} spawns executed in the second worktree')
        for s in executed[:40]:
            outputs = [o.get('path') for o in s.get('actualOutputs', []) or s.get('listedOutputs', [])]
            print('  {} {}'.format(s.get('mnemonic'), outputs[0] if outputs else ''))
        if executed:
            sys.exit('action keys differ between worktrees (see above)')
    finally:
        subprocess.run([bazel, '--output_base=' + str(base), 'shutdown'], cwd=tree, capture_output=True)
        if not args.keep:
            subprocess.run(['git', '-C', workspace, 'worktree', 'remove', '--force', str(tree)], capture_output=True)
            subprocess.run(['chmod', '-R', 'u+w', str(scratch)], capture_output=True)
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == '__main__':
    main()
