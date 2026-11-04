"""libpthread_kernel.a (tree/scripts/build-libpthread-kernel.py): libpthread-519.120.4's three kernel sources (patched by
tree/patches/libpthread), names shared with XNU renamed pthread519_<name>, and XNU's source_pthread_bootstrap.c
(our XNU patch adds it) compiled with prng_random.o's compile record.

usage: libpthread.py --xcode-env F --source DIR --xnu DIR --sdkroot DIR --objroot DIR --out A.a --bootstrap-out O
                     --path-root P --bin-dir D
"""
import argparse
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402
from kernel_names import archive_names, kernel_names  # noqa: E402


def check(path):
    data = path.read_bytes()
    if struct.unpack_from("<4I", data) != (0xFEEDFACF, 0x100000C, 0, 1):
        sys.exit("Unexpected pthread object architecture: " + path.name)


def main():
    parser = argparse.ArgumentParser()
    for name in ("xcode-env", "source", "xnu", "sdkroot", "objroot", "out", "bootstrap-out", "path-root", "bin-dir"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    here = common.execroot()
    xcode = common.xcode_env(args.xcode_env)
    env = common.environment(xcode)
    source = Path(here, args.source)
    objroot = Path(here, args.objroot)
    record_path = objroot / "DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/prng_random.o.json"
    record = json.loads(re.sub(r",\s*$", "", record_path.read_text().replace(common.EXECROOT, here)))
    framework = Path(here, args.sdkroot, "System/Library/Frameworks/Kernel.framework/Versions/A")
    tree = args.path_root.rstrip("/") + "/out/native-a57/root-v1/tree"
    maps = ["-ffile-prefix-map={}={}/.cache/libpthread-work".format(source, tree)] + \
        common.prefix_maps(args.path_root, args.bin_dir, args.xnu)

    def compile_one(path, destination, namespace=None):
        if path.name == "source_pthread_bootstrap.c":
            command, arguments, i = [], record["arguments"], 0
            while i < len(arguments):
                value = arguments[i]
                if value == record["file"]:
                    command.append(str(path))
                elif value == "-o":
                    command += ["-o", str(destination)]
                    i += 1
                elif value in ("-MJ", "-MF"):
                    i += 1
                elif value in ("-MD", "-MP"):
                    pass
                else:
                    command.append(value)
                i += 1
        else:
            command = ["xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios17.0",
                       "-isysroot", xcode["IPHONEOS_SDK"], "-mkernel", "-ffreestanding", "-fno-builtin",
                       "-fno-stack-check", "-std=gnu11", "-fblocks", "-O2", "-mno-implicit-float",
                       "-DKERNEL=1", "-DKERNEL_PRIVATE=1", "-DXNU_KERNEL_PRIVATE=1",
                       "-DMACH_KERNEL_PRIVATE=1", "-DMACH_ASSERT=1", "-DDEBUG=1",
                       "-D__PTHREAD_EXPOSE_INTERNALS__=1", "-DABSOLUTETIME_SCALAR_TYPE=1", "-DNEEDS_SCHED_CALL_T=1",
                       "-I", str(source / "private"), "-I", str(source / "include"), "-I", str(source),
                       "-I", str(framework / "PrivateHeaders"), "-I", str(framework / "Headers"),
                       "-Wno-deprecated-declarations", *maps, "-c", str(path), "-o", str(destination)]
        if namespace:
            command += ["-include", str(namespace)]
        result = subprocess.run(command, cwd=record["directory"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, env=env)
        if result.returncode:
            sys.exit(result.stdout[-4500:])
        check(destination)

    with tempfile.TemporaryDirectory() as scratch:
        out = Path(scratch)
        sources = [source / "kern" / name for name in ("kern_init.c", "kern_support.c", "kern_synch.c")]
        objects = [out / (s.stem + ".o") for s in sources]
        for s, o in zip(sources, objects):
            compile_one(s, o)
        names = kernel_names(objroot, env, global_only=False)
        definitions = set().union(*(archive_names(o, "-U", env) for o in objects))
        collisions = definitions & names
        if collisions:
            namespace = out / "namespace.h"
            namespace.write_text("".join("#define {} pthread519_{}\n".format(n[1:], n[1:]) for n in sorted(collisions)))
            for s, o in zip(sources, objects):
                compile_one(s, o, namespace)
        definitions = set().union(*(archive_names(o, "-U", env) for o in objects))
        imports = set().union(*(archive_names(o, "-u", env) for o in objects)) - definitions
        missing = sorted(imports - names - {"___dso_handle"})
        if not {"_pthread_kern", "_current_uthread"} <= collisions:
            sys.exit("Missing expected pthread/XNU static symbol isolation")
        if missing:
            sys.exit("Unavailable real kernel imports: {}".format(missing))
        bootstrap = out / "source_pthread_bootstrap.o"
        compile_one(Path(here, args.xnu, "bsd/kern/source_pthread_bootstrap.c"), bootstrap)
        archive = out / "libpthread_kernel.a"
        subprocess.run(["xcrun", "ar", "rcs", str(archive), *[str(o) for o in objects], str(bootstrap)], check=True, env=env)
        Path(args.out).write_bytes(archive.read_bytes())
        Path(args.bootstrap_out).write_bytes(bootstrap.read_bytes())


if __name__ == "__main__":
    main()
