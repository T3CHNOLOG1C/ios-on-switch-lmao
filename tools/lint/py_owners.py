"""Fail on any tracked .py file that no py_binary, py_test or py_library owns: no script runs outside Bazel.

    bazel run //tools/lint:py_owners

Not a test: it asks `bazel query` which targets own each file, and a test cannot run Bazel. Vendored code
(third_party/, XNU at kernel/xnu-<version>/, the agent skills in .agents/, which .bazelignore keeps from Bazel) is not
ours to own: only its scripts the build runs have targets (Mesa's generators), the rest never run.
"""

import os
import subprocess
import sys
from pathlib import Path

VENDORED = ('third_party/', 'kernel/xnu-', '.agents/')

OWNED = (
    'kind("source file", labels(srcs, kind("py_(binary|test|library)", //...)) + '
    'labels(main, kind("py_(binary|test)", //...)))'
)


def owned_paths(root: Path, bazel: str) -> set[str]:
    """Return the workspace-relative paths of the .py files a py_* target names in srcs or main."""
    labels = subprocess.run(
        [bazel, 'query', OWNED, '--output=label'], cwd=root, check=True, capture_output=True, text=True
    ).stdout.split()
    paths = set()
    for label in labels:
        if label.startswith('//'):
            package, _, name = label[2:].partition(':')
            paths.add(f'{package}/{name}' if package else name)
    return paths


def main() -> int:
    """Print every unowned tracked .py file; return 1 if there is one."""
    root = Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY') or Path.cwd())
    tracked = [
        path
        for path in subprocess.run(
            ['git', 'ls-files', '*.py'], cwd=root, check=True, capture_output=True, text=True
        ).stdout.split()
        if not path.startswith(VENDORED)
    ]
    unowned = sorted(set(tracked) - owned_paths(root, os.environ.get('BAZEL', 'bazel')))
    for path in unowned:
        print(f'{path}: no py_binary, py_test or py_library owns it')
    print(f'{len(tracked)} tracked .py files, {len(tracked) - len(unowned)} owned by a Bazel target')
    return 1 if unowned else 0


if __name__ == '__main__':
    sys.exit(main())
