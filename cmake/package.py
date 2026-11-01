#!/usr/bin/env python3
"""CMake's packaging step around scripts/package_raw_boot.py, which keeps the boot-args
(COMMAND_LINE), the ADT edits (ipad_adt) and the boot.cmd template handling.

Usage: package.py <kernel-dir> <boot-dir> [--smp]
           package <kernel-dir>/kernel.macho (staging in <kernel-dir>/package) into <boot-dir>
       package.py --inputs
           print the files packaging reads, one per line, for CMake's dependencies"""
import json
import shutil
import sys
import tempfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT))
from scripts import package_raw_boot as pkg  # noqa: E402


def inputs():
    sources = json.loads(pkg.INPUTS.read_text())['sources']
    files = [pkg.INPUTS, pkg.TEMPLATE, *sorted(p for p in pkg.FIRMWARE.iterdir() if p.is_file())]
    files += [Path(source['path']) for name, source in sources.items() if name != 'kernel']
    files += [PROJECT / 'scripts' / name for name in ('package_raw_boot.py', 'native_xnu_layout.py',
              'build_native_loader.py', 'package_native_handoff.py', 'native_oled_adt.py')]
    for directory in ('native/loader', 'native/handoff'):
        files += sorted(p for p in (PROJECT / directory).iterdir() if p.suffix in ('.c', '.h', '.S', '.ld'))
    files += [PROJECT / 'native/storage/sd_reference.c', PROJECT / 'native/storage/sd_reference.h']
    return files


def main():
    if sys.argv[1:] == ['--inputs']:
        print('\n'.join(str(p) for p in inputs()))
        return
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    kernel_dir, boot_dir, flags = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).absolute(), sys.argv[3:]
    # build_native_loader.py only writes below <project>/out (package_native_handoff.safe_output).
    if not kernel_dir.is_relative_to((PROJECT / 'out').resolve()):
        raise SystemExit(f'{kernel_dir} is not below {PROJECT / "out"}: configure the CMake build directory there')
    sys.argv = ['package_raw_boot.py', str(kernel_dir), *flags]
    default = PROJECT / 'out/boot' / pkg.NAME
    if boot_dir == default:
        pkg.main()
        return
    # main() writes ROOT/out/boot/<NAME> (ROOT is read there at run time; the inputs were
    # resolved at import), so give it a scratch ROOT and move the entry where it was asked for.
    with tempfile.TemporaryDirectory(dir=kernel_dir) as root:
        pkg.ROOT = Path(root)
        pkg.main()
        if boot_dir.exists():
            shutil.rmtree(boot_dir)
        boot_dir.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(Path(root) / 'out/boot' / pkg.NAME), str(boot_dir))
    print('moved to', boot_dir)


if __name__ == '__main__':
    main()
