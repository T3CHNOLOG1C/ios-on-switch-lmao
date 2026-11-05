#!/usr/bin/env python3
"""assemble.py --bundle D --seed D [--raw D] --trustcache F --trust-json F --out D

The GPU build's directory as the old build-tegra-gm20b-metal.py left out/gpu, which deploy's GPU staging
(stage-tegra-gm20b-metal.py --build) reads:
  ngpu/addons/TegraGM20BMetal.bundle/            the signed bundle
  ngpu/addons/TegraGM20BMetal.m2v-seed/shared/   the shader cache seed (SEED, v1/)
  ngpu/addons/TegraGM20BMetal.raw/pack.bin       the raw GM20B path's pipeline pack (the plugin uses it when staged)
  TegraGM20BMetal.trustcache, trust.json
"""
import argparse
import shutil
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    for name in ('bundle', 'seed', 'trustcache', 'trust-json', 'out'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--raw')
    args = parser.parse_args()
    out = Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    addons = out / 'ngpu/addons'
    shutil.copytree(args.bundle, addons / 'TegraGM20BMetal.bundle', copy_function=shutil.copyfile)
    shutil.copytree(args.seed, addons / 'TegraGM20BMetal.m2v-seed/shared', copy_function=shutil.copyfile)
    if args.raw:
        shutil.copytree(args.raw, addons / 'TegraGM20BMetal.raw', copy_function=shutil.copyfile)
    shutil.copyfile(args.trustcache, out / 'TegraGM20BMetal.trustcache')
    shutil.copyfile(args.trust_json, out / 'trust.json')
    # Regular files only: the inputs are followed, never linked.
    executables = {addons / 'TegraGM20BMetal.bundle' / name for name in ('TegraGM20BMetal', 'libvulkan_nouveau.dylib')}
    for path in [out, *out.rglob('*')]:
        path.chmod(0o755 if path.is_dir() or path in executables else 0o644)


if __name__ == '__main__':
    main()
