#!/usr/bin/env python3
"""Assemble //gpu:base, the directory the GPU stager (stage-tegra-gm20b-metal.py --build) and the image builder read.

usage: assemble.py --bundle D --trustcache F --trust-json F --out D

  ngpu/addons/TegraGM20BMetal.bundle/            the signed bundle
  TegraGM20BMetal.trustcache, trust.json
"""

import argparse
import shutil
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ('bundle', 'trustcache', 'trust-json', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    out = Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    addons = out / 'ngpu/addons'
    shutil.copytree(args.bundle, addons / 'TegraGM20BMetal.bundle', copy_function=shutil.copyfile)
    shutil.copyfile(args.trustcache, out / 'TegraGM20BMetal.trustcache')
    shutil.copyfile(args.trust_json, out / 'trust.json')
    # Regular files only: the inputs are followed, never linked.
    executables = {addons / 'TegraGM20BMetal.bundle' / name for name in ('TegraGM20BMetal', 'libvulkan_nouveau.dylib')}
    for path in [out, *out.rglob('*')]:
        path.chmod(0o755 if path.is_dir() or path in executables else 0o644)


if __name__ == '__main__':
    main()
