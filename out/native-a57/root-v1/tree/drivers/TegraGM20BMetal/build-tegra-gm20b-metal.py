#!/usr/bin/env python3
"""Build the TegraGM20B Metal plugin bundle for iOS 17 (arm64) and its trust-cache module.

usage: build-tegra-gm20b-metal.py [<outdir>] [--m2v <metal2vulkan checkout>] [--nvk <libvulkan_nouveau.dylib>] [--seed <dir>] [--reset-m2v]
                                  [--nvk-host <libvulkan_nouveau.dylib>] [--qc <default.metallib>] [--libs <store>] [--no-precompile]
                                  [--host]

  <outdir>  default out/gpu (the path package_raw_boot.py and stage-tegra-gm20b-metal.py read)
  --m2v     steelbrain/metal2vulkan at 720ef1ce (git clone, checkout); the patches patches/series lists (paths from drivers/),
            else drivers/NativeMetal/bridge/patches then this directory's patches/m2v: a tree holding the first k of them gets
            the rest applied, and the tree must then be exactly all of them. Default out/gpu-artifacts/m2v-src/metal2vulkan.
  --reset-m2v  first put the checkout back to 720ef1ce (its tracked files, and no untracked ones): for a tree that holds an
            earlier series, which is no first part of this one.
  --nvk     Mesa NVK for iOS with the nvkmd_darwin backend (Mesa 77452b90 + nvk-darwin.patch, build-nvk-ios.sh). Default
            <outdir>/nvk/libvulkan_nouveau.dylib.
  --seed    a shader cache seed to ship when none is generated (below): a cache directory laid out as m2v's, <dir>/{SEED,v1/...}
            (a device's m2v cache taken after a boot, v1/ as the device wrote it, SEED one line naming it). Only a seed made with
            the same m2v sources and NVK dylib hits. No default: without a generated seed or --seed, none is shipped.
  --nvk-host  the host twin of --nvk (precompile/build-nvk-host.sh on the same Mesa checkout): the same sources built for this
            Mac, so NAK compiles on it exactly as on the device. Its <dylib>.source must equal the iOS dylib's (precompile/
            nvk-source.sh: the checkout's git tree and the -Dnvk-build-id), else no seed is generated: a twin of other sources
            under the device's pipeline cache UUID would hand the device wrong code. Default <nvk dir>/libvulkan_nouveau.host.dylib.
  --qc      QuartzCore.framework/default.metallib of 21H461 (SHA-256 checked). Default out/gpu-artifacts/QuartzCore-21H461.default.metallib.
  --libs    the library store of the precompile (precompile/libraries.py: the 21H461 OS volume's and cryptex's metallibs, the
            libraries apps made from bytes and app bundles' metallibs harvested from the device). Default out/gpu-artifacts/m2v-libs;
            without it, only QuartzCore's lines are replayed.
  --no-precompile  no generated seed: only --seed's.
  --host    only the bridge, for this Mac (aarch64-apple-darwin), for the host simulation (hostsim/build.sh): prints its path.

The series (patches/series): AdrenoMetal's feat/ipados27-gpu-parity draw-path patches (patches/parity, as MaliMetal applies them on
this pin and OS build), then ours (patches/m2v): framebuffer fetch and raster order groups without rasterization-order attachment
access or a pixel interlock (Maxwell has neither: unordered input attachments, a barrier before each fetching draw that follows
another), Core Image's air.ci_imageblock role and pointer layouts, extended-range blends over unordered input attachments,
MaliMetal's YCbCr conversion memo and filter, the pooled block cap and fresh memory left unzeroed.

Same recipe as vendor/unixnu/scripts/build-native-metal-bridge.py + build-adreno-metal-plugin.py (UniXNU-sheng):
  * the bridge (drivers/NativeMetal/bridge/NativeMetalBridge.rs over metal2vulkan) as a static library, cargo +1.98.0 for
    aarch64-apple-ios, with the runtime's AIR fixtures assembled by llvm-as. Built from a copy with one change: the device
    accepts unordered framebuffer fetch (FramebufferFetchPolicy::AcceptUnordered). NVK on Maxwell has neither
    rasterization-order attachment access nor fragment shader interlock, so the default policy refuses every QuartzCore
    pipeline that reads its framebuffer (seen on the host simulation with the 21H461 default.metallib);
  * TegraGM20BMetalPlugin.mm + drivers/NativeMetal/NativeBridgeMetalFrontend.mm linked with it as an MH_BUNDLE;
  * libvulkan_nouveau.dylib copied into the bundle and signed first, then the bundle, both ad hoc with 4 KiB pages (this
    kernel's page size), so the bundle's seal covers the final dylib.

Output in <outdir>:
  ngpu/addons/TegraGM20BMetal.bundle   the tree for the root of the Data volume (stage-tegra-gm20b-metal.py). With
                                       sheng-gpu-addon=1 the kernel mounts /private/var/ngpu/addons read-only over
                                       /System/Library/Extensions before launchd (kern_exec.c native_gpu_addon_prepare,
                                       called from native_owner_data_prepare), where Metal looks for MetalPluginName.
  ngpu/addons/TegraGM20BMetal.m2v-seed/shared   the shader cache seed of every process, generated for this build
                                       (precompile/precompile.mm: every function specialization and render pipeline state of
                                       precompile/corpus.log, Core Animation's from the device journals of every process
                                       (precompile/harvest.py), made through this build's frontend and m2v built for this Mac
                                       (the cache's build directory hashes the sources) and the NVK twin with the iOS dylib's
                                       pipeline cache identity, over a GM20B mock: the translations and NAK's driver records the
                                       device would write, byte for byte), else --seed's. The plugin names it to every process's
                                       runtime (METAL2VULKAN_SHADER_CACHE_SEED, m2v patch 0108): a name the process's own cache
                                       misses is read there, in place and read only; nothing is copied. SEED names the build,
                                       the NVK identity and the contents (the plugin logs it).
  ngpu/addons/TegraGM20BMetal.raw/pack.bin   the raw GM20B path's pipeline pack for the render server (raw/pack/rawpack.py: Core
                                       Animation's corpus states made through this build's frontend, host bridge and NVK twin over a
                                       mock GM20B, read off the decoded push stream), stamped with this build's stack key, which the
                                       plugin is compiled with (TEGRA_RAW_STACK): a pack of another build is refused on the device.
                                       Made with the seed (it needs the same host bridge and twin); without one the render server
                                       keeps the m2v device. The plugin uses it when staged beside the bundle.
  precompile/                          the host bridge's tool, the twin as the device's identity, the generated cache
  TegraGM20BMetal.trustcache   one version-2 trust-cache module with both CDHashes (package_raw_boot.py appends it to the
                               static trust-cache bundle; every rebuild changes the CDHashes, so repackage the boot entry)
  trust.json                   the CDHashes
"""
import hashlib
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import time
import uuid
from pathlib import Path

DRV = Path(__file__).resolve().parent
TREE = DRV.parents[1]
ROOT = TREE.parents[3]   # the project root; TREE is out/native-a57/root-v1/tree
METAL = TREE / 'drivers/NativeMetal'
PIN = '720ef1ce9ae2a2068999a8675717418b9416283e'
SERIES = DRV / 'patches/series'
PATCHES = ([TREE / 'drivers' / line.strip() for line in SERIES.read_text().splitlines() if line.strip() and not line.startswith('#')]
           if SERIES.is_file() else
           sorted((METAL / 'bridge/patches').glob('*.patch')) + sorted((DRV / 'patches/m2v').glob('*.patch')))
LLVM = Path('/opt/homebrew/opt/llvm/bin')
NAME = 'TegraGM20BMetal'
PRECOMPILE = DRV / 'precompile'
RAW = DRV / 'raw'   # the raw GM20B path: runtime/ (compiled into the plugin), pack/ (the pack generator)
QC_SHA256 = '606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9'   # 21H461 QuartzCore.framework/default.metallib
CONTENT = '529a20fd270ababa3e5ec2fe4d1674d93739ba5bcd61861674b4fb6db349af9f'     # m2v's content directory (shader_cache/store.rs)


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit('FAILED: %s\n%s%s' % (' '.join(map(str, cmd))[:400], r.stdout[-4000:], r.stderr[-6000:]))
    return r.stdout + r.stderr


def option(name, default):
    if name not in sys.argv[1:]:
        return default
    if name not in sys.argv[1:-1]:
        sys.exit(__doc__)
    return Path(sys.argv[sys.argv.index(name) + 1]).resolve()


def tree_of(m2v, index, patches=None):
    """The git tree of PIN with `patches` applied (to an index only), or of the working tree (tracked and untracked files
    .gitignore keeps) when `patches` is None."""
    env = dict(os.environ, GIT_INDEX_FILE=str(index))
    run(['git', '-C', m2v, 'read-tree', PIN], env=env)
    if patches is None:
        run(['git', '-C', m2v, 'add', '-A'], env=env)
    for patch in patches or []:
        run(['git', '-C', m2v, 'apply', '--cached', patch], env=env)
    tree = run(['git', '-C', m2v, 'write-tree'], env=env).strip()
    index.unlink()
    return tree


def prefix_trees(m2v, index):
    """The git tree of PIN with each first part of PATCHES applied (to an index only): k patches at [k]."""
    env = dict(os.environ, GIT_INDEX_FILE=str(index))
    run(['git', '-C', m2v, 'read-tree', PIN], env=env)
    trees = [run(['git', '-C', m2v, 'write-tree'], env=env).strip()]
    for patch in PATCHES:
        run(['git', '-C', m2v, 'apply', '--cached', patch], env=env)
        trees.append(run(['git', '-C', m2v, 'write-tree'], env=env).strip())
    index.unlink()
    return trees


def bridge(m2v, out, target='aarch64-apple-ios', quick=False):
    """The bridge's static library for `target`; `quick`: without LTO and debug info and without --reset-m2v (the precompile's
    host build, after the device's: the cache keys hash the sources, not the code, and a reset would touch every source)."""
    if run(['git', '-C', m2v, 'rev-parse', 'HEAD']).strip() != PIN:
        sys.exit('metal2vulkan is not at ' + PIN)
    if '--reset-m2v' in sys.argv[1:] and not quick:
        run(['git', '-C', m2v, 'checkout', '-q', '-f', 'HEAD', '--', '.'])
        run(['git', '-C', m2v, 'clean', '-q', '-f', '-d'])
    out.mkdir(parents=True, exist_ok=True)
    index = out / '.m2v-index'
    actual = tree_of(m2v, index)
    trees = prefix_trees(m2v, index)
    applied = next((k for k in range(len(PATCHES), -1, -1) if trees[k] == actual), None)
    if applied is None:
        sys.exit('%s is not %s with a first part of %s (run with --reset-m2v for a tree holding an earlier series)' %
                 (m2v, PIN, ', '.join(p.name for p in PATCHES)))
    for patch in PATCHES[applied:]:
        run(['git', '-C', m2v, 'apply', patch])
    if tree_of(m2v, index) != trees[-1]:   # all applied, nothing else
        sys.exit('%s is not %s with exactly %s' % (m2v, PIN, ', '.join(p.name for p in PATCHES)))
    work = out / 'bridge'
    (work / 'src').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(METAL / 'bridge/NativeMetalBridge.rs', work / 'src/NativeMetalBridge.rs')
    objects = (METAL / 'bridge/NativeMetalObjects.rs').read_text()
    anchor = '        let mut options = DeviceOptions::default();\n'
    if objects.count(anchor) != 1:
        sys.exit('NativeMetalObjects.rs changed: no single device-options line to amend')
    (work / 'src/NativeMetalObjects.rs').write_text(objects.replace(anchor, anchor +
        '        options.policy.framebuffer_fetch = metal2vulkan::device::FramebufferFetchPolicy::AcceptUnordered;\n'))
    fixtures = m2v / 'crates/metal2vulkan-validation/fixtures/public'
    run([LLVM / 'llvm-as', '-o', work / 'runtime_examples.air', fixtures / 'examples/runtime_examples.ll'])
    run([LLVM / 'llvm-as', '-o', work / 'function_constant.air', fixtures / 'kernel_function_constant_gate_fold.ll'])
    (work / 'Cargo.toml').write_text('\n'.join([
        '[package]', 'name = "native-metal-bridge"', 'version = "0.1.0"', 'edition = "2024"', 'rust-version = "1.98"', '[workspace]',
        '[lib]', 'crate-type = ["staticlib"]', 'path = ' + json.dumps(str(work / 'src/NativeMetalBridge.rs')),
        '[dependencies]',
        'metal2vulkan = { path = ' + json.dumps(str(m2v / 'crates/metal2vulkan')) + ', features = ["ash"] }',
        'metal2vulkan-runtime = { path = ' + json.dumps(str(m2v / 'crates/metal2vulkan-runtime')) + ' }',
        'libc = "0.2"', 'sha2 = { version = "0.10", features = ["asm"] }',
        # One codegen unit and fat LTO: the draw path crosses metal2vulkan and metal2vulkan-runtime at every step (r5-m2vfast:
        # -11% encode CPU on the host simulation's CA frame; no shader cache key changes, the keys hash the sources).
        '[profile.release]', 'panic = "unwind"', 'debug = 1', 'codegen-units = 1', 'lto = "fat"', '']))
    env = dict(os.environ, NMB_FIXTURE_AIR=str(work / 'runtime_examples.air'),
               NMB_FC_AIR=str(work / 'function_constant.air'), IPHONEOS_DEPLOYMENT_TARGET='17.0')
    if quick:
        env.update(CARGO_PROFILE_RELEASE_LTO='false', CARGO_PROFILE_RELEASE_CODEGEN_UNITS='16', CARGO_PROFILE_RELEASE_DEBUG='0')
    run(['cargo', '+1.98.0', 'build', '--release', '--manifest-path', work / 'Cargo.toml', '--target', target,
         '--target-dir', out / 'm2v-target'], env=env)
    return out / 'm2v-target' / target / 'release/libnative_metal_bridge.a'


def sign(path, identifier):
    run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', identifier, path])
    run(['codesign', '--verify', '--strict', path])
    detail = run(['codesign', '--display', '--verbose=4', path])
    if not re.search(r'^Page size=4096$', detail, re.M) or not re.search(r'^Hash type=sha256', detail, re.M):
        sys.exit('%s: expected a SHA-256 signature with 4 KiB pages' % path)
    return re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)[1]


def arm64(path):
    magic, cputype, subtype = struct.unpack_from('<3I', path.read_bytes())
    if (magic, cputype, subtype & 0xffffff) != (0xfeedfacf, 0x100000c, 0):
        sys.exit('%s: expected a plain arm64 (subtype 0) Mach-O' % path)


def trust_module(cdhashes):
    """One version-2 module (vendor/unixnu/scripts/make-owner-trust-bundle.py): u32 version, uuid, u32 count, then per entry
    the 20-byte CDHash, hash type 2 (SHA-256), flags 0, constraint category 0, reserved 0, in ascending CDHash order."""
    hashes = sorted(bytes.fromhex(h) for h in cdhashes)
    identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'org.ioswitch.tegra-gm20b-metal:' + ''.join(h.hex() for h in hashes))
    return struct.pack('<I16sI', 2, identifier.bytes, len(hashes)) + b''.join(h + bytes([2, 0, 0, 0]) for h in hashes)


def raw_stack(nvk):
    """The raw path's stack key: a hash of what the pack's contents depend on, known before the plugin is compiled. The NVK dylib
    (its context init, NAK's programs and register values), m2v (the patch series and the bridge sources: the binding rules), the
    frontend (the journal's state keys) and the raw runtime and pack generator. 64 bits, as hex."""
    digest = hashlib.sha256(nvk.read_bytes())
    for path in [*PATCHES, METAL / 'bridge/NativeMetalBridge.rs', METAL / 'bridge/NativeMetalObjects.rs', METAL / 'NativeBridgeMetalFrontend.mm',
                 *sorted(p for p in RAW.rglob('*') if p.is_file() and p.suffix in ('.h', '.c', '.cpp', '.mm', '.py', '.inc'))]:
        digest.update(str(path.name).encode() + b'\0' + path.read_bytes())
    return digest.hexdigest()[:16]


def rawpack(out, library, twin, qc, corpus, stack):
    """The raw path's pack beside the bundle (ngpu/addons/TegraGM20BMetal.raw/pack.bin) from the precompile's host bridge and twin:
    None, or why there is none (the render server then keeps the m2v device)."""
    target = out / 'ngpu/addons' / (NAME + '.raw')
    shutil.rmtree(target, ignore_errors=True)
    work = out / 'raw-pack'
    shutil.rmtree(work, ignore_errors=True)
    r = subprocess.run([sys.executable, str(RAW / 'pack/rawpack.py'), '--bridge', str(library), '--nvk', str(twin), '--qc', str(qc),
                        '--corpus', str(corpus), '--out', str(work), '--stack', stack, '--clean'], capture_output=True, text=True,
                       env=dict(os.environ, RAWPACK_TREE=str(TREE)))
    if r.returncode or not (work / 'pack.bin').is_file():
        return 'the pack generator failed: %s' % (r.stdout + r.stderr)[-2000:]
    print('\n'.join(l for l in r.stdout.splitlines() if not l.startswith('  skipped')))
    target.mkdir(parents=True)
    shutil.copyfile(work / 'pack.bin', target / 'pack.bin')
    for path in [target, target / 'pack.bin']:
        path.chmod(0o755 if path.is_dir() else 0o644)
    return None


def lc_uuid(path, value=None):
    """The LC_UUID of the thin 64-bit Mach-O at `path`, after setting it to `value` (16 bytes) when given."""
    data = bytearray(path.read_bytes())
    magic, _, _, _, ncmds = struct.unpack_from('<5I', data)
    at = 32
    for _ in range(ncmds if magic == 0xfeedfacf else 0):
        cmd, size = struct.unpack_from('<2I', data, at)
        if cmd == 0x1b:   # LC_UUID
            if value is not None:
                data[at + 8:at + 24] = value
                path.write_bytes(data)
            return bytes(data[at + 8:at + 24])
        at += size
    sys.exit('%s: no LC_UUID' % path)


def precompile(m2v, out, nvk, stack):
    """The shader cache the device would write with this build for the corpus's states (the docstring's m2v-seed), made on this
    Mac: its v1/ and SEED name, or None and why not. One cache serves every process: m2v's names hash the build, the key and
    the device's profile, none of them per process."""
    twin = option('--nvk-host', nvk.with_name(nvk.stem + '.host.dylib'))
    qc = option('--qc', ROOT / 'out/gpu-artifacts/QuartzCore-21H461.default.metallib')
    if not twin.is_file():
        return None, 'no host twin of the NVK dylib at %s (--nvk-host; precompile/build-nvk-host.sh)' % twin
    sources = [Path(str(p) + '.source') for p in (nvk, twin)]
    if not all(p.is_file() for p in sources) or sources[0].read_text() != sources[1].read_text():
        return None, '%s and %s missing or different: the twin is not of the iOS dylib\'s sources' % tuple(sources)
    if not qc.is_file() or hashlib.sha256(qc.read_bytes()).hexdigest() != QC_SHA256:
        return None, '%s is not 21H461\'s QuartzCore default.metallib (--qc)' % qc
    work = out / 'precompile'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    began = time.monotonic()
    library = bridge(m2v, out, 'aarch64-apple-darwin', quick=True)
    bridged = time.monotonic()
    run(['xcrun', 'clang', '-O1', '-Wall', '-dynamiclib', '-I', TREE / 'drivers/TegraGM20B', PRECOMPILE / 'mock_iokit.c',
         '-framework', 'IOKit', '-framework', 'CoreFoundation', '-install_name', '@rpath/libmock_iokit.dylib',
         '-o', work / 'libmock_iokit.dylib'])
    run(['xcrun', 'clang++', '-O2', '-std=c++17', '-fobjc-arc', '-Wno-deprecated-declarations', '-I', METAL, '-x', 'objective-c++',
         PRECOMPILE / 'precompile.mm', METAL / 'NativeBridgeMetalFrontend.mm', '-x', 'none', library, '-L', work, '-lmock_iokit',
         '-Wl,-rpath,' + str(work), '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
         '-framework', 'IOKit', '-o', work / 'precompile'])
    # The twin with the device's pipeline cache identity: NVK hashes the -Dnvk-build-id when the build had one (both builds
    # had the same, .source says), else its own LC_UUID, which the twin then takes from the iOS dylib.
    dylib = work / 'libvulkan_nouveau.dylib'
    shutil.copyfile(twin, dylib)
    pinned = any(line.split()[1:] for line in sources[0].read_text().splitlines() if line.startswith('nvk-build-id'))
    if not pinned:
        lc_uuid(dylib, lc_uuid(nvk))
    run(['codesign', '--force', '--sign', '-', dylib])
    cache = work / 'cache'
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('NAK_', 'NVK_', 'MESA_', 'NMB_', 'M2V_', 'METAL2VULKAN_', 'TEGRA_', 'MTL_'))}
    env['MESA_SHADER_CACHE_DISABLE'] = 'true'
    # The corpus with each function's library: the journal's lib=, or for a line from before the journal named libraries, the
    # one library of the store holding its name (libraries.py resolve; QuartzCore's names stay QuartzCore's).
    store = option('--libs', ROOT / 'out/gpu-artifacts/m2v-libs')
    corpus = PRECOMPILE / 'corpus.log'
    if (store / 'names.json').is_file():
        corpus = work / 'corpus.log'
        print(run([sys.executable, PRECOMPILE / 'libraries.py', store, 'resolve', PRECOMPILE / 'corpus.log', corpus]).rstrip())
    else:
        print('precompile: no library store at %s (--libs; precompile/libraries.py index): QuartzCore\'s lines alone' % store)
    linked = time.monotonic()
    why = rawpack(out, library, dylib, qc, PRECOMPILE / 'corpus.log', stack)
    print('raw pack:', why or 'made, stack %s' % stack)
    made = run([work / 'precompile', '--libs', store, dylib, cache, qc, corpus], env=env)
    print('\n'.join(line for line in made.splitlines() if line.startswith('library ')))
    print(next(line for line in made.splitlines() if line.startswith('precompile:')),
          '(host bridge %.0f s, tool and twin %.0f s, run %.0f s)' % (bridged - began, linked - bridged, time.monotonic() - linked))
    builds = sorted(p.name for p in (cache / 'v1').iterdir() if p.name != CONTENT)
    records = sorted((cache / 'v1' / CONTENT).glob('*/*'))
    if len(builds) != 1 or not records:
        return None, 'the precompile made no single build directory and records: %s' % builds
    digest = hashlib.sha256()
    for path in sorted((cache / 'v1').rglob('*')):
        if path.is_file():
            digest.update(str(path.relative_to(cache)).encode() + b'\0' + path.read_bytes())
    pipeline_uuid = records[0].read_bytes()[20:36].hex()   # the record's identity: vendor, device, driver, pipelineCacheUUID
    return (cache / 'v1', 'generated m2v-build %s nvk %s content %s' % (builds[0][:8], pipeline_uuid[:8], digest.hexdigest()[:8])), None


def main():
    args = [a for i, a in enumerate(sys.argv[1:], 1)
            if not a.startswith('--') and sys.argv[i - 1] not in ('--m2v', '--nvk', '--seed', '--nvk-host', '--qc', '--libs')]
    if len(args) > 1:
        sys.exit(__doc__)
    out = Path(args[0]).resolve() if args else ROOT / 'out/gpu'
    m2v = option('--m2v', ROOT / 'out/gpu-artifacts/m2v-src/metal2vulkan')
    if '--host' in sys.argv[1:]:
        print(bridge(m2v, out, 'aarch64-apple-darwin'))
        return
    nvk = option('--nvk', out / 'nvk/libvulkan_nouveau.dylib')
    if not nvk.is_file():
        sys.exit('no NVK dylib at %s (--nvk)' % nvk)
    library = bridge(m2v, out)

    extensions = out / 'ngpu/addons'
    bundle = extensions / (NAME + '.bundle')
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir(parents=True)
    sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']).strip()
    exe = bundle / NAME
    stack = raw_stack(nvk)
    run(['xcrun', 'clang++', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O2', '-g', '-Wall', '-Wextra', '-Werror',
         '-fstack-protector-strong', '-std=c++17', '-fobjc-arc', '-Wno-deprecated-declarations', '-bundle', '-I', METAL,
         '-I', RAW / 'runtime', '-I', TREE / 'drivers/TegraGM20B', '-DTEGRA_RAW_STACK=0x%sull' % stack,
         '-x', 'objective-c++', DRV / 'TegraGM20BMetalPlugin.mm', METAL / 'NativeBridgeMetalFrontend.mm', RAW / 'runtime/RawMetal.mm',
         '-x', 'c++', RAW / 'runtime/raw_gpu.cpp', RAW / 'runtime/raw_kernel.cpp', RAW / 'runtime/raw_pack.cpp', '-x', 'none', library,
         '-Wl,-dead_strip', '-lobjc', '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
         '-framework', 'IOKit', '-o', exe])
    run(['strip', '-x', exe])
    shutil.rmtree(out / (NAME + '.dSYM'), ignore_errors=True)
    if (bundle / (NAME + '.dSYM')).exists():   # the Rust bridge's debug map lands next to the executable
        shutil.move(bundle / (NAME + '.dSYM'), out / (NAME + '.dSYM'))
    arm64(exe)

    dylib = bundle / 'libvulkan_nouveau.dylib'
    shutil.copyfile(nvk, dylib)
    run(['strip', '-x', dylib])
    arm64(dylib)
    nvk_cdhash = sign(dylib, 'org.ioswitch.nvk')
    (bundle / 'Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier='org.ioswitch.' + NAME, CFBundleExecutable=NAME, CFBundlePackageType='BNDL',
        CFBundleVersion='1', CFBundleShortVersionString='1', NSPrincipalClass='TegraGM20BMetalDevice',
        MinimumOSVersion='17.0'), fmt=plistlib.FMT_XML))
    plugin_cdhash = sign(bundle, 'org.ioswitch.' + NAME)
    for path in [extensions.parent, extensions, bundle, *bundle.rglob('*')]:
        path.chmod(0o755 if path.is_dir() or path in (exe, dylib) else 0o644)

    seed_out = extensions / (NAME + '.m2v-seed')
    shutil.rmtree(seed_out, ignore_errors=True)
    shared = seed_out / 'shared'
    generated, why = (None, '--no-precompile') if '--no-precompile' in sys.argv[1:] else precompile(m2v, out, nvk, stack)
    given = option('--seed', None)
    if generated:
        shutil.copytree(generated[0], shared / 'v1', copy_function=shutil.copyfile)
        (shared / 'SEED').write_text(generated[1] + '\n')
        print('seed shared:', generated[1])
    elif given:
        if not (given / 'SEED').is_file() or not (given / 'v1').is_dir():
            sys.exit('%s: a seed needs SEED and v1/' % given)
        shutil.copytree(given / 'v1', shared / 'v1', ignore=shutil.ignore_patterns('.*'), copy_function=shutil.copyfile)
        shutil.copyfile(given / 'SEED', shared / 'SEED')
        print('seed shared: not generated (%s), from %s: %s' % (why, given, (shared / 'SEED').read_text().strip()))
    else:
        print('seed: none: not generated:', why)
    for path in [seed_out, *seed_out.rglob('*')] if seed_out.is_dir() else []:
        path.chmod(0o755 if path.is_dir() else 0o644)

    (out / (NAME + '.trustcache')).write_bytes(trust_module([plugin_cdhash, nvk_cdhash]))
    (out / 'trust.json').write_text(json.dumps(dict(plugin_cdhash=plugin_cdhash, nvk_cdhash=nvk_cdhash,
                                                    plugin_bytes=exe.stat().st_size, nvk_bytes=dylib.stat().st_size), indent=2) + '\n')
    raw = extensions / (NAME + '.raw/pack.bin')
    print('built', bundle, 'plugin', plugin_cdhash, 'nvk', nvk_cdhash, 'seed', seed_out if seed_out.exists() else 'none',
          'raw pack', raw if raw.exists() else 'none (the render server keeps m2v)')


if __name__ == '__main__':
    main()
