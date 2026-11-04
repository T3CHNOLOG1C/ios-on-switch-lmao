"""An iOS 17 userland binary for the overlay, as cmake/Drivers.cmake built them: Xcode's clang for arm64-apple-ios17.0
with the iPhoneOS SDK, then an ad hoc signature (no timestamp: the CDHash is a function of the bytes).

usage: ios_binary.py --xcode-env F --out O --path-root P --bin-dir D [--coreaudio-headers]
                     [--sign-identifier ID] [--entitlements E] [--pagesize N] -- <clang args and sources>
--coreaudio-headers puts the macOS SDK's AudioServerPlugIn.h, AudioHardwareBase.h and AudioHardware.h first on the
include path as CoreAudio/ (the iOS SDK lacks them; the HAL plug-in interface is the same).
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--xcode-env", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--path-root", required=True)
    parser.add_argument("--bin-dir", required=True)
    parser.add_argument("--coreaudio-headers", action="store_true")
    parser.add_argument("--sign-identifier")
    parser.add_argument("--entitlements")
    parser.add_argument("--pagesize")
    parser.add_argument("clang", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    here = common.execroot()
    xcode = common.xcode_env(args.xcode_env)
    env = common.environment(xcode)
    clang = [a for a in args.clang if a != "--"]
    scratch = tempfile.mkdtemp()
    if args.coreaudio_headers:
        headers = Path(scratch, "H/CoreAudio")
        headers.mkdir(parents=True, exist_ok=True)
        for name in ("AudioServerPlugIn.h", "AudioHardwareBase.h", "AudioHardware.h"):
            os.symlink(xcode["MACOSX_SDK"] + "/System/Library/Frameworks/CoreAudio.framework/Headers/" + name,
                       headers / name)
        clang = ["-I", str(Path(scratch, "H"))] + clang
    # Absolute paths, as the CMake flow gave them, mapped back to the path root.
    clang = [os.path.join(here, a) if (not a.startswith("-") and os.path.exists(a)) else a for a in clang]
    command = ["xcrun", "clang", "-target", "arm64-apple-ios17.0", "-isysroot", xcode["IPHONEOS_SDK"]] + clang
    command += common.prefix_maps(args.path_root, args.bin_dir, "external/+http_archive+xnu")
    command += ["-o", os.path.join(here, args.out)]
    subprocess.run(command, check=True, env=env)
    if True:
        # Ad hoc, as the CMake flow signed them (identifier defaults to the file name).
        sign = ["codesign", "-f", "-s", "-"]
        if args.pagesize:
            sign += ["--pagesize", args.pagesize]
        if args.sign_identifier:
            sign += ["--identifier", args.sign_identifier]
        if args.entitlements:
            sign += ["--entitlements", args.entitlements]
        subprocess.run(sign + [args.out], check=True, env=env)
    data = Path(args.out).read_bytes()
    if here.encode() in data:
        sys.exit("{} names the execution root".format(args.out))


if __name__ == "__main__":
    main()
