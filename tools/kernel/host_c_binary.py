"""host_c_binary.py --xcode-env F --out O [--flag F]... SOURCE...: a small host (macOS arm64) C tool built from pinned
source with Xcode's clang and the macOS SDK (lz4 for the kernel window)."""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument('--xcode-env', required=True)
parser.add_argument('--out', required=True)
parser.add_argument('--flag', action='append', default=[])
parser.add_argument('sources', nargs='+')
args = parser.parse_args()
xcode = common.xcode_env(args.xcode_env)
subprocess.run(
    [
        'xcrun',
        '--sdk',
        'macosx',
        'clang',
        '-arch',
        'arm64',
        '-O2',
        '-isysroot',
        xcode['MACOSX_SDK'],
        *args.flag,
        *args.sources,
        '-o',
        args.out,
    ],
    check=True,
    env=common.environment(xcode),
)
