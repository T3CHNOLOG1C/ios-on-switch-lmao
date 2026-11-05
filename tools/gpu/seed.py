#!/usr/bin/env python3
"""The shared m2v shader cache seed for one build (the precompile half of the old build-tegra-gm20b-metal.py), as four
steps of //gpu:seed (gpu/defs.bzl metal_seed), each its own action:

    seed.py mock --xcode-env F --abi-header TegraGM20BABI.h --mock-iokit mock_iokit.c --out libmock_iokit.dylib
    seed.py tool --xcode-env F --metal-header NativeBridgeMetalFrontend.h --precompile precompile.mm
                 --frontend NativeBridgeMetalFrontend.mm --host-bridge <darwin libnative_metal_bridge.a>
                 --mock libmock_iokit.dylib --out precompile
    seed.py twin --nvk <iOS dylib> --twin <host dylib> --out libvulkan_nouveau.dylib   (the raw pack runs it too)
    seed.py run  --xcode-env F --tool precompile --mock libmock_iokit.dylib --nvk libvulkan_nouveau.dylib
                 --qc default.metallib --qc-sha256 H --root <dir> [--embedded <header>]... [--data <sha256>.metallib]...
                 --corpus <resolved corpus.log> [--records <driver record store>] --out <seed dir>

The corpus is resolved before the run by precompile/libraries.py resolve (its own action), so a corpus edit reruns that
and the run, a frontend edit the tool and the run, an NVK edit the twin and the run.

The cache the device would write with this build for the corpus's states, made on this Mac: every function
specialization and render pipeline state of precompile/corpus.log, translated by this build's frontend and m2v
built for this Mac (the cache's build directory hashes the sources) and compiled by the NVK twin, which takes the
iOS dylib's pipeline cache identity, over a GM20B mock of the kernel's user client. Writes <out>/SEED and <out>/v1.
The libraries the corpus names come from the store precompile.mm reads (precompile/libraries.py), laid out here: root/ the
OS volume's (--root, @gpu_apple_libs), data/<sha256>.metallib every library a process may make from bytes: those files', the
frontend's own (--embedded, a C array of a metallib's bytes) and the ones only the device made (--data, @gpu_device_data).
--records: the device's own driver records (@gpu_device_data, precompile/harvest.py --pack), each m2v's VkPipelineCache data
of one pipeline making named by what the making reads: those of the precompile's driver identity (vendor, device, driver
version, pipelineCacheUUID: this NVK's) whose envelope checks out join the seed's content directory beside the precompile's
own (which win a name both have). They cover the makings no corpus state's creation predicts (a draw's variant) and those
the precompile makes otherwise than the device; another NVK's records are left out.
Gates (as before): the twin's .source must equal the iOS dylib's, and the QuartzCore metallib must be the IPSW kind's
(--qc-sha256, ipsw/pins.bzl's quartzcore_metallib_sha256).
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from tools.gpu.nvk_twin import make_twin

CONTENT = '529a20fd270ababa3e5ec2fe4d1674d93739ba5bcd61861674b4fb6db349af9f'     # m2v's content directory


RECORD_MAGIC = b'M2VPPLC\0'   # m2v shader_cache/driver.rs: magic, identity (28 bytes), data length (u64), data, SHA-256


def merge_records(store, content, made):
    """The records of `store` (<xx>/<name>) of the identity of `made` (one of the precompile's) and a whole envelope, copied
    into `content` where it has no record of that name; (merged, made here already, other identity, refused) or None."""
    if not store or not store.is_dir():
        return None
    identity = made.read_bytes()[8:36]
    merged = here = other = refused = 0
    for path in sorted(store.glob('*/*')):
        data = path.read_bytes()
        whole = (len(data) >= 76 and data[:8] == RECORD_MAGIC
                 and 44 + int.from_bytes(data[36:44], 'little') + 32 == len(data)
                 and hashlib.sha256(data[:-32]).digest() == data[-32:])
        if not whole:
            refused += 1
        elif data[8:36] != identity:
            other += 1
        elif (content / path.parent.name / path.name).exists():
            here += 1
        else:
            (content / path.parent.name).mkdir(parents=True, exist_ok=True)
            (content / path.parent.name / path.name).write_bytes(data)
            merged += 1
    return merged, here, other, refused


def store_view(store, root, embedded, data):
    """The library store precompile.mm reads (precompile/libraries.py) at `store`: root/ -> `root`, and data/<sha256>.metallib
    for every library of `root`, each metallib of the `embedded` headers and each of `data`. Returns the data/ count."""
    (store / 'data').mkdir(parents=True)
    (store / 'root').symlink_to(root.absolute())
    made = {}
    for path in sorted(p for p in root.rglob('*') if p.is_file()):
        made[hashlib.sha256(path.read_bytes()).hexdigest()] = path
    for header in embedded:
        text = Path(header).read_text()
        library = bytes(int(byte, 16) for byte in re.findall(r'0x([0-9a-fA-F]{2})', text[text.index('{'):text.rindex('}')]))
        name = hashlib.sha256(library).hexdigest()
        (store / 'data' / (name + '.metallib')).write_bytes(library)
        made.pop(name, None)
    for path in map(Path, data):
        name = path.name.removesuffix('.metallib')
        if hashlib.sha256(path.read_bytes()).hexdigest() != name:
            sys.exit('%s is not the library its name says' % path)
        made[name] = path.absolute()
    for name, path in made.items():
        (store / 'data' / (name + '.metallib')).symlink_to(path)
    return sum(1 for _ in (store / 'data').iterdir())


def _xcode_env(path, work):
    xcode = dict(line.split('=', 1) for line in Path(path).read_text().splitlines() if '=' in line)
    (work / 'home').mkdir(parents=True, exist_ok=True)
    return {'PATH': '/usr/bin:/bin', 'DEVELOPER_DIR': xcode['DEVELOPER_DIR'], 'ZERO_AR_DATE': '1',
            'HOME': str(work / 'home'), 'TMPDIR': os.environ.get('TMPDIR', str(work))}


def _run(command, env):
    result = subprocess.run([str(c) for c in command], env=env, capture_output=True, text=True)
    if result.returncode:
        sys.exit('FAILED: %s\n%s%s' % (' '.join(map(str, command))[:600], result.stdout[-4000:], result.stderr[-6000:]))
    return result.stdout + result.stderr


def mock(args, root, work):
    env = _xcode_env(args.xcode_env, work)
    _run(['/usr/bin/xcrun', 'clang', '-O1', '-Wall', '-dynamiclib', '-ffile-prefix-map=%s=/switch-ios' % root, '-I',
          Path(args.abi_header).parent, args.mock_iokit, '-framework', 'IOKit', '-framework', 'CoreFoundation',
          '-install_name', '@rpath/libmock_iokit.dylib', '-o', args.out], env)


def tool(args, root, work):
    """The precompile tool, linked against the mock IOKit beside it (the run puts the two side by side)."""
    env = _xcode_env(args.xcode_env, work)
    _run(['/usr/bin/xcrun', 'clang++', '-O2', '-std=c++17', '-fobjc-arc', '-Wno-deprecated-declarations',
          '-ffile-prefix-map=%s=/switch-ios' % root, '-I', Path(args.metal_header).parent, '-x', 'objective-c++',
          args.precompile, args.frontend, '-x', 'none', args.host_bridge, '-L', Path(args.mock).parent, '-lmock_iokit',
          '-Wl,-rpath,@executable_path', '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
          '-framework', 'IOKit', '-o', args.out], env)


def twin(args, root, work):
    # The twin with the device's pipeline cache identity (tools/gpu/nvk_twin.py).
    (work / 'home').mkdir()
    make_twin(args.nvk, args.twin, args.out,
              {'PATH': '/usr/bin:/bin', 'HOME': str(work / 'home'), 'TMPDIR': os.environ.get('TMPDIR', str(work))})


def run(args, root, work):
    env = _xcode_env(args.xcode_env, work)
    if hashlib.sha256(Path(args.qc).read_bytes()).hexdigest() != args.qc_sha256:
        sys.exit("%s is not the IPSW kind's QuartzCore default.metallib (%s)" % (args.qc, args.qc_sha256))
    for name, path in (('precompile', args.tool), ('libmock_iokit.dylib', args.mock), ('libvulkan_nouveau.dylib', args.nvk)):
        shutil.copyfile(path, work / name)
        (work / name).chmod(0o755)
    store = work / 'store'
    print('library store: %d libraries made from bytes' % store_view(store, root / args.root, args.embedded, args.data))
    cache = work / 'cache'
    made = _run([work / 'precompile', '--libs', store, work / 'libvulkan_nouveau.dylib', cache, args.qc, args.corpus],
                dict(env, MESA_SHADER_CACHE_DISABLE='true'))
    print('\n'.join(line for line in made.splitlines() if line.startswith(('library ', 'precompile:'))))
    builds = sorted(p.name for p in (cache / 'v1').iterdir() if p.name != CONTENT)
    records = sorted((cache / 'v1' / CONTENT).glob('*/*'))
    if len(builds) != 1 or not records:
        sys.exit('the precompile made no single build directory and records: %s' % builds)
    merged = merge_records(Path(args.records) if args.records else None, cache / 'v1' / CONTENT, records[0])
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
    print('seed shared:', seed)
    if merged:
        print('seed records: %d precompiled, %d of the device\'s merged (%d already made here, %d of another identity, '
              '%d refused)' % (len(records), *merged))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest='step', required=True)
    for step, names in (('mock', ('xcode-env', 'abi-header', 'mock-iokit', 'out')),
                        ('tool', ('xcode-env', 'metal-header', 'precompile', 'frontend', 'host-bridge', 'mock', 'out')),
                        ('twin', ('nvk', 'twin', 'out')),
                        ('run', ('xcode-env', 'tool', 'mock', 'nvk', 'qc', 'qc-sha256', 'root', 'corpus', 'out'))):
        p = steps.add_parser(step)
        for name in names:
            p.add_argument('--' + name, required=True)
        if step == 'run':
            p.add_argument('--embedded', action='append', default=[])
            p.add_argument('--data', action='append', default=[])
            p.add_argument('--records')
    args = parser.parse_args()
    root = Path.cwd()
    work = root / ('_seed_%s_work' % args.step)
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    {'mock': mock, 'tool': tool, 'twin': twin, 'run': run}[args.step](args, root, work)
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
