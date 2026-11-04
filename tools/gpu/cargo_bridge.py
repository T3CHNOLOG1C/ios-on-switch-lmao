#!/usr/bin/env python3
"""Build the Metal bridge (drivers/NativeMetal/bridge/NativeMetalBridge.rs over m2v) as a static library with pinned
cargo, offline, inside a Bazel action (the bridge step of the old build-tegra-gm20b-metal.py).

    cargo_bridge.py --cargo C --rustc R --m2v DIR --vendor DIR --manifest Cargo.toml.in --lock Cargo.lock
                    --bridge-rs F --objects-rs F --fixture-air F --fc-air F --xcode-env F --target T [--quick]
                    --output lib.a

The crate is laid out in a scratch directory below the action's working directory: bridge/ (manifest, lock, sources)
with bridge/m2v a link to the patched m2v tree, excluded from the bridge's workspace (Cargo.toml.in). Cargo hashes a
path dependency into its crates' symbol names (-C metadata) by its path relative to the workspace root when it lies
inside it, else by its absolute path, which differs in every sandbox; inside, the hash is the same wherever the action
runs. Every absolute path the compiler would embed (panic locations, debug info) is remapped to /ioswitch/... by a
rustc wrapper (cargo does not see the flags, so they change no crate metadata), and the C compiler of sha2's
assembly gets the same maps.
`--quick` is the precompile's host build: no LTO, 16 codegen units, no debug info (the shader cache keys hash the
sources, not the code).
"""
import argparse
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

ANCHOR = '        let mut options = DeviceOptions::default();\n'
POLICY = '        options.policy.framebuffer_fetch = metal2vulkan::device::FramebufferFetchPolicy::AcceptUnordered;\n'


def main():
    parser = argparse.ArgumentParser()
    for name in ('cargo', 'rustc', 'm2v', 'vendor', 'manifest', 'lock', 'bridge-rs', 'objects-rs', 'fixture-air',
                 'fc-air', 'xcode-env', 'target', 'output'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--quick', action='store_true')
    parser.add_argument('--std', action='append', default=[], help='a standard library file (lib/rustlib/<triple>/lib/*)')
    args = parser.parse_args()

    root = Path.cwd()
    work = root / '_cargo_bridge' / args.target
    if work.exists():
        shutil.rmtree(work)
    (work / 'bridge/src').mkdir(parents=True)
    (work / 'bridge/m2v').symlink_to(root / args.m2v)
    shutil.copyfile(args.manifest, work / 'bridge/Cargo.toml')
    shutil.copyfile(args.lock, work / 'bridge/Cargo.lock')
    shutil.copyfile(args.bridge_rs, work / 'bridge/src/NativeMetalBridge.rs')
    objects = Path(args.objects_rs).read_text()
    if objects.count(ANCHOR) != 1:
        sys.exit('NativeMetalObjects.rs changed: no single device-options line to amend')
    # NVK on Maxwell has neither rasterization-order attachment access nor fragment shader interlock: the device takes
    # unordered framebuffer fetch (the old build script's one amendment).
    (work / 'bridge/src/NativeMetalObjects.rs').write_text(objects.replace(ANCHOR, ANCHOR + POLICY))

    home = work / 'cargo-home'
    home.mkdir()
    (home / 'config.toml').write_text('\n'.join([
        '[source.crates-io]', 'replace-with = "vendored"',
        '[source.vendored]', 'directory = "%s"' % (root / args.vendor),
        '[net]', 'offline = true', '']))

    # A sysroot of exactly the declared standard libraries (the iOS one and this Mac's, which build scripts need):
    # rustc's own would be the toolchain repository's, which the sandbox shows only in part.
    sysroot = work / 'sysroot'
    for std in args.std:
        parts = Path(std).parts
        triple = parts[parts.index('rustlib') + 1]
        link = sysroot / 'lib/rustlib' / triple / 'lib' / Path(std).name
        link.parent.mkdir(parents=True, exist_ok=True)
        if not link.exists():
            link.symlink_to(root / std)
    remaps = ['--sysroot=%s' % sysroot,
              '--remap-path-prefix=%s=/ioswitch' % root,
              '--remap-path-prefix=%s=/ioswitch/m2v' % (root / args.m2v),
              '--remap-path-prefix=%s=/ioswitch/m2v-build' % work,
              '--remap-path-prefix=%s=/ioswitch/m2v' % (work / 'bridge/m2v')]
    wrapper = work / 'rustc-wrapper'
    wrapper.write_text('#!/bin/sh\nexec "$@" %s\n' % ' '.join(remaps))
    wrapper.chmod(wrapper.stat().st_mode | stat.S_IEXEC)

    xcode = dict(line.split('=', 1) for line in Path(args.xcode_env).read_text().splitlines() if '=' in line)
    cflags = ' '.join(['-ffile-prefix-map=%s=/ioswitch/m2v-build' % work, '-ffile-prefix-map=%s=/ioswitch' % root])
    env = {
        'PATH': os.pathsep.join([str((root / args.cargo).parent), str((root / args.rustc).parent),
                                 xcode['XCODE_TOOLCHAIN'], '/usr/bin', '/bin', '/usr/sbin', '/sbin']),
        'HOME': str(work),
        'TMPDIR': os.environ.get('TMPDIR', str(work)),
        'CARGO_HOME': str(home),
        'CARGO_TARGET_DIR': str(work / 'target'),
        'CARGO_INCREMENTAL': '0',
        'CARGO_TERM_COLOR': 'never',
        'RUSTC': str(root / args.rustc),
        'RUSTC_WRAPPER': str(wrapper),
        'DEVELOPER_DIR': xcode['DEVELOPER_DIR'],
        'IPHONEOS_DEPLOYMENT_TARGET': '17.0',
        'MACOSX_DEPLOYMENT_TARGET': '14.0',
        'CFLAGS': cflags,
        'ZERO_AR_DATE': '1',
        'SOURCE_DATE_EPOCH': '0',
        'NMB_FIXTURE_AIR': str(root / args.fixture_air),
        'NMB_FC_AIR': str(root / args.fc_air),
    }
    if args.quick:
        env.update(CARGO_PROFILE_RELEASE_LTO='false', CARGO_PROFILE_RELEASE_CODEGEN_UNITS='16',
                   CARGO_PROFILE_RELEASE_DEBUG='0')
    command = [str(root / args.cargo), 'build', '--release', '--offline', '--locked',
               '--manifest-path', str(work / 'bridge/Cargo.toml'), '--target', args.target]
    result = subprocess.run(command, env=env, cwd=work / 'bridge', capture_output=True, text=True)
    if result.returncode:
        sys.exit('cargo failed (%d):\n%s%s' % (result.returncode, result.stdout[-6000:], result.stderr[-12000:]))
    shutil.copyfile(work / 'target' / args.target / 'release/libnative_metal_bridge.a', args.output)
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
