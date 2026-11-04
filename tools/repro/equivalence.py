#!/usr/bin/env python3
"""Compare the Bazel build with a CMake-flow build of the same sources, in three layers.

usage: equivalence.py <cmake checkout> [<bazel-bin>]

The CMake checkout must have been built (out/cmake/kernel, out/boot/ioswitch) and the Bazel build run with
--//bazel/kernel:path_root=<cmake checkout>, --//bazel/kernel:build_date=<its version.c date>,
--//bazel/kernel:builder=<its builder> and --//boot:gpu_trustcache=//boot:no_gpu (or the same GPU trust cache).

(C) kernel: every section of Bazel's linked kernel against the CMake flow's source-kernel.macho. The linker's debug
    map (N_OSO: object paths and mtimes in __LINKEDIT) cannot match, and with it __LINKEDIT's size and the header's
    UUID; everything else must.
(B) prelink: tools/prelink run on the CMake flow's own linked kernel must reproduce its kernel.macho bit for bit.
(A) package: tools/package/boot.py run on the CMake flow's kernel.macho (and Bazel's other inputs) must reproduce
    its out/boot/ioswitch bit for bit.
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

WS = Path(__file__).resolve().parents[2]


def sections(data):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    at, out = 32, {}
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, at)
        if cmd == 0x19:
            seg = data[at + 8:at + 24].rstrip(b"\0").decode()
            nsects = struct.unpack_from("<I", data, at + 64)[0]
            for i in range(nsects):
                s = at + 72 + 80 * i
                name = data[s:s + 16].rstrip(b"\0").decode()
                addr, sz, off, _, _, _, flags = struct.unpack_from("<QQIIIII", data, s + 32)
                body = b"" if (flags & 0xFF) in (1, 0xC, 0x12) else data[off:off + sz]
                out[(seg, name)] = (addr, sz, hashlib.sha256(body).hexdigest())
        at += size
    return out


def compare_kernel(bazel, cmake):
    a, b = sections(Path(bazel).read_bytes()), sections(Path(cmake).read_bytes())
    differ = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
    print("(C) kernel sections: {} compared, {} differ{}".format(len(set(a) | set(b)), len(differ),
                                                             (": " + ", ".join(",".join(k) for k in differ)) if differ else ""))
    return not differ


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    cmake = Path(sys.argv[1])
    bin_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else WS / "bazel-bin"
    tree = bin_dir / "out/native-a57/root-v1/tree"
    external = Path(subprocess.run(["bazel", "info", "output_base"], capture_output=True, text=True, cwd=WS,
                                   check=True).stdout.strip()) / "external"
    ok = compare_kernel(tree / "build/xnu/mach.development.bcm2837.unstripped.noctf", cmake / "out/cmake/kernel/source-kernel.macho")
    with tempfile.TemporaryDirectory() as scratch:
        scratch = Path(scratch)
        # (B) the prelinker on the CMake kernel.
        out = scratch / "kernel.macho"
        roots = json.loads((WS / "bazel/pinned/prelink/dma-prelink.json").read_text())["roots"]
        new = sorted({"com.apple.iokit.IOGPUFamily", "com.apple.driver.AppleGameControllerPersonality",
                      "com.apple.iokit.IOUSBDeviceFamily", "com.apple.driver.usb.AppleUSBCommon",
                      "com.apple.driver.AppleUSBDeviceMux", "com.apple.iokit.IOUSBHostFamily",
                      "com.apple.driver.usb.AppleUSBXHCI", "com.apple.iokit.IOAccessoryManager", "com.apple.iokit.IOPortFamily",
                      "com.apple.driver.usb.AppleUSBHub", "com.apple.driver.usb.AppleUSBHostCompositeDevice",
                      "com.apple.driver.usb.IOUSBHostHIDDevice", "com.apple.iokit.IOUSBMassStorageDriver",
                      "com.apple.iokit.IOSCSIArchitectureModelFamily", "com.apple.iokit.IOSCSIBlockCommandsDevice",
                      "com.apple.iokit.IOPCIFamily"})
        args = [sys.executable, str(WS / "tools/prelink/emit_prelink.py"), "--kernel",
                str(cmake / "out/cmake/kernel/source-kernel.macho"), "--prelinked", str(bin_dir / "ipsw/kernelcache.decompressed"),
                "--slide=-0x1000000", "--early-security-init", "com.apple.driver.AppleMobileFileIntegrity=0xfffffff005ab4ae0",
                "--output", str(out), "--inherit-original-report", str(tree / "build/prelink/dma-kernel.json"),
                "--previous", str(WS / "bazel/pinned/prelink/dma-prelink.json")]
        for r in roots + new:
            args += ["--root", r]
        for r in new:
            args += ["--new-root", r]
        for c in ("com.apple.iokit.IOUSBHostFamily=com.apple.driver.AppleSMC=1",
                  "com.apple.iokit.IOUSBHostFamily=com.apple.driver.usb.AppleUSBHostPlatformProperties=0",
                  "com.apple.iokit.IOAccessoryManager=com.apple.driver.AppleOnboardSerial=1",
                  "com.apple.iokit.IOAccessoryManager=com.apple.iokit.IOSerialFamily=0"):
            args += ["--cut", c]
        for d in ("com.apple.iokit.IOUSBHostFamily=AppleUSBHostResources",
                  "com.apple.iokit.IOUSBHostFamily=AppleUSBHostResourcesTypeC",
                  "com.apple.iokit.IOUSBMassStorageDriver=IOUSBMassStorageResource"):
            args += ["--defer", d]
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
        same = sha(out) == sha(cmake / "out/cmake/kernel/kernel.macho")
        print("(B) prelink of the CMake kernel: {} ({})".format("IDENTICAL" if same else "DIFFERENT", sha(out)[:16]))
        ok &= same
        # (A) the package from the CMake kernel.
        package = scratch / "boot"
        boot = bin_dir / "boot/ioswitch"
        command = [sys.executable, str(WS / "tools/package/boot.py"), "--xcode-env",
                   str(external / "+xcode_host+xcode/xcode.env")]
        # boot.py takes workspace-relative paths; run it from a directory that has every input at its path.
        root = scratch / "root"
        for rel, src in {
            "kernel.macho": cmake / "out/cmake/kernel/kernel.macho",
            "adt": bin_dir / "boot/full-ram.adt",
            "static": bin_dir / "ipsw/original-static-system.bundle",
            "app": next((external / "+ipsw+ipsw_app_cryptex_trust_cache").glob("*.trustcache")),
            "seed": WS / "bazel/pinned/data-seed/data-v10.dbr",
            "template": WS / "boot/raw-v24-boot.cmd",
            "bl31": WS / "bazel/pinned/firmware/bl31.bin", "bl33": WS / "bazel/pinned/firmware/bl33.bin",
            "dtb": WS / "bazel/pinned/firmware/oled-sku3.dtb", "exporter": WS / "bazel/pinned/firmware/exporter.uimg",
            "hcd": WS / "bazel/pinned/firmware/BCM4356A3.hcd",
            "manifest": WS / "out/native-a57/root-v1/tree/overlay/manifest.json",
            "portrait": WS / "out/native-a57/root-v1/tree/drivers/NativeMobileDisplay/IOSwitchPortrait.h",
        }.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, root / rel)
        overlay = []
        for entry in json.loads((root / "manifest").read_text())["files"]:
            built = cmake / "out/native-a57/root-v1/tree" / entry["source"]
            if built.exists():
                (root / "overlay" / entry["source"]).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(built, root / "overlay" / entry["source"])
                overlay += ["--overlay", entry["source"] + "=overlay/" + entry["source"]]
        sources = []
        for d in ("native/loader", "native/handoff", "native/storage", "tools/package"):
            for f in sorted((WS / d).iterdir()):
                if f.is_file():
                    (root / d).mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(f, root / d / f.name)
                    sources += ["--source", d + "/" + f.name]
        llvm = external / "+llvm_tools+llvm/bin"
        tools = []
        for t in ("ld.lld", "llvm-objcopy", "llvm-readelf", "llvm-objdump", "lz4"):
            shutil.copyfile(bin_dir / "tools/lz4" if t == "lz4" else llvm / t, root / t)
            os.chmod(root / t, 0o755)
            tools += ["--llvm-tool", t]
        command += ["--kernel", "kernel.macho", "--adt", "adt", "--static-trust", "static", "--app-trust", "app",
                    "--data-seed", "seed", "--template", "template", "--bl31", "bl31", "--bl33", "bl33", "--dtb", "dtb",
                    "--exporter", "exporter", "--bt-patch", "hcd", "--overlay-manifest", "manifest",
                    "--portrait-header", "portrait", "--out", str(package), "--smp"] + overlay + sources + tools
        subprocess.run(command, check=True, cwd=root, stdout=subprocess.DEVNULL)
        cmake_boot = cmake / "out/boot/ioswitch"
        files = sorted(str(p.relative_to(cmake_boot)) for p in cmake_boot.rglob("*") if p.is_file())
        mine = sorted(str(p.relative_to(package)) for p in package.rglob("*") if p.is_file())
        differ = [f for f in files if f not in mine or sha(package / f) != sha(cmake_boot / f)]
        print("(A) package of the CMake kernel: {} files, {} differ{}".format(len(files), len(differ), (": " + ", ".join(differ)) if differ else ""))
        ok &= not differ and files == mine
        # The Bazel package itself, file by file against the CMake one (differences follow from (C)).
        rows = []
        for f in files:
            ours = boot / f
            rows.append("    {:60} {}".format(f, "identical" if ours.exists() and sha(ours) == sha(cmake_boot / f) else "differs"))
        print("Bazel //:boot vs CMake out/boot/ioswitch:\n" + "\n".join(rows))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
