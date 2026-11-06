#!/usr/bin/env python3
"""Stage the TegraGM20B Metal plugin onto the SD card's iPadOS Data volume, mounted on the Mac.

usage: stage-tegra-gm20b-metal.py <Data mount point> (--build <outdir> | --remove) [--no-raw]

<outdir> is the directory //gpu:bundle builds (bazel-bin/gpu/bundle; //deploy:sd --stage-gpu passes it). Exit status 0
on success, else one error line.

Mount the Data volume read-write first, e.g.
`diskutil mount -mountOptions nobrowse D1B1D053-D0A6-496C-8625-A947A3DA863E` (SwitchRawMount.h SWITCH_RAW_DATA_UUID),
and eject the card before it goes back into the Switch.

As vendor/unixnu/scripts/add-gpu-plugin-data.py --publish (UniXNU-sheng), on the live volume instead of a seed image:
  * <Data>/ngpu/addons/TegraGM20BMetal.bundle from <build outdir>/ngpu/addons (//gpu:bundle). The kernel shows
    /private/var/ngpu/addons over /System/Library/Extensions;
  * <Data>/ngpu/addons/TegraGM20BMetal.m2v-seed beside it, when the build has one (the shader cache seeds, --seed);
  * CAEnableDeepFramebuffer, CAEnableFramebufferCompression and CAEnableIOSurfaceCompression = false merged into the
    preferences backboardd reads when it starts, before any plugin code runs (its own domain and the global one, for
    mobile and root): the GPU and the display driver take plain 8-bit rows.
  * <Data>/ngpu/addons/TegraGM20BMetal.raw beside it, when the build has one (the raw GM20B path's pipeline pack: with
    it staged, backboardd draws through the raw path; --no-raw, or TEGRA_RAW_STAGE=0 in the environment, stages the
    bundle without it, and the render server keeps the m2v device);
--remove deletes the bundle again (the preferences stay; without a Metal device they change nothing).
The CDHashes must also be in the boot entry's static trust cache (//boot:package appends //gpu:trustcache, the same
TegraGM20BMetal.trustcache), so stage and package from the same build.
"""

import os
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

DATA_UUID = 'D1B1D053-D0A6-496C-8625-A947A3DA863E'
NAME = 'TegraGM20BMetal.bundle'
PREFS = {
    'CAEnableDeepFramebuffer': False,
    'CAEnableFramebufferCompression': False,
    'CAEnableIOSurfaceCompression': False,
}


def main() -> None:
    argv = sys.argv[1:]
    remove = '--remove' in argv
    raw = '--no-raw' not in argv and os.environ.get('TEGRA_RAW_STAGE', '1') != '0'
    argv = [a for a in argv if a != '--no-raw']
    build = None  # //gpu:bundle's directory (//deploy:sd passes it)
    if '--build' in argv[:-1]:
        build = Path(argv[argv.index('--build') + 1]).resolve()
        del argv[argv.index('--build') : argv.index('--build') + 2]
    args = [a for a in argv if a != '--remove']
    if len(args) != 1 or (build is None and not remove):
        sys.exit(__doc__)
    data = Path(args[0]).resolve()
    info = plistlib.loads(
        subprocess.run(['diskutil', 'info', '-plist', str(data)], capture_output=True, check=True).stdout
    )
    if info.get('VolumeUUID') != DATA_UUID or info.get('MountPoint') != str(data) or not info.get('WritableVolume'):
        sys.exit('%s is not the iPadOS Data volume %s mounted read-write' % (data, DATA_UUID))
    if not (data / 'mobile').is_dir():
        sys.exit('%s has no mobile/; wrong volume?' % data)
    target = data / 'ngpu/addons' / NAME
    seed_target = data / 'ngpu/addons/TegraGM20BMetal.m2v-seed'
    raw_target = data / 'ngpu/addons/TegraGM20BMetal.raw'
    for old in (target, seed_target, raw_target):
        if old.exists():
            shutil.rmtree(old)
    if remove:
        print('removed', target)
        return
    source = build / 'ngpu/addons' / NAME
    if not (source / 'TegraGM20BMetal').is_file() or not (build / 'TegraGM20BMetal.trustcache').is_file():
        sys.exit('stage-tegra-gm20b-metal: no plugin build in %s (bazel build //gpu:bundle)' % build)
    for directory in (data / 'ngpu', data / 'ngpu/addons'):
        directory.mkdir(exist_ok=True)
        directory.chmod(0o755)
    shutil.copytree(source, target, symlinks=False, copy_function=shutil.copyfile)
    for path in [target, *target.rglob('*')]:
        path.chmod(0o755 if path.is_dir() or path.name in ('TegraGM20BMetal', 'libvulkan_nouveau.dylib') else 0o644)
    if (build / 'ngpu/addons/TegraGM20BMetal.m2v-seed').is_dir():
        shutil.copytree(build / 'ngpu/addons/TegraGM20BMetal.m2v-seed', seed_target, copy_function=shutil.copyfile)
        for path in [seed_target, *seed_target.rglob('*')]:
            path.chmod(0o755 if path.is_dir() else 0o644)
    if raw and (build / 'ngpu/addons/TegraGM20BMetal.raw/pack.bin').is_file():
        shutil.copytree(build / 'ngpu/addons/TegraGM20BMetal.raw', raw_target, copy_function=shutil.copyfile)
        for path in [raw_target, *raw_target.rglob('*')]:
            path.chmod(0o755 if path.is_dir() else 0o644)
    for home, owner in (('mobile', 501), ('root', 0)):
        directory = data / home / 'Library/Preferences'
        directory.mkdir(parents=True, exist_ok=True)
        # backboardd's defaults domain is its bundle ID, com.apple.backboardd; the global domain is what took effect
        # so far
        for name in ('com.apple.backboardd.plist', 'backboardd.plist', '.GlobalPreferences.plist'):
            file = directory / name
            values = plistlib.loads(file.read_bytes()) if file.exists() else {}
            values.update(PREFS)
            file.write_bytes(plistlib.dumps(values, fmt=plistlib.FMT_BINARY))
            file.chmod(0o644)
            if os.geteuid() == 0:
                os.chown(file, owner, owner)
    for leftover in (data / 'ngpu').rglob('._*'):  # AppleDouble files a non-APFS copy may leave
        leftover.unlink()
    subprocess.run(['sync'], check=True)
    print(
        'staged',
        target,
        sorted(p.name for p in target.iterdir()),
        'seed' if seed_target.exists() else 'no seed',
        'raw pack' if raw_target.exists() else 'no raw pack',
    )


if __name__ == '__main__':
    main()
