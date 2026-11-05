"""The symbol names XNU's kernel defines (kernel_names.py), as a list the archives linked beside it namespace
against: one line per name, "g <name>" for a global, "l <name>" for a local.

An XNU change that adds or removes no symbol leaves this list, and so corecrypto's archive, as it was: Bazel then
does not rebuild corecrypto (41 s) after an edit inside an XNU function.

usage: xnu_symbols.py --xcode-env F --objroot DIR --out NAMES.txt
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402
from kernel_names import kernel_names  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    for name in ("xcode-env", "objroot", "out"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    env = common.environment(common.xcode_env(args.xcode_env))
    objroot = Path(common.execroot(), args.objroot)
    globals_ = kernel_names(objroot, env)
    every = kernel_names(objroot, env, global_only=False)
    lines = ["g " + name for name in sorted(globals_)] + ["l " + name for name in sorted(every - globals_)]
    Path(args.out).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
