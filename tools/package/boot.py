"""The boot entry (out/boot/ioswitch's layout) from declared inputs: package_raw_boot.main() unchanged (boot-args,
ADT edits, trust caches, layout, loader, boot.scr, overlay), with its path constants pointed at Bazel's inputs instead
of the live tree's out/ directories, and its result moved to the declared output directory.

usage: boot.py --kernel K --adt A --static-trust T --app-trust T [--gpu-trust T] --data-seed D --template B
               --bl31 F --bl33 F --dtb F --exporter F --bt-patch F --overlay-manifest M [--overlay SRC=PATH]...
               --portrait-header H --llvm-tool PATH... --out DIR [--smp]
"""
import argparse
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))


def main():
    parser = argparse.ArgumentParser()
    for name in ("kernel", "adt", "static-trust", "app-trust", "data-seed", "template", "bl31", "bl33", "dtb",
                 "exporter", "bt-patch", "overlay-manifest", "portrait-header", "out", "xcode-env"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--gpu-trust")
    parser.add_argument("--overlay", action="append", default=[], help="SOURCE=PATH: a manifest source and its file")
    parser.add_argument("--llvm-tool", action="append", default=[])
    parser.add_argument("--source", action="append", default=[], help="a loader source, at its workspace path")
    parser.add_argument("--smp", action="store_true")
    args = parser.parse_args()
    here = Path.cwd()
    developer = next(l.split("=", 1)[1] for l in Path(args.xcode_env).read_text().splitlines() if l.startswith("DEVELOPER_DIR="))
    with tempfile.TemporaryDirectory() as scratch:
        scratch = Path(scratch)
        # The loader builder finds its tools on PATH: Xcode's clang (/usr/bin/clang, as the CMake flow had it) and
        # LLVM 23.1.4's ld.lld/llvm-objcopy/llvm-readelf/llvm-objdump (the Homebrew version it used).
        tools = scratch / "bin"
        tools.mkdir()
        for tool in args.llvm_tool:
            os.symlink(here / tool, tools / Path(tool).name)
        os.environ.update(PATH="{}:/usr/bin:/bin".format(tools), DEVELOPER_DIR=developer, LC_ALL="C", TZ="UTC")
        import package_native_handoff as handoff
        import build_native_loader as loader
        import package_raw_boot as pkg
        # The packaging refuses symlinked inputs (and Bazel's sandbox stages every input as one): regular copies. The
        # loader's sources (native/loader, native/handoff, native/storage) keep their workspace paths below src/.
        src = scratch / "src"

        def regular(path):
            copy = src / path
            if not copy.exists():
                copy.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(here / path, copy)
            return copy

        for path in args.source:
            regular(path)
        handoff.ROOT = loader.ROOT = src
        loader.safe_output = lambda path: Path(path).absolute()
        firmware = scratch / "firmware"
        firmware.mkdir()
        for name, path in (("bl31.bin", args.bl31), ("bl33.bin", args.bl33), ("oled-sku3.dtb", args.dtb),
                           ("exporter.uimg", args.exporter), ("BCM4356A3.hcd", args.bt_patch)):
            shutil.copyfile(path, firmware / name)
        manifest = scratch / "layout-manifest.json"
        manifest.write_text(json.dumps({"sources": {
            "adt": {"path": str(regular(args.adt))},
            "TrustCache": {"path": str(regular(args.static_trust))},
            "NativeDataSeed": {"path": str(regular(args.data_seed))},
        }}))
        # The overlay manifest names its files relative to the kernel tree (build/... for built ones): a tree view.
        # package_raw_boot leaves out a file it cannot find ("not built"); under Bazel every manifest entry must be a
        # declared input of //boot:package (its overlay dict), so a missing one is an error, not a silent omission.
        given = {spec.partition("=")[0] for spec in args.overlay}
        wanted = {entry["source"] for entry in json.loads(Path(args.overlay_manifest).read_text())["files"]}
        if wanted - given:
            sys.exit("overlay/manifest.json lists {} but //boot:package's overlay does not give them: add them to "
                     "boot/BUILD.bazel".format(", ".join(sorted(wanted - given))))
        work = scratch / "work"
        for spec in args.overlay:
            source, _, path = spec.partition("=")
            target = work / "tree" / source
            target.parent.mkdir(parents=True, exist_ok=True)
            os.symlink(here / path, target)
        pkg.ROOT = scratch / "root"
        pkg.WORK = work
        pkg.INPUTS = manifest
        pkg.TEMPLATE = here / args.template
        pkg.FIRMWARE = firmware
        pkg.BT_PATCH = firmware / "BCM4356A3.hcd"
        pkg.APP_CRYPTEX_TRUST = regular(args.app_trust)
        pkg.GPU_TRUST = regular(args.gpu_trust) if args.gpu_trust else scratch / "no-gpu-trust-cache"
        pkg.OVERLAY_MANIFEST = here / args.overlay_manifest
        pkg.PORTRAIT_HEADER = here / args.portrait_header
        kernel_dir = scratch / "kernel"
        kernel_dir.mkdir()
        shutil.copyfile(args.kernel, kernel_dir / "kernel.macho")
        sys.argv = ["package_raw_boot.py", str(kernel_dir)] + (["--smp"] if args.smp else [])
        pkg.main()
        out = Path(args.out)
        shutil.rmtree(out, ignore_errors=True)
        shutil.move(str(pkg.ROOT / "out/boot" / pkg.NAME), str(out))


if __name__ == "__main__":
    main()
