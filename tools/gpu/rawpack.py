#!/usr/bin/env python3
"""Make the raw GM20B path's pipeline pack (drivers/TegraGM20BMetal/raw/pack/rawpack.py) beside the seed.

As the old build made it, in two actions (gpu/seed/defs.bzl: seed_tools builds, seed_raw_pack runs):

    rawpack.py build --xcode-env F --host-bridge <darwin bridge .a> --generator <raw/pack/rawpack.py> --tree <tree>
                     --out <dir>
        the generator's host tools (the probe through this build's frontend and host bridge, the mock GM20B, the raw
        path's ycbcr.metallib) into <dir>
    rawpack.py corpus --corpus corpus.log --out <file>
        the corpus lines the probe makes (raw/pack/probe.mm: Core Animation's render states, without lib=), so an edit
        to any other line leaves the run's input as it was
    rawpack.py run --nvk <twin dylib> --raw <dir> --manifest <MANIFEST.json> --qc default.metallib --corpus <file>
                   --generator <raw/pack/rawpack.py> --tree <tree> --out <dir>
        those states made through the shader tools (//gpu/seed:tools: raw/ holds the probe, the mock and
        ycbcr.metallib; the NVK twin) over the mock GM20B, read off the decoded push stream, stamped with the tools'
        stack key. Writes <out>/pack.bin. Runs no compiler: the image builder runs it with the release's prebuilt tools.

A Core Animation corpus edit reruns only the run; a frontend or raw/pack source edit both.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROBE_LABEL = ' label=com.apple.coreanimation.'
LIBRARY = re.compile(r' lib=\S+')


def probe_lines(corpus: str) -> list[str]:
    """Return the corpus lines probe.mm makes, as it reads them."""
    return [
        LIBRARY.sub('', line)
        for line in corpus.split('\n')
        if line.startswith('R ') and ' ok=1 ' in line and PROBE_LABEL in line
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest='step', required=True)
    for step, names in (
        ('build', ('xcode-env', 'host-bridge', 'generator', 'tree', 'out')),
        ('corpus', ('corpus', 'out')),
        ('run', ('nvk', 'raw', 'manifest', 'qc', 'corpus', 'generator', 'tree', 'out')),
    ):
        p = steps.add_parser(step)
        for name in names:
            p.add_argument('--' + name, required=True)
    args = parser.parse_args()
    if args.step == 'corpus':
        Path(args.out).write_text(''.join(line + '\n' for line in probe_lines(Path(args.corpus).read_text())))
        return
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
    dylib = work / 'libvulkan_nouveau.dylib'
    shutil.copyfile(root / args.nvk, dylib)
    dylib.chmod(0o755)
    generated = work / 'out'
    stack = json.loads((root / args.manifest).read_text())['stack']
    result = subprocess.run(
        [
            *generator,
            '--prebuilt',
            str(root / args.raw),
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
