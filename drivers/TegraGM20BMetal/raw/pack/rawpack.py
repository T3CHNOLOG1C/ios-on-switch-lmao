#!/usr/bin/env python3
"""rawpack: the raw GM20B path's pipeline pack (runtime/raw_pack.h), made on this Mac.

Core Animation's render pipeline states (PROBE_LABEL: the render server's) go through this build's own frontend + m2v
(the host bridge) and NVK (the host twin with the device's pipeline cache identity), over a mock GM20B whose every
submission is executed by nvdec (pack/nvdec.cpp, Mesa's Fermi MME simulator in pack/mesa). The states are made from
their names (ca/caset.py, ca/caspec.py: QuartzCore's own decoding of a name into a state, with this IPSW's tables,
ca/qc-<build>.json): the corpus's names, QuartzCore's offline compilation manifest and their closure over the blend CA
puts in the pipeline (CA_TIERS). With no tables of this QuartzCore (another IPSW's), the corpus's states as they are.

  1. probe (pack/probe.mm) makes every state and draws it once with tagged inputs, three times: payloads of 256, 16 and
     4096 bytes (a uniform buffer's range is NVK's 256-byte rounding of the payload; the 4096-byte run gives the
     function's window);
  2. mkprograms.py reads NAK's programs out of the NVK driver records the run left in its m2v cache directory;
  3. mkpack.py reads the probe logs and writes pack.bin: per state its programs, registers, push words and descriptor
     layout, plus NVK's context init with the addresses to relocate. States m2v draws with more than one draw or with
     dispatches (the extended-range formats' shadow images and merges) are left out: the raw path hands them to m2v.

usage: rawpack.py --bridge <host libnative_metal_bridge.a> --nvk <host twin dylib> --qc <default.metallib>
                  --corpus <corpus.log> --out <dir> [--stack <hex key>] [--prebuilt <dir>]
       rawpack.py --build-only --bridge <host libnative_metal_bridge.a> --out <dir>
writes <out>/pack.bin, pack.json, layouts.txt, report.txt and keeps the probe logs in <out>/work (deleted with --clean).
--build-only compiles the probe, the mock GPU (libmock_iokit.dylib) and ycbcr.metallib into <dir> and stops; --prebuilt
<dir> runs those instead of compiling them (//gpu/seed:local_tools makes them its own action, so a corpus edit only
reruns the probes).
--stack: the build's stack key (the plugin is compiled with the same value), stamped into the pack header: the runtime
refuses a pack of another build.
"""

import contextlib
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent  # drivers/TegraGM20BMetal/raw/pack
RAW = HERE.parent
TREE = Path(os.environ.get('RAWPACK_TREE', RAW.parents[2]))  # tree/ (drivers/TegraGM20BMetal/raw -> tree)
METAL = TREE / 'drivers/NativeMetal'
ABI = TREE / 'drivers/TegraGM20B'


def run(cmd: list[str | Path], env: dict[str, str] | None = None, out: Path | None = None) -> str:
    with open(out, 'w') if out else contextlib.nullcontext() as stdout:
        r = subprocess.run(
            [str(c) for c in cmd],
            capture_output=out is None,
            text=True,
            env=env,
            stdout=stdout,
            stderr=subprocess.PIPE if out else None,
            check=False,
        )
    if r.returncode:
        sys.exit(
            f'rawpack: FAILED: {" ".join(map(str, cmd))[:400]}\n{(r.stdout or "")[-3000:]}{(r.stderr or "")[-5000:]}'
        )
    return (r.stdout or '') + (r.stderr or '')


def option(name: str, required: bool = True) -> str | None:
    if name in sys.argv[1:-1]:
        return sys.argv[sys.argv.index(name) + 1]
    if required:
        sys.exit(__doc__)
    return None


BUILT = ('probe', 'libmock_iokit.dylib', 'ycbcr.metallib')
# The Core Animation states the pack holds (ca/caset.py's tiers). The fetch tier (the fragment blends a compositing
# filter asks for, over every fixed-function fragment state) is left out: its ~11300 fragment programs would make the
# shader heap, uploaded whole at open, some 50 MB.
CA_TIERS = ('corpus', 'manifest', 'blend')


def ca_corpus(qc: Path, corpus: Path, work: Path) -> Path:
    """Give the corpus with its Core Animation lines made from their names, when ca/ has this QuartzCore's tables."""
    digest = hashlib.sha256(qc.read_bytes()).hexdigest()
    for path in sorted((HERE / 'ca').glob('qc-*.json')):
        with open(path) as f:
            if json.load(f).get('metallib_sha256') == digest:
                break
    else:
        print(f"rawpack: no QuartzCore tables for default.metallib {digest[:16]}: the corpus's states as they are")
        return corpus
    sys.path.insert(0, str(HERE / 'ca'))
    import caset  # noqa: PLC0415 (ca/ is only on the path here)
    import caspec  # noqa: PLC0415

    tables = caspec.Tables(str(path))
    out = work / 'corpus.ca.log'
    with open(corpus) as src, open(out, 'w') as dst:
        dst.writelines(line for line in src if not caspec.corpus_fields(line)[0])
        for tier, names in caset.labels(tables, caset.corpus_names(str(corpus)), CA_TIERS).items():
            dst.writelines(
                f'R ok=1 label=com.apple.coreanimation.{n} {caspec.state_fields(tables, n)} |\n' for n in names
            )
            print(f'rawpack: Core Animation states, {tier}: {len(names)}')
    return out


def build(work: Path, bridge: Path) -> None:
    """Build the mock GPU, the probe and the raw path's own states into `work`.

    The mock GPU with nvdec on every submission, the probe through the frontend + host bridge (beside it: rpath
    @executable_path) and the raw path's own states: the 4:2:0 YCbCr conversions (ycbcr.metal), an iOS library like
    QuartzCore's, drawn after Core Animation's (probe.mm PROBE_YCBCR_LIB).
    """
    # Source paths (Mesa's asserts' __FILE__) as /switch-ios/..., whichever checkout or sandbox this runs in: the probe
    # and the mock are an action's outputs, and the run's cache key is their bytes.
    maps = [f'-ffile-prefix-map={RAW.parents[2]}=/switch-ios/tree', f'-ffile-prefix-map={Path.cwd()}=/switch-ios']
    cflags = ['-O2', '-Wall', *maps, '-I', ABI]
    objs = []
    for src in ('mesa/mme_fermi.c', 'mesa/mme_fermi_sim.c'):
        obj = work / (Path(src).stem + '.o')
        run(['xcrun', 'clang', '-O2', '-std=c11', '-w', *maps, '-I', HERE / 'mesa', '-c', HERE / src, '-o', obj])
        objs.append(obj)
    run(['xcrun', 'clang', *cflags, '-c', HERE / 'mock_iokit.c', '-o', work / 'mock_iokit.o'])
    run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / 'cap.cpp', '-o', work / 'cap.o'])
    run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / 'nvdec.cpp', '-o', work / 'nvdec.o'])
    run(
        [
            'xcrun',
            'clang++',
            '-dynamiclib',
            work / 'mock_iokit.o',
            work / 'cap.o',
            work / 'nvdec.o',
            *objs,
            '-framework',
            'IOKit',
            '-framework',
            'CoreFoundation',
            '-install_name',
            '@rpath/libmock_iokit.dylib',
            '-o',
            work / 'libmock_iokit.dylib',
        ]
    )
    run(
        [
            'xcrun',
            'clang++',
            '-O2',
            '-std=c++17',
            '-fobjc-arc',
            '-Wno-deprecated-declarations',
            *maps,
            '-I',
            METAL,
            '-x',
            'objective-c++',
            HERE / 'probe.mm',
            METAL / 'NativeBridgeMetalFrontend.mm',
            '-x',
            'none',
            bridge,
            '-L',
            work,
            '-lmock_iokit',
            '-Wl,-rpath,@executable_path',
            '-lobjc',
            '-framework',
            'Foundation',
            '-framework',
            'Metal',
            '-framework',
            'IOSurface',
            '-framework',
            'IOKit',
            '-o',
            work / 'probe',
        ]
    )
    run(['xcrun', '-sdk', 'iphoneos', 'metal', '-c', HERE / 'ycbcr.metal', '-I', HERE, '-o', work / 'ycbcr.air'])
    run(['xcrun', '-sdk', 'iphoneos', 'metallib', work / 'ycbcr.air', '-o', work / 'ycbcr.metallib'])


def main() -> None:
    if '--build-only' in sys.argv[1:]:
        out = Path(option('--out')).resolve()
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as scratch:
            build(Path(scratch), Path(option('--bridge')).resolve())
            for name in BUILT:
                shutil.copyfile(Path(scratch) / name, out / name)
                (out / name).chmod(0o755)
        return
    nvk, qc, corpus = (Path(option(n)).resolve() for n in ('--nvk', '--qc', '--corpus'))
    out = Path(option('--out')).resolve()
    stack = int(option('--stack', False) or '0', 16)
    work = out / 'work'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    corpus = ca_corpus(qc, corpus, work)
    prebuilt = option('--prebuilt', False)
    if prebuilt:
        for name in BUILT:
            shutil.copyfile(Path(prebuilt) / name, work / name)
            (work / name).chmod(0o755)
    else:
        build(work, Path(option('--bridge')).resolve())
    env = {
        k: v
        for k, v in os.environ.items()
        if not k.startswith(
            ('NAK_', 'NVK_', 'MESA_', 'NMB_', 'M2V_', 'METAL2VULKAN_', 'TEGRA_', 'MTL_', 'NVDEC_', 'RAWCAP_', 'PROBE_')
        )
    }
    # NMB_CACHE_MB: the m2v cache directory holds every state's NVK driver record until mkprograms reads them (the
    # bridge's 256 MiB cap evicts the oldest past ~7000 states, which then bind programs no record names)
    env.update(
        MESA_SHADER_CACHE_DISABLE='true',
        NVDEC_PROBE='1',
        NVDEC_SCRUB='1',
        NVDEC_TRACE='3',
        NMB_ASYNC_PSO='0',
        NMB_ASYNC_PSO_API='0',
        NMB_CACHE_MB='65536',
    )
    env.update(PROBE_YCBCR_LIB=str(work / 'ycbcr.metallib'))
    # first a run that only fills the m2v cache with NVK's driver records (NAK's programs), then the three decoded runs,
    # which name each bound program by those records (RAWCAP_PROGRAMS) and hit the same cache
    cache = work / 'cache'
    cache.mkdir()

    def probe(name: str, extra: dict[str, str]) -> None:
        with open(work / f'{name}.out', 'w') as out:
            r = subprocess.run(
                [str(work / 'probe'), str(nvk), str(cache), str(qc), str(corpus)],
                env=dict(env, **extra),
                stdout=out,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
        if r.returncode:
            sys.exit(f'rawpack: probe {name} failed:\n{r.stderr[-4000:]}')
        tail = [line for line in r.stderr.splitlines() if line.startswith('probe:')]
        print(f'rawpack: {name}: {tail[-1] if tail else "?"}')

    probe('warm', {})
    print(run([sys.executable, HERE / 'mkprograms.py', work / 'programs.bin', cache]).strip())
    for size in (256, 16, 4096):
        probe(
            f'probe{size}',
            {
                'PROBE_BYTES': str(size),
                'RAWCAP_LOG': str(work / f'probe{size}.log'),
                'RAWCAP_PROGRAMS': str(work / 'programs.bin'),
            },
        )
    report = run(
        [
            sys.executable,
            HERE / 'mkpack.py',
            work / 'probe256.out',
            work / 'probe256.log',
            work / 'programs.bin',
            out,
            work / 'probe',
        ]
    )
    if stack:
        pack = bytearray((out / 'pack.bin').read_bytes())
        struct.pack_into('<Q', pack, 8 + 16 + 8 * 2 * 8, stack)  # PackHeader.stack (after the section table)
        (out / 'pack.bin').write_bytes(pack)
    print(report.rstrip())
    if '--clean' in sys.argv[1:]:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
