"""Link XNU's kernel (mach.development.bcm2837.unstripped.noctf) with our archives: the command XNU's make recorded
(xnu_build.py), with the archive list the XNU build script would have passed as LD_KERNEL_ARCHIVES, run in the same
directory. ZERO_AR_DATE=1 and -oso_prefix keep build times and sandbox paths out of the linker's debug map.

usage: link.py --xcode-env F --link-json J --objroot DIR --sdkroot DIR --output O --bin-dir D -- <archive args>...
Archive args: "force_load:<path>", "plain:<path>" (an object or archive on the line as is), "lib-dir:<dir>",
"lib:<name>" (-l<name>), in link order.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    for name in ("xcode-env", "link-json", "objroot", "sdkroot", "output", "bin-dir"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("archives", nargs="*")
    args = parser.parse_args()
    here = common.execroot()
    xcode = common.xcode_env(args.xcode_env)
    recorded = json.loads(Path(args.link_json).read_text())

    def expand(value):
        return value.replace(common.EXECROOT, here)

    archives = []
    for spec in args.archives:
        kind, _, path = spec.partition(":")
        absolute = os.path.join(here, path)
        if kind == "force_load":
            archives.append("-Wl,-force_load," + absolute)
        elif kind == "plain":
            archives.append(absolute)
        elif kind == "lib-dir":
            archives.append("-L" + absolute)
        elif kind == "lib":
            archives.append("-l" + path)
        else:
            sys.exit("bad archive spec " + spec)
    argv = [expand(a) for a in recorded["argv"]]
    if "@KERNEL_ARCHIVES@" not in argv:
        sys.exit("the recorded link has no @KERNEL_ARCHIVES@ placeholder")
    at = argv.index("@KERNEL_ARCHIVES@")
    argv[at:at + 1] = archives
    output = os.path.join(here, args.output)
    out_at = argv.index("-o")
    argv[out_at + 1] = output
    # The linker's debug map (N_OSO) names every object: relative to the output tree, never the sandbox.
    argv += ["-Wl,-oso_prefix," + os.path.join(here, args.bin_dir) + "/"]
    env = common.environment(xcode)
    cwd = expand(recorded["cwd"])
    with tempfile.TemporaryDirectory() as scratch:
        # The object lists name @EXECROOT@: give the linker resolved copies.
        for i, value in enumerate(argv):
            if value == "-filelist":
                listed = Path(cwd, argv[i + 1]).read_text().replace(common.EXECROOT, here)
                resolved = os.path.join(scratch, "{}.filelist".format(i))
                Path(resolved).write_text(listed)
                argv[i + 1] = resolved
        result = subprocess.run([recorded["ld"]] + argv, cwd=cwd, env=env)
    if result.returncode:
        sys.exit(result.returncode)
    data = Path(output).read_bytes()
    if here.encode() in data:
        at = data.index(here.encode())
        sys.exit("the kernel names the execution root: {!r}".format(data[max(0, at - 100):at + 200]))


if __name__ == "__main__":
    main()
