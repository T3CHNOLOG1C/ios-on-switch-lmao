#!/usr/bin/env python3
"""fake_boot_package.py <out dir> [--generation N]: a small stand-in for //:boot (same layout, dummy bytes).

For the installer's and deploy's tests only: switchroot/ioswitch/<the eight files>, bootloader/ini/ioswitch.ini (the
real entry's text), overlay/<two files>. --generation changes two files' contents, to test that a rerun rewrites
exactly the files that differ."""
import sys
from pathlib import Path

FILES = ["bl31.bin", "bl33.bin", "oled-sku3.dtb", "exporter.uimg", "loader.uboot.bin", "kernel-window.bin", "boot.cmd",
         "boot.scr"]
INI = "[IOSwitch]\nl4t=1\nboot_prefixes=/switchroot/ioswitch/\nid=IOSWITCH\n"


def gpu_bundle(out):
    """//gpu:bundle's layout with dummy files (deploy --stage-gpu tests)."""
    bundle = out / "ngpu/addons/TegraGM20BMetal.bundle"
    bundle.mkdir(parents=True)
    for name in ("TegraGM20BMetal", "libvulkan_nouveau.dylib", "Info.plist"):
        (bundle / name).write_bytes(b"fake " + name.encode())
    seed = out / "ngpu/addons/TegraGM20BMetal.m2v-seed/shared/v1"
    seed.mkdir(parents=True)
    (seed / "SEED").write_text("fake seed\n")
    (out / "TegraGM20BMetal.trustcache").write_bytes(bytes(24))


def main():
    out = Path(sys.argv[1])
    if "--gpu" in sys.argv:
        gpu_bundle(out)
        return
    generation = int(sys.argv[sys.argv.index("--generation") + 1]) if "--generation" in sys.argv else 0
    for name in FILES:
        path = out / "switchroot/ioswitch" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        body = "fake {} for the installer test".format(name).encode()
        if name in ("kernel-window.bin", "boot.scr"):
            body += b" generation %d" % generation
        path.write_bytes(body * 64)
    ini = out / "bootloader/ini/ioswitch.ini"
    ini.parent.mkdir(parents=True, exist_ok=True)
    ini.write_text(INI)
    for relative, mode in (("usr/sbin/BlueTool", 0o755), ("System/Library/LaunchDaemons/com.ioswitch.test.plist", 0o644)):
        path = out / "overlay" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"overlay " + relative.encode() + b" generation %d\n" % generation)
        path.chmod(mode)


if __name__ == "__main__":
    main()
