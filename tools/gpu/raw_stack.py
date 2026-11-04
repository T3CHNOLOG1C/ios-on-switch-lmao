#!/usr/bin/env python3
"""raw_stack.py --nvk <iOS NVK dylib> --drivers <tree/drivers> --series <patches/series> --raw <drivers/TegraGM20BMetal/raw>
               --out <file>

The raw GM20B path's stack key (the old build script's raw_stack()): a hash of what the raw pack's contents depend on,
known before the plugin is compiled, which is compiled in (-DTEGRA_RAW_STACK) and stamped into the pack, so the device
refuses a pack of another build. The NVK dylib (its context init, NAK's programs and register values), m2v (the patch
series, in order, and the bridge sources: the binding rules), the frontend (the journal's state keys) and the raw
runtime and pack generator (every .h/.c/.cpp/.mm/.py/.inc below raw/, in path order), each by its file name and bytes.
Writes the first 16 hex digits."""
import argparse
import hashlib
from pathlib import Path

SUFFIXES = ('.h', '.c', '.cpp', '.mm', '.py', '.inc')


def main():
    parser = argparse.ArgumentParser()
    for name in ('nvk', 'drivers', 'series', 'raw', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    drivers, raw = Path(args.drivers), Path(args.raw)
    patches = [drivers / line.strip() for line in Path(args.series).read_text().splitlines()
               if line.strip() and not line.startswith('#')]
    metal = drivers / 'NativeMetal'
    sources = [metal / 'bridge/NativeMetalBridge.rs', metal / 'bridge/NativeMetalObjects.rs',
               metal / 'NativeBridgeMetalFrontend.mm']
    raw_files = sorted(p for p in raw.rglob('*') if p.is_file() and p.suffix in SUFFIXES)
    digest = hashlib.sha256(Path(args.nvk).read_bytes())
    for path in [*patches, *sources, *raw_files]:
        digest.update(str(path.name).encode() + b'\0' + path.read_bytes())
    Path(args.out).write_text(digest.hexdigest()[:16] + '\n')


if __name__ == '__main__':
    main()
