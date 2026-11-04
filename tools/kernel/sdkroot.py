"""The kernel SDK root (the CMake flow's tree/.cache/fakeroot): availability.pl, libplatform's headers, libdispatch's
kernel os/ headers and XNU's installed headers, as the XNU build script assembled them before building XNU.

usage: sdkroot.py --xcode-env F --out DIR --availability DIR --libplatform DIR --libdispatch DIR --xnu DIR
                  --python-bin DIR [--jobs N]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

XNU_DARWIN = "23.6.0"
XNU_VERSION = "10063.141.1"


def install_availability_headers(source, availability_pl, out, python3, env, log, scratch):
    """AvailabilityVersions' CMake install (INSTALL_KERNEL_HEADERS=1, not DriverKit), without CMake: the templates
    preprocessed by availability.pl, the modulemaps by unifdef_command.sh, installed where its CMakeLists puts them;
    its Kernel.framework symlinks become copies (the same bytes, and tree artifacts stay self-contained)."""
    generated = Path(scratch) / "availability"
    generated.mkdir()
    templates = ["Availability.h", "AvailabilityInternal.h", "AvailabilityInternalLegacy.h",
                 "AvailabilityInternalPrivate.h", "AvailabilityMacros.h", "AvailabilityProhibitedInternal.h",
                 "AvailabilityVersions.h", "VersionMap.h", "for_dyld_priv.inc", "os_availability.h"]
    for name in templates:
        common.run([python3, availability_pl, "--preprocess", source / "templates" / name, generated / name], env, log=log)
    for name in ["os_availability.modulemap", "os_availability_private.modulemap", "AvailabilityPrivate.modulemap"]:
        common.run(["sh", source / "unifdef_command.sh", "-UBUILD_FOR_DRIVERKIT", "-o", generated / name,
                    source / "templates" / name], env, log=log)
    public = ["Availability.h", "AvailabilityInternal.h", "AvailabilityInternalLegacy.h", "AvailabilityMacros.h",
              "AvailabilityVersions.h"]
    local = ["AvailabilityInternalPrivate.h", "AvailabilityProhibitedInternal.h"]
    kernel = out / "System/Library/Frameworks/Kernel.framework/Versions/A"
    places = [(name, out / "usr/local/include/dyld" / name) for name in ("VersionMap.h", "for_dyld_priv.inc")]
    places += [(name, out / "usr/local/include" / name) for name in local]
    places += [(name, out / "usr/local/include" / name)
               for name in ("os_availability_private.modulemap", "AvailabilityPrivate.modulemap")]
    places += [("os_availability.h", out / "usr/include/os/availability.h")]
    places += [(name, out / "usr/include" / name) for name in public]
    places += [("os_availability.modulemap", out / "usr/include/os_availability.modulemap")]
    places += [(name, kernel / "PrivateHeaders" / name) for name in local]
    places += [("os_availability.h", kernel / "Headers/os/availability.h")]
    places += [(name, kernel / "Headers" / name) for name in public]
    for name, target in places:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(generated / name, target)
        target.chmod(0o644)


def main():
    parser = argparse.ArgumentParser()
    for name in ("xcode-env", "out", "availability", "libplatform", "libdispatch", "xnu", "python-bin"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--jobs", default="16")
    parser.add_argument("--log", required=True)
    args = parser.parse_args()
    xcode = common.xcode_env(args.xcode_env)
    out = Path(args.out).absolute()
    env = common.environment(xcode, extra_path=[str(Path(args.python_bin).absolute())])
    with open(args.log, "w") as log, tempfile.TemporaryDirectory() as scratch:
        # availability.pl: AvailabilityVersions' `availability --preprocess` (what its CMake install step runs).
        libexec = out / "usr/local/libexec"
        libexec.mkdir(parents=True, exist_ok=True)
        availability = Path(args.availability).absolute()
        python3 = env["PATH"].split(":")[0] + "/python3"
        common.run([python3, availability / "availability", "--preprocess",
                    availability / "availability", libexec / "availability.pl"], env, cwd=availability, log=log)
        (libexec / "availability.pl").chmod(0o755)
        install_availability_headers(availability, libexec / "availability.pl", out, python3, env, log, scratch)
        # libplatform: public and private headers, and the private platform headers into Kernel.framework.
        (out / "usr/local/include/kernel").mkdir(parents=True, exist_ok=True)
        libplatform = Path(args.libplatform)
        common.copy_tree(libplatform / "include", out / "usr/local/include")
        common.copy_tree(libplatform / "private", out / "usr/local/include")
        common.copy_tree(libplatform / "private/platform",
                         out / "System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform")
        # libdispatch's os/ headers, reduced to their kernel view.
        os_dir = out / "usr/local/include/os"
        os_dir.mkdir(parents=True, exist_ok=True)
        unifdef = xcode["DEVELOPER_DIR"] + "/Toolchains/XcodeDefault.xctoolchain/usr/bin/unifdef"
        if not os.path.exists(unifdef):
            unifdef = "/usr/bin/unifdef"
        for header in sorted((Path(args.libdispatch) / "os").glob("*.h")):
            result = subprocess.run([unifdef, "-DKERNEL=1", "-DOS_FIREHOSE_SPI=1", "-DOS_VOUCHER_ACTIVITY_SPI_TYPES=1",
                                     "-UOS_VOUCHER_ACTIVITY_SPI", str(header)], capture_output=True, env=env)
            if result.returncode not in (0, 1):
                sys.exit(result.stderr.decode())
            (os_dir / header.name).write_bytes(result.stdout)
        # XNU's installed headers (Kernel.framework, System.framework, usr/include, ...).
        xnu = Path(args.xnu).absolute()
        make_env = dict(env, KERNEL_SDKROOT=str(out), XNU_AVAILABILITY_PL=str(libexec / "availability.pl"))
        common.run(["make", "-C", xnu, "installhdrs", "SDKROOT=macosx", "ARCH_CONFIGS=ARM64", "MACHINE_CONFIGS=BCM2837",
                    "KERNEL_CONFIGS=DEVELOPMENT", "RC_DARWIN_KERNEL_VERSION=" + XNU_DARWIN,
                    "RC_ProjectSourceVersion=" + XNU_VERSION, "MAKEJOBS=-j" + args.jobs,
                    "OBJROOT=" + scratch + "/obj", "SYMROOT=" + scratch + "/sym", "DSTROOT=" + str(out)],
                   make_env, log=log)
        (out / (".xnu-" + XNU_VERSION + "-headers")).write_text("")


if __name__ == "__main__":
    main()
