"""Fail when a file the pinned NVK outputs were generated from (inputs.txt) is no longer what they were made from.

    check_inputs.py <inputs.txt> <mesa source root> <mesa build root>  (build/<path> is under the build root)

nvkcl.spv/.cpp/.h (mesa_clc and vtn_bindgen2) and astc_spv.h (glslangValidator) are checked in because the build does
not build those compilers; when a Mesa change touches what they read, regenerate them with the real tools and update
inputs.txt.
"""

import hashlib
import sys
from pathlib import Path


def main() -> int:
    """Check every line of inputs.txt; return 1 and list the files that differ if any does."""
    inputs, source, build = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    bad = []
    for line in inputs.read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        tool, digest, path = line.split(None, 2)
        local = build / path.removeprefix('build/') if path.startswith('build/') else source / path
        if not local.is_file() or hashlib.sha256(local.read_bytes()).hexdigest() != digest:
            bad.append(f'{tool}: {path}')
    for entry in bad:
        print(f'{entry} is not the file the pinned output was generated from')
    if bad:
        print('regenerate the pinned outputs in third_party/mesa/bazel/pinned with the real tools; update inputs.txt')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
