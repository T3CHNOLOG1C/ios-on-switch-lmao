"""Link XNU's kernel (mach.development.bcm2837.unstripped.noctf) with our archives: the command XNU's make recorded
(xnu_build.py), with the archive list the XNU build script would have passed as LD_KERNEL_ARCHIVES, run in the same
directory. ZERO_AR_DATE=1 and -oso_prefix keep build times and sandbox paths out of the linker's debug map; while
the layout table keeps old spellings (old_output_spelling), the linker reads its inputs through links at their
old paths, so the debug map names the same objects as before the layout move.

usage: link.py --xcode-env F --link-json J --objroot DIR --sdkroot DIR --output O --bin-dir D -- <archive args>...
Archive args: "force_load:<path>", "plain:<path>" (an object or archive on the line as is), "lib-dir:<dir>",
"lib:<name>" (-l<name>), in link order.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402


# The kernel tree's own subpackages before the move: their outputs were below OLD_TREE/<package>/ already.
OLD_SUBPACKAGES = ("drivers/NativeMetal", "drivers/TegraGM20BMetal")


def old_output_spelling(path: str) -> str:
    """The pre-move spelling of an output-directory-relative path.

    A moved package's outputs (the kernel package's, drivers/<X>'s, kits/<X>'s, diagnostics/ioswitch's, overlay's) were
    the kernel tree package's. Unchanged while nothing has moved (common's layout table).
    """
    if common.KERNEL_PACKAGE == common.OLD_TREE:
        return path
    parts = path.split("/")
    old = common.OLD_TREE
    if parts[0] == common.KERNEL_PACKAGE:
        return "/".join([old, *parts[1:]])
    if len(parts) > 2 and parts[0] in ("drivers", "kits"):
        if "/".join(parts[:2]) in OLD_SUBPACKAGES:
            return old + "/" + path
        return "/".join([old, *parts[2:]])
    if parts[:2] == ["diagnostics", "ioswitch"]:
        return "/".join([old, *parts[2:]])
    if parts[0] == "overlay":
        return "/".join([old, *parts[1:]])
    return path


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

    bin_root = os.path.join(here, args.bin_dir)
    scratch = tempfile.mkdtemp()
    # Where the linker sees the output tree: the real one, or (layout compat) links at the old spellings.
    alias_root = bin_root if common.KERNEL_PACKAGE == common.OLD_TREE else os.path.join(scratch, "bin")

    def alias(path: str) -> str:
        if alias_root == bin_root or not path.startswith(bin_root + "/"):
            return path
        link = os.path.join(alias_root, old_output_spelling(path[len(bin_root) + 1:]))
        if not os.path.lexists(link):
            os.makedirs(os.path.dirname(link), exist_ok=True)
            os.symlink(path, link)
        elif os.path.realpath(link) != os.path.realpath(path):
            sys.exit(f"two link inputs share the old spelling {link}")
        return link

    archives, lib_dirs = [], []
    for spec in args.archives:
        kind, _, path = spec.partition(":")
        if kind == "lib-dir":
            lib_dirs.append(os.path.join(here, path))
        if kind == "lib" and alias_root != bin_root:
            # ld64 names a library it searched for by its real path: give it the archive's old spelling instead.
            found = [f for f in (os.path.join(d, f"lib{path}.a") for d in lib_dirs) if os.path.isfile(f)]
            if found:
                archives.append(alias(found[0]))
                continue
        absolute = alias(os.path.join(here, path))
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
    cwd = expand(recorded["cwd"])

    def object_path(value: str) -> str:
        # ld64 names a relative object by its physical working directory: give it the old spelling's absolute path.
        if alias_root == bin_root:
            return value
        return alias(value if os.path.isabs(value) else os.path.join(cwd, value))

    def object_arg(value: str) -> str:
        # Objects on the command line (XNU's C++ objects are .cpo); the object lists hold nothing else.
        if value.startswith("-") or not value.endswith((".o", ".cpo", ".a")):
            return value
        return object_path(value)

    argv = [object_arg(a) for a in argv]
    if "@KERNEL_ARCHIVES@" not in argv:
        sys.exit("the recorded link has no @KERNEL_ARCHIVES@ placeholder")
    at = argv.index("@KERNEL_ARCHIVES@")
    argv[at:at + 1] = archives
    output = os.path.join(here, args.output)
    out_at = argv.index("-o")
    argv[out_at + 1] = output
    # The linker's debug map (N_OSO) names every object: relative to the output tree, never the sandbox.
    argv += ["-Wl,-oso_prefix," + alias_root + "/"]
    env = common.environment(xcode)
    try:
        # The object lists name @EXECROOT@: give the linker resolved copies.
        for i, value in enumerate(argv):
            if value == "-filelist":
                listed = Path(cwd, argv[i + 1]).read_text().replace(common.EXECROOT, here)
                listed = "".join(object_path(line) + "\n" for line in listed.splitlines() if line)
                resolved = os.path.join(scratch, "{}.filelist".format(i))
                Path(resolved).write_text(listed)
                argv[i + 1] = resolved
        result = subprocess.run([recorded["ld"]] + argv, cwd=cwd, env=env)
    finally:
        shutil.rmtree(scratch)
    if result.returncode:
        sys.exit(result.returncode)
    data = Path(output).read_bytes()
    for root, what in ((here, "the execution root"), (scratch, "the link's scratch directory")):
        if root.encode() in data:
            at = data.index(root.encode())
            sys.exit(f"the kernel names {what}: {data[max(0, at - 100) : at + 200]!r}")


if __name__ == "__main__":
    main()
