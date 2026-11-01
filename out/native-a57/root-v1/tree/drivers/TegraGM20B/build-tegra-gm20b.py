#!/usr/bin/env python3
"""Build drivers/TegraGM20B into one archive for the kernel link: <outdir>/libTegraGM20B.a (link it with -force_load).

usage: build-tegra-gm20b.py <outdir> [--with-i2c]

Units and flags:
  * vendored nouveau (nouveau/**.c) and the C shim/platform/gpu files: the C diagnostics flags from
    passes/native-root-persistent-data-v1/archive-commands.json (IOSwitchDiag.c), minus its -I/-include, plus our include
    paths and -std=gnu11. Vendored files only also get -Wno-pointer-sign -Wno-sign-compare -Wno-missing-field-initializers
    (the relaxations Linux builds nvkm with). Everything stays -Wall -Wextra -Werror.
  * C++ (shim/xnu_glue.cpp, TegraGM20B.cpp, TegraGM20BUserClient.cpp, TegraGM20BAccelerator.cpp):
    build/native-ioswitch-root/compile-command.json. TegraGM20BAccelerator.cpp subclasses the original IOAcceleratorES, so its
    static initializer is removed from __mod_init_func and exported as __GLOBAL__sub_I_TegraGM20BAccelerator.cpp (TegraGM20B.cpp
    runs it after IOGPUFamily). Its IOAcceleratorES imports are bound by drivers/IOSwitchAccelStub/original-bindings.S, which
    the kernel link already has; this archive carries no bindings.
  * firmware.S: .incbin of firmware/nvidia/gm20b/** (linux-firmware, LICENCE.nvidia).
  * --with-i2c adds drivers/TegraI2C/TegraI2C.cpp, only for a link without the block archive (which carries TegraI2C).
"""
import json
import os
import subprocess
import sys
from pathlib import Path

DRV = Path(__file__).resolve().parent
TREE = DRV.parents[1]
WORK = TREE.parent


def record(path):
    return json.loads(path.read_text().strip().rstrip(','))


def run(cmd, cwd=None):
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True,
                       env=dict(os.environ, PATH='/opt/homebrew/opt/llvm/bin:' + os.environ['PATH']))
    if r.returncode:
        sys.exit('FAILED: %s\n%s%s' % (' '.join(map(str, cmd))[:400], r.stdout[-4000:], r.stderr[-6000:]))
    return r.stdout


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        sys.exit(__doc__)
    out = Path(args[0]).resolve()
    (out / 'obj').mkdir(parents=True, exist_ok=True)
    objs = []

    # C flags: the diagnostics unit's command without its source, output and include options
    cmds = json.loads((WORK / 'passes/native-root-persistent-data-v1/archive-commands.json').read_text())
    diag = next(c for c in cmds if str(TREE / 'diagnostics/ioswitch/IOSwitchDiag.c') in c)
    cflags, skip = [], False
    for a in diag:
        if skip:
            skip = False
        elif a in ('-I', '-include', '-o', '-isystem'):
            skip = True
        elif not (a.endswith('.c') or a.startswith('-I')):
            cflags.append(a)
    c_dir = record(TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/arm_init.o.json')['directory']
    nv = DRV / 'nouveau'
    inc = ['-std=gnu11', '-Wno-sign-conversion', '-Wno-shorten-64-to-32', '-Wno-implicit-int-conversion', '-I', DRV / 'shim/include', '-I', nv / 'include', '-I', nv / 'include/nvkm', '-I', nv / 'nvkm',
           '-I', nv / 'nvkm/subdev/gsp', '-I', nv, '-include', DRV / 'shim/include/nvxnu_prelude.h']
    vendored_wno = ['-Wno-pointer-sign', '-Wno-sign-compare', '-Wno-missing-field-initializers', '-Wno-shadow',
                    '-Wno-conditional-uninitialized']
    c_units = sorted(nv.rglob('*.c')) + [DRV / 'shim' / f for f in ('linux.c', 'tegra_shim.c', 'stubs.c', 'nvif_nvkm.c')] + \
        [DRV / 'platform.c', DRV / 'gpu.c']
    for src in c_units:
        o = out / 'obj' / (str(src.relative_to(DRV)).replace('/', '_') + '.o')
        extra = vendored_wno if src.is_relative_to(nv) else []
        run(cflags + extra + inc + [src, '-o', o], cwd=c_dir)
        objs.append(o)

    # C++: the block driver's recorded command
    block = json.loads((TREE / 'build/native-ioswitch-root/compile-command.json').read_text())
    cxx_dir = record(TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')['directory']
    cxx_units = [DRV / 'shim/xnu_glue.cpp', DRV / 'TegraGM20B.cpp', DRV / 'TegraGM20BUserClient.cpp',
                 DRV / 'TegraGM20BAccelerator.cpp']
    if '--with-i2c' in sys.argv:
        cxx_units.append(TREE / 'drivers/TegraI2C/TegraI2C.cpp')
    for src in cxx_units:
        o = out / 'obj' / (src.stem + '.o')
        cmd = list(block)
        cmd[cmd.index(str(TREE / 'drivers/NativeIOSwitchBlock/NativeIOSwitchBlock.cpp'))] = str(src)
        cmd[cmd.index('-o') + 1] = str(o)
        run(cmd + ['-I', DRV], cwd=cxx_dir)
        if src.stem == 'TegraGM20BAccelerator':   # late object, as cmake's ioswitch_late_object()
            late = out / 'obj/TegraGM20BAccelerator.late.o'
            run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
                 '--globalize-symbol=__GLOBAL__sub_I_TegraGM20BAccelerator.cpp', o, late])
            o = late
        objs.append(o)

    sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']).strip()
    run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', DRV / 'firmware.S',
         '-I', DRV / 'firmware', '-o', out / 'obj/firmware.o'])
    objs.append(out / 'obj/firmware.o')

    lib = out / 'libTegraGM20B.a'
    lib.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', lib] + objs)
    print('built', lib, lib.stat().st_size, 'bytes,', len(objs), 'objects')


if __name__ == '__main__':
    main()
