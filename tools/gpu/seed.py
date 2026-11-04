#!/usr/bin/env python3
"""Generate the shared m2v shader cache seed for one build (the precompile half of the old build-tegra-gm20b-metal.py).

    seed.py --xcode-env F --host-bridge <darwin libnative_metal_bridge.a> --mock-iokit mock_iokit.c --abi-header
            TegraGM20BABI.h --precompile precompile.mm --frontend NativeBridgeMetalFrontend.mm --metal-header
            NativeBridgeMetalFrontend.h --nvk <iOS dylib> --twin <host dylib> --qc default.metallib --store <dir>
            --corpus corpus.log --libraries libraries.py --out <seed dir>

The cache the device would write with this build for the corpus's states, made on this Mac: every function
specialization and render pipeline state of precompile/corpus.log, translated by this build's frontend and m2v
built for this Mac (the cache's build directory hashes the sources) and compiled by the NVK twin, which takes the
iOS dylib's pipeline cache identity, over a GM20B mock of the kernel's user client. Writes <out>/SEED and <out>/v1.
Gates (as before): the twin's .source must equal the iOS dylib's, and the QuartzCore metallib must be 21H461's.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

QC_SHA256 = '606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9'   # 21H461 QuartzCore default.metallib
CONTENT = '529a20fd270ababa3e5ec2fe4d1674d93739ba5bcd61861674b4fb6db349af9f'     # m2v's content directory


def lc_uuid(path, value=None):
    """The LC_UUID of the thin 64-bit Mach-O at `path`, after setting it to `value` (16 bytes) when given."""
    data = bytearray(Path(path).read_bytes())
    magic, _, _, _, ncmds = struct.unpack_from('<5I', data)
    at = 32
    for _ in range(ncmds if magic == 0xfeedfacf else 0):
        cmd, size = struct.unpack_from('<2I', data, at)
        if cmd == 0x1b:   # LC_UUID
            if value is not None:
                data[at + 8:at + 24] = value
                Path(path).write_bytes(data)
            return bytes(data[at + 8:at + 24])
        at += size
    sys.exit('%s: no LC_UUID' % path)


def main():
    parser = argparse.ArgumentParser()
    for name in ('xcode-env', 'host-bridge', 'mock-iokit', 'abi-header', 'precompile', 'frontend', 'metal-header',
                 'nvk', 'twin', 'qc', 'store', 'corpus', 'libraries', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    root = Path.cwd()
    xcode = dict(line.split('=', 1) for line in Path(args.xcode_env).read_text().splitlines() if '=' in line)
    work = root / '_seed_work'
    if work.exists():
        shutil.rmtree(work)
    (work / 'home').mkdir(parents=True)
    tool_env = {'PATH': '/usr/bin:/bin', 'DEVELOPER_DIR': xcode['DEVELOPER_DIR'], 'ZERO_AR_DATE': '1',
                'HOME': str(work / 'home'), 'TMPDIR': os.environ.get('TMPDIR', str(work))}

    def run(command, env=tool_env):
        result = subprocess.run([str(c) for c in command], env=env, capture_output=True, text=True)
        if result.returncode:
            sys.exit('FAILED: %s\n%s%s' % (' '.join(map(str, command))[:600], result.stdout[-4000:], result.stderr[-6000:]))
        return result.stdout + result.stderr

    sources = [Path(str(p) + '.source') for p in (args.nvk, args.twin)]
    if not all(p.is_file() for p in sources) or sources[0].read_text() != sources[1].read_text():
        sys.exit('%s and %s missing or different: the twin is not of the iOS dylib\'s sources' % tuple(sources))
    if hashlib.sha256(Path(args.qc).read_bytes()).hexdigest() != QC_SHA256:
        sys.exit('%s is not 21H461\'s QuartzCore default.metallib' % args.qc)

    prefix_maps = ['-ffile-prefix-map=%s=/ioswitch' % root]
    run(['/usr/bin/xcrun', 'clang', '-O1', '-Wall', '-dynamiclib', *prefix_maps, '-I', Path(args.abi_header).parent,
         args.mock_iokit, '-framework', 'IOKit', '-framework', 'CoreFoundation',
         '-install_name', '@rpath/libmock_iokit.dylib', '-o', work / 'libmock_iokit.dylib'])
    run(['/usr/bin/xcrun', 'clang++', '-O2', '-std=c++17', '-fobjc-arc', '-Wno-deprecated-declarations', *prefix_maps,
         '-I', Path(args.metal_header).parent, '-x', 'objective-c++', args.precompile, args.frontend, '-x', 'none',
         args.host_bridge, '-L', work, '-lmock_iokit', '-Wl,-rpath,' + str(work), '-framework', 'Foundation',
         '-framework', 'Metal', '-framework', 'IOSurface', '-framework', 'IOKit', '-o', work / 'precompile'])
    # The twin with the device's pipeline cache identity: NVK hashes the -Dnvk-build-id when the build had one (both
    # builds had the same, .source says), else its own LC_UUID, which the twin then takes from the iOS dylib.
    dylib = work / 'libvulkan_nouveau.dylib'
    shutil.copyfile(args.twin, dylib)
    dylib.chmod(0o755)
    pinned = any(line.split()[1:] for line in sources[0].read_text().splitlines() if line.startswith('nvk-build-id'))
    if not pinned:
        lc_uuid(dylib, lc_uuid(args.nvk))
    run(['/usr/bin/codesign', '--force', '--sign', '-', dylib])

    store = Path(args.store)
    corpus = Path(args.corpus)
    if (store / 'names.json').is_file():
        corpus = work / 'corpus.log'
        print(run([sys.executable, '-I', args.libraries, store, 'resolve', args.corpus, corpus]).rstrip())
    cache = work / 'cache'
    env = dict(tool_env, MESA_SHADER_CACHE_DISABLE='true')
    made = run([work / 'precompile', '--libs', store, dylib, cache, args.qc, corpus], env=env)
    print('\n'.join(line for line in made.splitlines() if line.startswith(('library ', 'precompile:'))))
    builds = sorted(p.name for p in (cache / 'v1').iterdir() if p.name != CONTENT)
    records = sorted((cache / 'v1' / CONTENT).glob('*/*'))
    if len(builds) != 1 or not records:
        sys.exit('the precompile made no single build directory and records: %s' % builds)
    digest = hashlib.sha256()
    for path in sorted((cache / 'v1').rglob('*')):
        if path.is_file():
            digest.update(str(path.relative_to(cache)).encode() + b'\0' + path.read_bytes())
    pipeline_uuid = records[0].read_bytes()[20:36].hex()   # the record's identity: vendor, device, driver, pipelineCacheUUID
    seed = 'generated m2v-build %s nvk %s content %s' % (builds[0][:8], pipeline_uuid[:8], digest.hexdigest()[:8])

    out = Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(cache / 'v1', out / 'v1', copy_function=shutil.copyfile)
    (out / 'SEED').write_text(seed + '\n')
    for path in [out, *out.rglob('*')]:
        path.chmod(0o755 if path.is_dir() else 0o644)
    shutil.rmtree(work)
    print('seed shared:', seed)


if __name__ == '__main__':
    main()
