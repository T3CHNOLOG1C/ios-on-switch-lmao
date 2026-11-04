"""Build XNU's objects with make, as tree/scripts/build-xnu-persistent-data-v17-fast-active.sh did, and record the
kernel link instead of running it (the link needs our driver archives, which are compiled from this build's own
compile records: the link is the separate kernel_link action).

The XNU_* options (build-options.json) become exactly the make arguments that script passed; options it rejected
are rejected here too. make runs in the action's declared output (OBJROOT = --out, the CMake flow's tree/build/xnu/ios-obj) with:
  * -ffile-prefix-map so no sandbox path reaches an object (common.prefix_maps),
  * KERNEL_BUILD_DATE and a `whoami` printing the builder, so the version string is fixed,
  * LD = a recorder that writes the link command (link.json) and stops make there.
Every text file make leaves (compile records, link.filelist, ...) has the execution root replaced by @EXECROOT@,
so the tree is identical in every sandbox; dependency files are dropped.

usage: xnu_build.py --xcode-env F --xnu DIR --sdkroot DIR --options JSON --tree-root DIR --out DIR --owner-header F
                    --link-json F --log F --path-root P --bin-dir D --build-date S --builder S [--jobs N]
"""
import argparse
import json
import os
import shlex
import shutil
import stat
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

XNU_DARWIN = "23.6.0"
XNU_VERSION = "10063.141.1"
DEFAULT_EXTRA_CFLAGS = ("-Wno-reserved-identifier -Wno-pre-c11-compat -Wno-missing-designated-field-initializers "
                        "-Wno-suggest-override -Wno-unnecessary-virtual-specifier -Wno-suggest-destructor-override "
                        "-Wno-ms-bitfield-padding")
# Options the script read; anything else in build-options.json is a mistake.
KNOWN = {
    "XNU_SOURCE_CORECRYPTO14", "XNU_SOURCE_PTHREAD", "XNU_ARM_DT_PLATFORM", "XNU_ARMCPU", "XNU_NATIVE_ARCH_TIMER",
    "XNU_NATIVE_PAN", "XNU_SOURCE_MOCKFS", "XNU_DT_PANIC_REGION", "XNU_NATIVE_CS_HASH_CACHE", "XNU_CRYPTO_BOOTSTRAP",
    "XNU_EXTRA_LD_ARCHIVES", "XNU_EXTRA_CFLAGS", "JOBS", "XNU_IOSWITCH_DIAGNOSTICS", "XNU_BOOT_PROGRESS",
    "XNU_BOOTSTRAP", "XNU_NATIVE_AES", "XNU_TRUST_CACHE_RUNTIME", "XNU_SHENG_PMGR", "XNU_IOSWITCH_VERIFIED_ROOT",
    "XNU_VERIFIED_ROOT_PROBE", "XNU_OWNER_ROOT_AUDIT", "XNU_OWNER_ROOT_TRUST", "XNU_OWNER_CACHE_MOUNT",
    "XNU_OWNER_CACHE_STARTUP", "XNU_OWNER_BOOT_MANIFEST", "XNU_OWNER_UNION_SYSTEMOS", "XNU_OWNER_DATA_MOUNT",
    "XNU_OWNER_CRYPTEX_FILES", "XNU_OWNER_CACHE_CARRIER", "XNU_OWNER_CACHE_CARRIER_WRONG_UUID", "XNU_NATIVE_RAM_BLOCK",
    "XNU_OWNER_DATA_VOLUME_UUID", "XNU_NATIVE_VIRTIO_BLOCK", "XNU_NATIVE_VIRTIO_CONSOLE", "XNU_NATIVE_UFS",
    "XNU_NATIVE_LEGACY_DISPLAY", "XNU_NATIVE_MOBILE_DISPLAY", "XNU_NATIVE_OFFLINE_GPU", "XNU_NATIVE_AIR_INIT",
    "XNU_IOSWITCH_BLOCK_ARCHIVE", "XNU_VERIFIED_ROOT_ARCHIVE", "XNU_ZONE_PGZ_ALIGNMENT_PROBE", "XNU_ADRENO_FIRMWARE_PROBE",
    "XNU_ADRENO_PM4_PROBE", "XNU_SMMU4K_PROBE", "XNU_CONSTANT_UPLOADS_PROBE", "XNU_ARMVIRT_PLATFORM", "XNU_NATIVE_ADRENO",
}
# Options this port does not implement (their archives or generators are not part of this tree any more).
UNSUPPORTED = {"XNU_BOOT_PROGRESS", "XNU_NATIVE_UFS", "XNU_NATIVE_VIRTIO_BLOCK", "XNU_NATIVE_VIRTIO_CONSOLE",
               "XNU_NATIVE_OFFLINE_GPU", "XNU_NATIVE_MOBILE_DISPLAY", "XNU_ADRENO_FIRMWARE_PROBE", "XNU_ADRENO_PM4_PROBE",
               "XNU_SMMU4K_PROBE", "XNU_CONSTANT_UPLOADS_PROBE", "XNU_ARMVIRT_PLATFORM", "XNU_NATIVE_ADRENO"}


def flags(options):
    """(CFLAGS_EXTRA, [make variable assignments], {name: value} the link reads) for build-options.json."""
    unknown = set(options) - KNOWN
    if unknown:
        sys.exit("unknown XNU options: " + ", ".join(sorted(unknown)))

    def flag(name, default="0"):
        value = str(options.get(name, default))
        if value not in ("0", "1"):
            sys.exit("{} must be 0 or 1".format(name))
        return value

    for name in UNSUPPORTED:
        if flag(name) != "0":
            sys.exit("{}=1 is not supported by the Bazel build".format(name))
    if flag("XNU_BOOTSTRAP") != "0":
        sys.exit("XNU_BOOTSTRAP: the Bazel build always compiles first and links separately")
    verified_root = flag("XNU_IOSWITCH_VERIFIED_ROOT")
    diagnostics = flag("XNU_IOSWITCH_DIAGNOSTICS")
    corecrypto = flag("XNU_SOURCE_CORECRYPTO14")
    arm_dt = flag("XNU_ARM_DT_PLATFORM")
    if flag("XNU_ARMCPU") == "1" and (corecrypto != "1" or arm_dt != "1"):
        sys.exit("ArmCpu bring-up requires the real source provider and a real platform driver")
    if flag("XNU_SOURCE_PTHREAD") == "1" and corecrypto != "1":
        sys.exit("The current pthread source-link recipe requires XNU_SOURCE_CORECRYPTO14=1")
    crypto_mode = str(options.get("XNU_CRYPTO_BOOTSTRAP", "source"))
    if crypto_mode not in ("source", "prelinked"):
        sys.exit("XNU_CRYPTO_BOOTSTRAP must be source or prelinked")
    if crypto_mode == "source" and corecrypto == "1":
        sys.exit("XNU_CRYPTO_BOOTSTRAP=source needs seal-source-crypto-kernel.py, which the Bazel build does not run")
    args = []
    args.append("prng_random.o_CFLAGS_ADD=-UXNU_SOURCE_CORECRYPTO14" if crypto_mode == "prelinked"
                else "prng_random.o_CFLAGS_ADD=")
    personality = ""
    if flag("XNU_NATIVE_AES") == "1":
        if crypto_mode != "prelinked" or corecrypto != "1":
            sys.exit("NativeAES currently requires the qualified original AES/crypto prelink closure")
        personality = "-DXNU_NATIVE_AES=1"
    ram_block = flag("XNU_NATIVE_RAM_BLOCK")
    if ram_block == "1":
        if flag("XNU_NATIVE_AES") != "1":
            sys.exit("NativeRamBlock requires the qualified original storage/AES prelink closure")
        personality += " -DXNU_NATIVE_RAM_BLOCK=1"
    args.append("KernelConfigTables.cpo_CFLAGS_ADD=" + personality)
    args.append("startup.o_CFLAGS_ADD=-DXNU_NATIVE_TRUST_CACHE=" + flag("XNU_TRUST_CACHE_RUNTIME"))
    mockfs = flag("XNU_SOURCE_MOCKFS")
    if mockfs == "1" and (flag("XNU_ARMCPU") != "1" or flag("XNU_SOURCE_PTHREAD") != "1"):
        sys.exit("The offline executable-root fixture requires the real CPU/pthread components")
    args.append("vfs_conf.o_CFLAGS_ADD=-DMOCKFS=" + mockfs)
    args.append("ubc_subr.o_CFLAGS_ADD=-DXNU_NATIVE_CS_HASH_CACHE=" + flag("XNU_NATIVE_CS_HASH_CACHE"))
    extra = str(options.get("XNU_EXTRA_CFLAGS", DEFAULT_EXTRA_CFLAGS))
    probe = flag("XNU_VERIFIED_ROOT_PROBE")
    if probe == "1" and (corecrypto != "1" or crypto_mode != "prelinked"):
        sys.exit("The owner crypto probe requires the qualified original prelinked crypto provider")
    audit = flag("XNU_OWNER_ROOT_AUDIT")
    if audit == "1" and probe != "1":
        sys.exit("The root-binding audit requires real owner-verified block crypto")
    trust = flag("XNU_OWNER_ROOT_TRUST")
    if trust == "1" and (audit != "1" or verified_root != "1"):
        sys.exit("Owner root acceptance requires actual mounted-media binding and verified block storage")
    sheng_pmgr = flag("XNU_SHENG_PMGR")
    if sheng_pmgr == "1" and corecrypto != "1":
        sys.exit("The sheng PMGR instrument requires the original-component kernel configuration")
    legacy_display = flag("XNU_NATIVE_LEGACY_DISPLAY")
    args.append("bsd_init.o_CFLAGS_ADD=-DXNU_SHENG_PMGR={} -DXNU_VERIFIED_ROOT_PROBE={} -DXNU_OWNER_ROOT_AUDIT={} "
                "-DXNU_OWNER_ROOT_TRUST={} -DXNU_IOSWITCH_VERIFIED_ROOT={} -DXNU_ADRENO_FIRMWARE_PROBE=0 "
                "-DXNU_ADRENO_PM4_PROBE=0 -DXNU_SMMU4K_PROBE=0 -DXNU_CONSTANT_UPLOADS_PROBE=0 "
                "-DXNU_NATIVE_LEGACY_DISPLAY={} -DXNU_NATIVE_OFFLINE_GPU=0".format(
                    sheng_pmgr, probe, audit, trust, verified_root, legacy_display))
    args.append("IOUserClient.cpo_CFLAGS_ADD=-DXNU_NATIVE_MOBILE_DISPLAY=0 -DXNU_NATIVE_LEGACY_DISPLAY=" + legacy_display)
    cache_mount = flag("XNU_OWNER_CACHE_MOUNT")
    if cache_mount == "1" and trust != "1":
        sys.exit("The offline SystemOS mount requires real independent owner root enforcement")
    cache_startup = flag("XNU_OWNER_CACHE_STARTUP")
    if cache_startup == "1" and cache_mount != "1":
        sys.exit("Owner dyld startup selection requires authenticated actual cache mounting")
    manifest = flag("XNU_OWNER_BOOT_MANIFEST")
    if manifest == "1" and cache_startup != "1":
        sys.exit("Owner boot metadata requires real authenticated owner root/cache startup")
    args.append("IOKitBSDInit.cpo_CFLAGS_ADD=-DXNU_OWNER_BOOT_MANIFEST=" + manifest)
    data_mount = flag("XNU_OWNER_DATA_MOUNT")
    if data_mount == "1" and manifest != "1":
        sys.exit("Scratch Data requires authenticated original OS/owner metadata startup")
    cryptex_files = flag("XNU_OWNER_CRYPTEX_FILES")
    if cryptex_files == "1" and cache_startup != "1":
        sys.exit("Owner Cryptex file lookup requires authenticated mounted SystemOS startup")
    union = flag("XNU_OWNER_UNION_SYSTEMOS")
    if union == "1" and (cache_startup != "1" or cryptex_files != "0"):
        sys.exit("Owner union composition requires authenticated startup and no fallback-path experiment")
    air = flag("XNU_NATIVE_AIR_INIT")
    if air == "1" and (cache_mount != "1" or data_mount != "1" or trust != "1"):
        sys.exit("Offline AIR init requires the actual owner root, SystemOS and scratch Data path")
    args.append("kern_exec.o_CFLAGS_ADD=-DXNU_NATIVE_AIR_INIT={} -DXNU_OWNER_CACHE_MOUNT={} -DXNU_OWNER_CACHE_STARTUP={} "
                "-DXNU_OWNER_BOOT_MANIFEST={} -DXNU_OWNER_DATA_MOUNT={} -DXNU_NATIVE_RAM_BLOCK={} "
                "-DXNU_OWNER_CRYPTEX_FILES={} -DXNU_OWNER_UNION_SYSTEMOS={} -include @OWNER_DATA_HEADER@".format(
                    air, cache_mount, cache_startup, manifest, data_mount, ram_block, cryptex_files, union))
    carrier = flag("XNU_OWNER_CACHE_CARRIER")
    wrong = flag("XNU_OWNER_CACHE_CARRIER_WRONG_UUID")
    if (carrier == "1" and cache_startup != "1") or (wrong == "1" and carrier != "1"):
        sys.exit("Owner carrier policy requires authenticated owner startup; wrong UUID is an isolated negative control")
    args.append("vm_unix.o_CFLAGS_ADD=-DXNU_OWNER_CACHE_CARRIER={} -DXNU_OWNER_CACHE_CARRIER_WRONG_UUID={}".format(
        carrier, wrong))
    args.append("pe_init.o_CFLAGS_ADD=-DXNU_DT_PANIC_REGION=" + flag("XNU_DT_PANIC_REGION"))
    args.append("zalloc.o_CFLAGS_ADD=-DXNU_ZONE_PGZ_ALIGNMENT_PROBE=" + flag("XNU_ZONE_PGZ_ALIGNMENT_PROBE"))
    arch_timer = flag("XNU_NATIVE_ARCH_TIMER", flag("XNU_ARMCPU"))
    if arch_timer == "1" and flag("XNU_ARMCPU") != "1":
        sys.exit("Architectural timer entry requires the actual ArmCpu interrupt provider")
    args.append("start.o_SFLAGS_ADD=-DXNU_NATIVE_ARCH_TIMER={} -DIOSWITCH_XNU_DIAGNOSTICS={}".format(arch_timer, diagnostics))
    pan = flag("XNU_NATIVE_PAN")
    extra += " -DXNU_NATIVE_PAN=" + pan
    for name in ("copyio.o", "pcb.o", "platform_tests.o"):
        args.append(name + "_CFLAGS_ADD=" + ("-march=armv8.1-a" if pan == "1" else ""))
    if corecrypto == "1":
        extra += " -DXNU_SOURCE_CORECRYPTO14=1"
    extra += " -DXNU_BOOT_PROGRESS=0 -DXNU_IOSWITCH_VERIFIED_ROOT=" + verified_root
    extra += " -DIOSWITCH_XNU_DIAGNOSTICS=" + diagnostics + " -I@TREE@/diagnostics/ioswitch"
    owner_header = "/* Scratch Data IOKit selection disabled. */\n"
    if data_mount == "1" and ram_block == "1":
        import uuid
        try:
            value = str(uuid.UUID(str(options.get("XNU_OWNER_DATA_VOLUME_UUID", "")))).upper()
        except ValueError:
            sys.exit("Set XNU_OWNER_DATA_VOLUME_UUID to the actual observed scratch APFS volume UUID")
        owner_header = '#define XNU_OWNER_DATA_VOLUME_UUID "' + value + '"\n'
    return extra, args, owner_header


def main():
    parser = argparse.ArgumentParser()
    for name in ("xcode-env", "xnu", "sdkroot", "options", "tree-root", "out", "owner-header", "link-json", "log",
                 "path-root", "bin-dir", "build-date", "builder"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--jobs", default=str(os.cpu_count() or 8))
    args = parser.parse_args()
    here = common.execroot()
    xcode = common.xcode_env(args.xcode_env)
    options = json.loads(Path(args.options).read_text())
    extra, make_args, owner_header = flags(options)
    objroot = Path(args.out).absolute()
    owner = Path(args.owner_header).absolute()
    owner.write_text(owner_header)
    tree = Path(args.tree_root).absolute()
    xnu = Path(args.xnu).absolute()
    extra = extra.replace("@TREE@", str(tree))
    make_args = [a.replace("@OWNER_DATA_HEADER@", str(owner)) for a in make_args]
    extra += " " + " ".join(common.prefix_maps(args.path_root, args.bin_dir, args.xnu))
    # UBSan's source locations (signed-overflow checks left in -O0 objects: vm_tests.o) ignore -ffile-prefix-map; keep
    # only the last three path components there ("osfmk/vm/vm_tests.c"), the same in every checkout and sandbox.
    extra += " " + common.UBSAN_PATHS
    # The XNU build script's mkdir of build/xnu/obj/EXPORT_HDRS/libsa never reached the include path the compile
    # records name (ios-obj/EXPORT_HDRS/libsa, -Werror,-Wmissing-include-dirs); the CMake flow's tree had it from an
    # earlier run. Same directory here.
    (objroot / "EXPORT_HDRS/libsa").mkdir(parents=True, exist_ok=True)
    # Bazel keeps no empty directory in a tree artifact: a marker file keeps this one (and every later -I of it).
    (objroot / "EXPORT_HDRS/libsa/.keep").write_text("")
    with tempfile.TemporaryDirectory() as scratch, open(args.log, "w") as log:
        scratch = Path(scratch)
        bin_dir = scratch / "bin"
        bin_dir.mkdir()
        whoami = bin_dir / "whoami"
        whoami.write_text("#!/bin/sh\necho {}\n".format(shlex.quote(args.builder)))
        recorder = bin_dir / "record-link"
        recorder.write_text("#!/bin/sh\n{} -I -c 'import json, os, sys; json.dump({{\"cwd\": os.getcwd(), \"argv\": "
                            "sys.argv[2:]}}, open(sys.argv[1], \"w\"))' {} \"$@\"\nexit 3\n".format(
                                shlex.quote(sys.executable), shlex.quote(str(scratch / "link.json"))))
        for tool in (whoami, recorder):
            tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
        python_bin = str(Path(sys.executable).parent)
        env = common.environment(xcode, extra_path=[str(bin_dir), python_bin],
                                 KERNEL_SDKROOT=str(Path(args.sdkroot).absolute()),
                                 XNU_AVAILABILITY_PL=str(Path(args.sdkroot).absolute() / "usr/local/libexec/availability.pl"),
                                 KERNEL_BUILD_DATE=args.build_date,
                                 # XNU's makefiles sort the export lists into __LINKINFO,__symbolsets with the locale's
                                 # collation: pinned to the one the hardware-tested kernels were built under.
                                 LC_ALL=common.BUILD_LOCALE, LANG=common.BUILD_LOCALE)
        command = [
            "make", "-C", xnu, "build", "SDKROOT=iphoneos", "ARCH_CONFIGS=ARM64", "MACHINE_CONFIGS=BCM2837",
            "KERNEL_CONFIGS=DEVELOPMENT", "ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64",
            "LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY", "ARM64_LINK_ADDR=0xfffffff007004000",
            "ARM64_PLKSEG_ADDR=0xfffffff004004000",
            "DEPLOYMENT_TARGET_FLAGS=-miphoneos-version-min=17.0 -DXNU_TARGET_OS_IOS",
            "DEPLOYMENT_LINKER_FLAGS=-Wl,-ios_version_min,17.0", "RC_DARWIN_KERNEL_VERSION=" + XNU_DARWIN,
            "RC_ProjectSourceVersion=" + XNU_VERSION, "BUILD_LTO=0", "BOUND_CHECKS=0", "BUILD_WERROR=0",
            "MAKEJOBS=-j" + args.jobs, "CFLAGS_EXTRA=" + extra, *make_args,
            "LD_KERNEL_ARCHIVES=@KERNEL_ARCHIVES@", "XNU_EXTERNAL_KERNEL_ARCHIVES=", "LD=" + str(recorder),
            "OBJROOT=" + str(objroot), "SYMROOT=" + str(scratch / "ios-sym"), "DSTROOT=" + str(scratch / "ios-dst"),
        ]
        import subprocess
        result = subprocess.run([str(c) for c in command], env=env, stdout=log, stderr=subprocess.STDOUT)
        link = scratch / "link.json"
        if not link.exists():
            log.flush()
            sys.exit("XNU build failed before its link (exit {}): see {}".format(result.returncode, args.log))
        recorded = json.loads(link.read_text())
    # Make the tree sandbox-independent: drop make's dependency files and the kernel the recorder never wrote, and
    # put @EXECROOT@ where the execution root appears in text files.
    marker = here.encode()
    escaped = []
    # XNU's host build tools (config, kextsymboltool, ...): built with the host compiler for this build only.
    shutil.rmtree(objroot / "SETUP", ignore_errors=True)
    for path in sorted(objroot.rglob("*")):
        if path.is_symlink():
            target = os.readlink(path)
            if target.startswith(here):
                path.unlink()
                os.symlink(os.path.relpath(target, path.parent), path)
            continue
        if not path.is_file():
            continue
        if path.suffix == ".d":
            path.unlink()
            continue
        data = path.read_bytes()
        if marker in data:
            if b"\0" in data[:4096] and path.suffix in (".o", ".cpo", ".a"):
                at = data.index(marker)
                record = path.with_name(path.name + ".json")
                found = [a for a in json.loads(record.read_text().rstrip().rstrip(","))["arguments"]
                         if "prefix-map" in a or a.startswith("-O") or a.startswith("-g")] if record.exists() else []
                escaped.append("{}: ...{!r}... flags {}".format(path.relative_to(objroot), data[max(0, at - 80):at + 300], found))
                continue
            path.write_bytes(data.replace(marker, common.EXECROOT.encode()))
    if escaped:
        sys.exit("objects name the execution root (a compile escaped the prefix maps):\n" + "\n".join(escaped[:20]))
    recorded = json.loads(json.dumps(recorded).replace(here, common.EXECROOT))
    recorded["ld"] = xcode["XCODE_TOOLCHAIN"] + "/clang++"
    Path(args.link_json).write_text(json.dumps(recorded, indent=1) + "\n")


if __name__ == "__main__":
    main()
