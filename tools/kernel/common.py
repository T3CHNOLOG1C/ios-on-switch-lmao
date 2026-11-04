"""Helpers shared by the kernel build tools (run by Bazel actions, never by hand)."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

# Written into recipes, link commands and records in place of the action's own execution root, which differs
# between sandboxes; every tool that reads them puts its own execution root back.
EXECROOT = "@EXECROOT@"
# Apple clang's UBSan source locations do not honour -ffile-prefix-map: keep their last three path components.
UBSAN_PATHS = "-fsanitize-undefined-strip-path-components=-3"
# The collation XNU's make sorts symbol sets with (the CMake flow inherited the user's en_IE.UTF-8).
BUILD_LOCALE = "en_IE.UTF-8"
# The PATH every tool gives the processes it starts: the system's and Xcode's tools, nothing from Homebrew.
SYSTEM_PATH = "/usr/bin:/bin:/usr/sbin:/sbin"


def xcode_env(path):
    """@xcode//:xcode.env as a dict (DEVELOPER_DIR, XCODE_TOOLCHAIN, IPHONEOS_SDK, MACOSX_SDK)."""
    values = {}
    for line in Path(path).read_text().splitlines():
        key, _, value = line.partition("=")
        if key:
            values[key] = value
    return values


def environment(xcode, extra_path=(), **extra):
    """A clean environment: fixed PATH, Xcode selected by DEVELOPER_DIR, deterministic archives, no locale drift."""
    env = {
        "PATH": ":".join([*extra_path, SYSTEM_PATH]),
        "DEVELOPER_DIR": xcode["DEVELOPER_DIR"],
        "HOME": os.environ.get("TMPDIR", "/tmp"),
        "TMPDIR": os.environ.get("TMPDIR", "/tmp"),
        "LC_ALL": "C",
        "LANG": "C",
        "TZ": "UTC",
        "ZERO_AR_DATE": "1",
    }
    env.update({k: str(v) for k, v in extra.items()})
    return env


def run(command, env, cwd=None, log=None):
    command = [str(c) for c in command]
    result = subprocess.run(command, env=env, cwd=cwd, stdout=log or None, stderr=subprocess.STDOUT if log else None)
    if result.returncode:
        if log:
            log.flush()
        sys.exit("command failed ({}): {}".format(result.returncode, " ".join(command)[:2000]))


def execroot():
    return os.getcwd()


def prefix_maps(path_root, bin_dir, xnu_dir):
    """-ffile-prefix-map flags that make every path a compile can embed independent of the sandbox: the patched XNU
    source shows as <root>/out/native-a57/root-v1/tree/.cache/xnu-work (where the CMake flow kept its checkout),
    build outputs as <root>/<package path> (bazel-bin/out/... -> out/...), sources as <root>/<path>."""
    here = execroot()
    tree = path_root.rstrip("/") + "/out/native-a57/root-v1/tree/.cache/xnu-work"
    return [
        "-ffile-prefix-map={}/{}={}".format(here, xnu_dir, tree),
        "-ffile-prefix-map={}/{}={}".format(here, bin_dir, path_root.rstrip("/")),
        "-ffile-prefix-map={}={}".format(here, path_root.rstrip("/")),
    ]


def copy_tree(source, target):
    """Copy a tree as Bazel stages it (files are symlinks into the execroot): contents, not links. Framework bundles'
    alias entries (Headers -> Versions/Current/Headers, Versions/Current -> A), which a tree artifact input shows as
    plain directories, become relative symlinks again instead of second copies."""
    source, target = Path(source), Path(target)

    def aliases(directory, names):
        directory = Path(directory)
        skip = []
        if directory.name.endswith(".framework") and (directory / "Versions/A").is_dir():
            skip = [n for n in names if n != "Versions" and (directory / "Versions/A" / n).exists()]
        elif directory.name == "Versions" and directory.parent.name.endswith(".framework") and "A" in names:
            skip = ["Current"] if "Current" in names else []
        return skip

    shutil.copytree(source, target, symlinks=False, dirs_exist_ok=True, ignore=aliases)
    for framework in sorted(target.rglob("*.framework")):
        versions = framework / "Versions"
        source_framework = source / framework.relative_to(target)
        if not (versions / "A").is_dir():
            continue
        if (source_framework / "Versions/Current").exists() and not (versions / "Current").exists():
            os.symlink("A", versions / "Current")
        for entry in sorted((source_framework / "Versions/A").iterdir()):
            if (source_framework / entry.name).exists() and not (framework / entry.name).exists():
                os.symlink("Versions/Current/" + entry.name, framework / entry.name)
