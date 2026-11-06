"""storage_abi.py --xcode-env F --source DIR --out DIR: the CMake flow's tree/build/storage-abi/include
(tree/scripts/audit-storage-interface.py): IOStorageFamily-315's headers as IOKit/storage/*.h, plus the macOS SDK's
TargetConditionals.h, the one platform header the kernel SDK lacks.
"""

import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common

parser = argparse.ArgumentParser()
parser.add_argument('--xcode-env', required=True)
parser.add_argument('--source', required=True)
parser.add_argument('--out', required=True)
args = parser.parse_args()
xcode = common.xcode_env(args.xcode_env)
out = Path(args.out)
headers = out / 'IOKit/storage'
headers.mkdir(parents=True, exist_ok=True)
for path in sorted(Path(args.source).glob('*.h')):
    shutil.copyfile(path, headers / path.name)
shutil.copyfile(Path(xcode['MACOSX_SDK']) / 'usr/include/TargetConditionals.h', out / 'TargetConditionals.h')
