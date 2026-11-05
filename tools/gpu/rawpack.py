#!/usr/bin/env python3
"""The raw GM20B path's pipeline pack (drivers/TegraGM20BMetal/raw/pack/rawpack.py) as the old build made it beside the
seed, in two actions of //gpu:raw_pack:

    rawpack.py build --xcode-env F --host-bridge <darwin bridge .a> --generator <raw/pack/rawpack.py> --tree <tree>
                     --out <dir>
        the generator's host tools (the probe through this build's frontend and host bridge, the mock GM20B, the raw
        path's ycbcr.metallib) into <dir>
    rawpack.py run --xcode-env F --tools <dir> --twin-dylib <seed.py twin's dylib> --qc default.metallib
                   --corpus corpus.log --stack <stack file> --generator <raw/pack/rawpack.py> --tree <tree> --out <dir>
        Core Animation's corpus states made through those tools and the NVK twin over the mock GM20B, read off the decoded
        push stream, stamped with the build's stack key. Writes <out>/pack.bin.

A corpus edit reruns only the run; a frontend or raw/pack source edit both.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest='step', required=True)
    for step, names in (('build', ('xcode-env', 'host-bridge', 'generator', 'tree', 'out')),
                        ('run', ('xcode-env', 'tools', 'twin-dylib', 'qc', 'corpus', 'stack', 'generator', 'tree', 'out'))):
        p = steps.add_parser(step)
        for name in names:
            p.add_argument('--' + name, required=True)
    args = parser.parse_args()
    root = Path.cwd()
    xcode = dict(line.split('=', 1) for line in Path(args.xcode_env).read_text().splitlines() if '=' in line)
    work = root / ('_rawpack_%s_work' % args.step)
    if work.exists():
        shutil.rmtree(work)
    (work / 'home').mkdir(parents=True)
    env = {'PATH': '/usr/bin:/bin', 'DEVELOPER_DIR': xcode['DEVELOPER_DIR'], 'ZERO_AR_DATE': '1',
           'HOME': str(work / 'home'), 'TMPDIR': os.environ.get('TMPDIR', str(work)), 'RAWPACK_TREE': str(root / args.tree)}
    generator = [sys.executable, str(root / args.generator)]
    if args.step == 'build':
        result = subprocess.run(generator + ['--build-only', '--bridge', str(root / args.host_bridge), '--out', str(root / args.out)],
                                env=env, capture_output=True, text=True)
        if result.returncode:
            sys.exit('the pack generator\'s tools failed:\n%s' % (result.stdout + result.stderr)[-6000:])
        shutil.rmtree(work)
        return
    # The twin with the device's pipeline cache identity (seed.py twin, its own action), at the path it had here.
    dylib = work / 'libvulkan_nouveau.dylib'
    shutil.copyfile(args.twin_dylib, dylib)
    dylib.chmod(0o755)
    generated = work / 'out'
    stack = Path(args.stack).read_text().strip()
    result = subprocess.run(generator + ['--prebuilt', str(root / args.tools), '--nvk', str(dylib), '--qc', str(root / args.qc),
                                         '--corpus', str(root / args.corpus), '--out', str(generated), '--stack', stack,
                                         '--clean'], env=env, capture_output=True, text=True)
    if result.returncode or not (generated / 'pack.bin').is_file():
        sys.exit('the pack generator failed:\n%s' % (result.stdout + result.stderr)[-6000:])
    print('\n'.join(line for line in result.stdout.splitlines() if not line.startswith('  skipped')))
    out = Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    shutil.copyfile(generated / 'pack.bin', out / 'pack.bin')
    shutil.rmtree(work)
    print('raw pack: made, stack %s' % stack)


if __name__ == '__main__':
    main()
