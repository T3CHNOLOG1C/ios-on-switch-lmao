#!/usr/bin/env python3
"""r8-webgpu: relink an NVK darwin dylib so that the entrypoint tables' weak references nothing defines (Mesa's vk_entrypoints_gen.py
--weak; ~2960 nvk_/vk_/wsi_ names) are absolute symbols at 0, resolved by the static linker, instead of `-undefined dynamic_lookup`
flat-namespace lookups dyld makes at every dlopen: each such lookup searches every image of the process (an iOS app has hundreds),
which made every Metal device open in every process pay thousands of misses. The tables read 0 for them, as they did.
  relink-nvk.py <meson build dir> [<target> default src/nouveau/vulkan/libvulkan_nouveau.dylib]"""
import re
import shlex
import subprocess
import sys
from pathlib import Path

build = Path(sys.argv[1]).resolve()
target = sys.argv[2] if len(sys.argv) > 2 else 'src/nouveau/vulkan/libvulkan_nouveau.dylib'
dylib = build / target


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit('FAILED %s\n%s%s' % (cmd if isinstance(cmd, str) else ' '.join(map(str, cmd)), r.stdout[-3000:], r.stderr[-3000:]))
    return r.stdout


names = [line.split()[-4] for line in run(['xcrun', 'nm', '-m', str(dylib)]).splitlines()
         if '(undefined)' in line and line.endswith('(dynamically looked up)')]
if not names:
    print('relink-nvk: no flat-namespace lookups in', dylib)
    sys.exit(0)
odd = [n for n in names if not re.match(r'_(nvk|vk|wsi)_[A-Za-z0-9_]+$', n)]
if odd:
    sys.exit('relink-nvk: flat lookups that are not entrypoint table names: %s' % odd[:20])
stubs = build / 'nvk-absent-entrypoints.s'
stubs.write_text(''.join('.private_extern %s\n%s = 0\n' % (n, n) for n in names))
obj = build / 'nvk-absent-entrypoints.o'
build_version = run(['xcrun', 'vtool', '-show-build', str(dylib)]).split()
platform = build_version[build_version.index('platform') + 1] if 'platform' in build_version else 'MACOS'
minos = build_version[build_version.index('minos') + 1] if 'minos' in build_version else '14.0'
triple = {'IOS': 'arm64-apple-ios', 'MACOS': 'arm64-apple-macos'}[platform] + minos
run(['xcrun', 'clang', '-c', '-target', triple, str(stubs), '-o', str(obj)])
# the target's own link command, from ninja, without the flat lookups and with the absolute symbols
commands = run(['ninja', '-C', str(build), '-t', 'commands', target]).splitlines()
link = next(c for c in reversed(commands) if ' -o ' + target in c or ('-o ' + target) in c)
args = shlex.split(link)
if '-Wl,-undefined,dynamic_lookup' not in args:
    sys.exit('relink-nvk: the link command has no -undefined dynamic_lookup')
args = [a for a in args if a != '-Wl,-undefined,dynamic_lookup']
out = args.index('-o')
args[out + 2:out + 2] = [str(obj)]
run(args, cwd=str(build))
left = [line for line in run(['xcrun', 'nm', '-m', str(dylib)]).splitlines() if 'dynamically looked up' in line]
if left:
    sys.exit('relink-nvk: still %d flat lookups' % len(left))
print('relink-nvk: %d absent entrypoints made absolute (0); no flat-namespace lookups left in %s' % (len(names), dylib))
