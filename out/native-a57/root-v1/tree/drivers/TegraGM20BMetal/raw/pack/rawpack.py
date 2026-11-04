#!/usr/bin/env python3
"""rawpack: the raw GM20B path's pipeline pack (runtime/raw_pack.h), made on this Mac from the precompile corpus through this
build's own frontend + m2v (the host bridge) and NVK (the host twin with the device's pipeline cache identity), over a mock GM20B
whose every submission is executed by nvdec (pack/nvdec.cpp, Mesa's Fermi MME simulator in pack/mesa). Core Animation's render
pipeline states only (PROBE_LABEL): the render server's.

  1. probe (pack/probe.mm) makes every state and draws it once with tagged inputs, three times: payloads of 256, 16 and 4096
     bytes (a uniform buffer's range is NVK's 256-byte rounding of the payload; the 4096-byte run gives the function's window);
  2. mkprograms.py reads NAK's programs out of the NVK driver records the run left in its m2v cache directory;
  3. mkpack.py reads the probe logs and writes pack.bin: per state its programs, registers, push words and descriptor layout,
     plus NVK's context init with the addresses to relocate. States m2v draws with more than one draw or with dispatches (the
     extended-range formats' shadow images and merges) are left out: the raw path hands them to m2v.

usage: rawpack.py --bridge <host libnative_metal_bridge.a> --nvk <host twin dylib> --qc <default.metallib> --corpus <corpus.log>
                  --out <dir> [--stack <hex key>]
writes <out>/pack.bin, pack.json, layouts.txt, report.txt and keeps the probe logs in <out>/work (deleted with --clean).
--stack: the build's stack key (the plugin is compiled with the same value), stamped into the pack header: the runtime refuses a
pack of another build.
"""
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent            # drivers/TegraGM20BMetal/raw/pack
RAW = HERE.parent
TREE = Path(os.environ.get('RAWPACK_TREE', RAW.parents[2]))   # tree/ (drivers/TegraGM20BMetal/raw -> tree)
METAL = TREE / 'drivers/NativeMetal'
ABI = TREE / 'drivers/TegraGM20B'


def run(cmd, env=None, out=None):
    r = subprocess.run([str(c) for c in cmd], capture_output=out is None, text=True, env=env,
                       stdout=open(out, 'w') if out else None, stderr=subprocess.PIPE if out else None)
    if r.returncode:
        sys.exit('rawpack: FAILED: %s\n%s%s' % (' '.join(map(str, cmd))[:400], (r.stdout or '')[-3000:], (r.stderr or '')[-5000:]))
    return (r.stdout or '') + (r.stderr or '')


def option(name, required=True):
    if name in sys.argv[1:-1]:
        return sys.argv[sys.argv.index(name) + 1]
    if required:
        sys.exit(__doc__)
    return None


def main():
    bridge, nvk, qc, corpus = (Path(option(n)).resolve() for n in ('--bridge', '--nvk', '--qc', '--corpus'))
    out = Path(option('--out')).resolve()
    stack = int(option('--stack', False) or '0', 16)
    work = out / 'work'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    # the mock GPU with nvdec on every submission, and the probe through the frontend + host bridge
    cflags = ['-O2', '-Wall', '-I', ABI]
    objs = []
    for src in ('mesa/mme_fermi.c', 'mesa/mme_fermi_sim.c'):
        obj = work / (Path(src).stem + '.o')
        run(['xcrun', 'clang', '-O2', '-std=c11', '-w', '-I', HERE / 'mesa', '-c', HERE / src, '-o', obj])
        objs.append(obj)
    run(['xcrun', 'clang', *cflags, '-c', HERE / 'mock_iokit.c', '-o', work / 'mock_iokit.o'])
    run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / 'cap.cpp', '-o', work / 'cap.o'])
    run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / 'nvdec.cpp', '-o', work / 'nvdec.o'])
    run(['xcrun', 'clang++', '-dynamiclib', work / 'mock_iokit.o', work / 'cap.o', work / 'nvdec.o', *objs, '-framework', 'IOKit',
         '-framework', 'CoreFoundation', '-install_name', '@rpath/libmock_iokit.dylib', '-o', work / 'libmock_iokit.dylib'])
    run(['xcrun', 'clang++', '-O2', '-std=c++17', '-fobjc-arc', '-Wno-deprecated-declarations', '-I', METAL, '-x', 'objective-c++',
         HERE / 'probe.mm', METAL / 'NativeBridgeMetalFrontend.mm', '-x', 'none', bridge, '-L', work, '-lmock_iokit',
         '-Wl,-rpath,' + str(work), '-lobjc', '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
         '-framework', 'IOKit', '-o', work / 'probe'])
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('NAK_', 'NVK_', 'MESA_', 'NMB_', 'M2V_', 'METAL2VULKAN_', 'TEGRA_', 'MTL_', 'NVDEC_', 'RAWCAP_', 'PROBE_'))}
    env.update(MESA_SHADER_CACHE_DISABLE='true', NVDEC_PROBE='1', NVDEC_SCRUB='1', NVDEC_TRACE='3', NMB_ASYNC_PSO='0', NMB_ASYNC_PSO_API='0')
    # first a run that only fills the m2v cache with NVK's driver records (NAK's programs), then the three decoded runs, which
    # name each bound program by those records (RAWCAP_PROGRAMS) and hit the same cache
    cache = work / 'cache'
    cache.mkdir()

    def probe(name, extra):
        r = subprocess.run([str(work / 'probe'), str(nvk), str(cache), str(qc), str(corpus)], env=dict(env, **extra),
                           stdout=open(work / ('%s.out' % name), 'w'), stderr=subprocess.PIPE, text=True)
        if r.returncode:
            sys.exit('rawpack: probe %s failed:\n%s' % (name, r.stderr[-4000:]))
        tail = [l for l in r.stderr.splitlines() if l.startswith('probe:')]
        print('rawpack: %s: %s' % (name, tail[-1] if tail else '?'))

    probe('warm', {})
    print(run([sys.executable, HERE / 'mkprograms.py', work / 'programs.bin', cache]).strip())
    for size in (256, 16, 4096):
        probe('probe%d' % size, dict(PROBE_BYTES=str(size), RAWCAP_LOG=str(work / ('probe%d.log' % size)),
                                     RAWCAP_PROGRAMS=str(work / 'programs.bin')))
    report = run([sys.executable, HERE / 'mkpack.py', work / 'probe256.out', work / 'probe256.log', work / 'programs.bin', out,
                  work / 'probe'])
    if stack:
        pack = bytearray((out / 'pack.bin').read_bytes())
        struct.pack_into('<Q', pack, 8 + 16 + 8 * 2 * 8, stack)   # PackHeader.stack (after the section table)
        (out / 'pack.bin').write_bytes(pack)
    print(report.rstrip())
    if '--clean' in sys.argv[1:]:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
