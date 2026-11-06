#!/usr/bin/env python3
"""Assemble the GPU build's directory, which deploy's GPU staging (stage-tegra-gm20b-metal.py --build) reads.

usage: assemble.py --bundle D --trustcache F --trust-json F [--overlay D]... --out D

  ngpu/addons/TegraGM20BMetal.bundle/            the signed bundle
  ngpu/addons/TegraGM20BMetal.m2v-seed/shared/   the shader cache seed, from an overlay (//gpu/seed:tree)
  ngpu/addons/TegraGM20BMetal.raw/pack.bin       the raw GM20B path's pipeline pack, from an overlay
  TegraGM20BMetal.trustcache, trust.json
"""

import argparse
import shutil
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ('bundle', 'trustcache', 'trust-json', 'out'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--overlay', action='append', default=[])
    args = parser.parse_args()
    out = Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    addons = out / 'ngpu/addons'
    shutil.copytree(args.bundle, addons / 'TegraGM20BMetal.bundle', copy_function=shutil.copyfile)
    for overlay in args.overlay:
        shutil.copytree(overlay, out, copy_function=shutil.copyfile, dirs_exist_ok=True)
    shutil.copyfile(args.trustcache, out / 'TegraGM20BMetal.trustcache')
    shutil.copyfile(args.trust_json, out / 'trust.json')
    # Regular files only: the inputs are followed, never linked.
    executables = {addons / 'TegraGM20BMetal.bundle' / name for name in ('TegraGM20BMetal', 'libvulkan_nouveau.dylib')}
    for path in [out, *out.rglob('*')]:
        path.chmod(0o755 if path.is_dir() or path in executables else 0o644)


if __name__ == '__main__':
    main()
