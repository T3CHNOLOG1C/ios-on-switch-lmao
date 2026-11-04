"""snapshot.py --boot DIR --gpu DIR NAME: copy the built boot entry and Metal bundle to out/boot/lkg/<NAME> in the
checkout (switchroot/, bootloader/, overlay/ and gpu/, the layout deploy-sd.sh's PKG= took), for rollback and for
deploying exactly that build later:

    bazel run //boot:snapshot -- 1900-r9-something
    bazel run //deploy:sd -- disk4 --yes --stage-gpu \\
        --boot-package=$PWD/out/boot/lkg/1900-r9-something --gpu-bundle=$PWD/out/boot/lkg/1900-r9-something/gpu

The snapshot also keeps the build's kernel.macho, which names the unslid kernel addresses in panics and in the
"IOSwitch kprof" lines: `xcrun atos -o out/boot/lkg/<NAME>/kernel.macho -arch arm64 <addr>...`.

Refuses to overwrite an existing snapshot."""
import argparse
import hashlib
import os
import shutil
import sys
from pathlib import Path


def located(path):
    if path and not os.path.isabs(path):
        try:
            from python.runfiles import runfiles
            path = runfiles.Create().Rlocation(path) or path
        except ImportError:
            pass
    return Path(path).resolve()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--boot", required=True)
    parser.add_argument("--gpu")
    parser.add_argument("--kernel", help="the build's kernel.macho, kept for symbolication")
    parser.add_argument("--no-gpu", action="store_true", help="snapshot the boot entry only")
    parser.add_argument("name")
    args = parser.parse_args()
    if "/" in args.name or args.name.startswith("."):
        sys.exit("snapshot name must be a plain directory name")
    workspace = Path(os.environ.get("BUILD_WORKSPACE_DIRECTORY") or sys.exit("run with bazel run"))
    target = workspace / "out/boot/lkg" / args.name
    if target.exists():
        sys.exit("{} exists: pick another name".format(target))
    boot = located(args.boot)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(boot, target, symlinks=False)
    # copytree keeps bazel-bin's read-only modes; the snapshot root must take gpu/ and kernel.macho.
    target.chmod(0o755)
    if args.gpu and not args.no_gpu:
        shutil.copytree(located(args.gpu), target / "gpu", symlinks=False)
    if args.kernel:
        shutil.copyfile(located(args.kernel), target / "kernel.macho")
    for path in target.rglob("*"):
        path.chmod(0o755 if path.is_dir() or os.access(path, os.X_OK) else 0o644)
    print(target)
    for path in sorted((target / "switchroot/ioswitch").iterdir()):
        print("  {} sha256 {}".format(path.name, hashlib.sha256(path.read_bytes()).hexdigest()))


if __name__ == "__main__":
    main()
