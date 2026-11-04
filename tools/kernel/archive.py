"""archive.py --xcode-env F --out A.a member.o...: a fresh static archive, members in the given order (with -force_load,
the link order), as cmake/XnuRecipes.cmake's ioswitch_archive (`xcrun ar rcs`); ZERO_AR_DATE=1 zeroes the member
dates, the one intended difference from the CMake flow's archives."""
import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument("--xcode-env", required=True)
parser.add_argument("--out", required=True)
parser.add_argument("members", nargs="+")
args = parser.parse_args()
env = common.environment(common.xcode_env(args.xcode_env))
if os.path.exists(args.out):
    os.unlink(args.out)
subprocess.run(["xcrun", "ar", "rcs", args.out, *args.members], check=True, env=env)
