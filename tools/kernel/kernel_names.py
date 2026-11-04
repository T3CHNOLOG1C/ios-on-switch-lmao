"""Symbol names of the XNU kernel alone (tree/scripts/xnu_symbols.py kernel_names): the objects of XNU's link
(link.filelist) plus the link's aliases (all-alias.exp), for archives that must not collide with it."""
import subprocess
from pathlib import Path

import common

OBJ = "DEVELOPMENT_ARM64_BCM2837"


def defined_names(paths, global_only, env):
    names = set()
    paths = [str(path) for path in paths]
    for start in range(0, len(paths), 200):
        command = ["xcrun", "nm", *(["-g"] if global_only else []), "-U", "-j", *paths[start:start + 200]]
        output = subprocess.check_output(command, text=True, env=env)
        names |= {line.strip() for line in output.splitlines() if line.startswith("_")}
    return names


def kernel_names(objroot, env, global_only=True):
    here = common.execroot()
    filelist = Path(objroot) / OBJ / "link.filelist"
    objects = [line.strip().replace(common.EXECROOT, here) for line in filelist.read_text().splitlines() if line.strip()]
    names = defined_names(objects, global_only, env)
    aliases = Path(objroot) / OBJ / "all-alias.exp"
    if aliases.is_file():
        names |= {word for line in aliases.read_text().splitlines() for word in line.split() if word.startswith("_")}
    return names


def archive_names(path, mode, env, global_only=True):
    output = subprocess.check_output(["xcrun", "nm", *(["-g"] if global_only else []), mode, "-j", str(path)],
                                     text=True, env=env)
    return {line.strip() for line in output.splitlines() if line.startswith("_")}
