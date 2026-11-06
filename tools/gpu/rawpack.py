#!/usr/bin/env python3
"""Make the raw GM20B path's pipeline pack (drivers/TegraGM20BMetal/raw/pack/rawpack.py) beside the seed.

As the old build made it, in two actions (gpu/seed/defs.bzl: seed_tools builds, seed_raw_pack runs):

    rawpack.py build --xcode-env F --host-bridge <darwin bridge .a> --generator <raw/pack/rawpack.py> --tree <tree>
                     --out <dir>
        the generator's host tools (the probe through this build's frontend and host bridge, the mock GM20B, the raw
        path's ycbcr.metallib) into <dir>
    rawpack.py run --shader-tools <dir> --qc default.metallib --corpus corpus.log --generator <raw/pack/rawpack.py>
                   --tree <tree> --out <dir>
        Core Animation's corpus states made through the shader tools (//gpu/seed:tools: raw/ holds those three, beside
        the NVK twin) over the mock GM20B, read off the decoded push stream, stamped with the tools' stack key. Writes
        <out>/pack.bin. Runs no compiler: the image builder runs it with the release's prebuilt tools.

A corpus edit reruns only the run; a frontend or raw/pack source edit both.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest='step', required=True)
    for step, names in (
        ('build', ('xcode-env', 'host-bridge', 'generator', 'tree', 'out')),
        ('run', ('shader-tools', 'qc', 'corpus', 'generator', 'tree', 'out')),
    ):
        p = steps.add_parser(step)
        for name in names:
            p.add_argument('--' + name, required=True)
    args = parser.parse_args()
    root = Path.cwd()
    work = root / f'_rawpack_{args.step}_work'
    if work.exists():
        shutil.rmtree(work)
    (work / 'home').mkdir(parents=True)
    env = {
        'PATH': '/usr/bin:/bin',
        'ZERO_AR_DATE': '1',
        'HOME': str(work / 'home'),
        'TMPDIR': os.environ.get('TMPDIR', str(work)),
        'RAWPACK_TREE': str(root / args.tree),
    }
    if args.step == 'build':
        xcode = dict(line.split('=', 1) for line in Path(args.xcode_env).read_text().splitlines() if '=' in line)
        env['DEVELOPER_DIR'] = xcode['DEVELOPER_DIR']
    generator = [sys.executable, str(root / args.generator)]
    if args.step == 'build':
        result = subprocess.run(
            [*generator, '--build-only', '--bridge', str(root / args.host_bridge), '--out', str(root / args.out)],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode:
            sys.exit(f"the pack generator's tools failed:\n{(result.stdout + result.stderr)[-6000:]}")
        shutil.rmtree(work)
        return
    tools = root / args.shader_tools
    dylib = work / 'libvulkan_nouveau.dylib'
    shutil.copyfile(tools / 'libvulkan_nouveau.dylib', dylib)
    dylib.chmod(0o755)
    generated = work / 'out'
    stack = json.loads((tools / 'MANIFEST.json').read_text())['stack']
    result = subprocess.run(
        [
            *generator,
            '--prebuilt',
            str(tools / 'raw'),
            '--nvk',
            str(dylib),
            '--qc',
            str(root / args.qc),
            '--corpus',
            str(root / args.corpus),
            '--out',
            str(generated),
            '--stack',
            stack,
            '--clean',
        ],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode or not (generated / 'pack.bin').is_file():
        sys.exit(f'the pack generator failed:\n{(result.stdout + result.stderr)[-6000:]}')
    print('\n'.join(line for line in result.stdout.splitlines() if not line.startswith('  skipped')))
    out = Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    shutil.copyfile(generated / 'pack.bin', out / 'pack.bin')
    shutil.rmtree(work)
    print(f'raw pack: made, stack {stack}')


if __name__ == '__main__':
    main()
