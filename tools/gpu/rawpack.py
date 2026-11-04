#!/usr/bin/env python3
"""rawpack.py --xcode-env F --host-bridge <darwin bridge .a> --nvk <iOS dylib> --twin <host dylib> --qc default.metallib
              --corpus corpus.log --stack <stack file> --generator <raw/pack/rawpack.py> --tree <tree> --out <dir>

The raw GM20B path's pipeline pack (drivers/TegraGM20BMetal/raw/pack/rawpack.py) as the old build made it beside the
seed: Core Animation's corpus states made through this build's frontend, host bridge and NVK twin over a mock GM20B,
read off the decoded push stream, stamped with the build's stack key. Writes <out>/pack.bin."""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

from tools.gpu.nvk_twin import make_twin


def main():
    parser = argparse.ArgumentParser()
    for name in ('xcode-env', 'host-bridge', 'nvk', 'twin', 'qc', 'corpus', 'stack', 'generator', 'tree', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    root = Path.cwd()
    xcode = dict(line.split('=', 1) for line in Path(args.xcode_env).read_text().splitlines() if '=' in line)
    work = root / '_rawpack_work'
    if work.exists():
        shutil.rmtree(work)
    (work / 'home').mkdir(parents=True)
    env = {'PATH': '/usr/bin:/bin', 'DEVELOPER_DIR': xcode['DEVELOPER_DIR'], 'ZERO_AR_DATE': '1',
           'HOME': str(work / 'home'), 'TMPDIR': os.environ.get('TMPDIR', str(work)), 'RAWPACK_TREE': str(root / args.tree)}
    dylib = work / 'libvulkan_nouveau.dylib'
    make_twin(args.nvk, args.twin, dylib, env)
    generated = work / 'out'
    stack = Path(args.stack).read_text().strip()
    result = subprocess.run([sys.executable, str(root / args.generator), '--bridge', str(root / args.host_bridge),
                             '--nvk', str(dylib), '--qc', str(root / args.qc), '--corpus', str(root / args.corpus),
                             '--out', str(generated), '--stack', stack, '--clean'], env=env, capture_output=True, text=True)
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
