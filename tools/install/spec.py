#!/usr/bin/env python3
"""What the kernel expects on the SD card, read from the kernel's own sources (they are the spec, not a copy).

  spec.py --raw-mount SwitchRawMount.h --cryptex-h SwitchCryptex.h --cryptex-cpp SwitchCryptex.cpp
          --block-cpp NativeSwitchBlock.cpp --out spec.json

SwitchRawMount.h     SWITCH_RAW_DATA_UUID, SWITCH_RAW_PREBOOT_UUID
SwitchCryptex.h        SWITCH_PREBOOT_UUID (must agree), the two cryptex volume UUIDs
SwitchCryptex.cpp      the Preboot paths of the two cryptex images
NativeSwitchBlock.cpp  kRootUUID (the System volume, also the ADT root-matching UUID), and the APFS type GUID and
                         GPT name it selects the partition by (any size)
"""

import argparse
import json
import re
import uuid
from pathlib import Path


def _define(text, name, path):
    found = re.search(r'^#define\s+' + name + r'\s+(.+?)\s*(?://.*|/\*.*)?$', text, re.MULTILINE)
    if not found:
        raise SystemExit(f'{path}: no #define {name}')
    return found.group(1).strip()


def _uuid(value, what):
    try:
        return str(uuid.UUID(value.strip('"'))).upper()
    except ValueError:
        raise SystemExit(f'{what} is not a UUID: {value}')


def parse(raw_mount, cryptex_h, cryptex_cpp, block_cpp):
    raw_text, h_text = Path(raw_mount).read_text(), Path(cryptex_h).read_text()
    cpp_text, block_text = Path(cryptex_cpp).read_text(), Path(block_cpp).read_text()
    data = _uuid(_define(raw_text, 'SWITCH_RAW_DATA_UUID', raw_mount), 'SWITCH_RAW_DATA_UUID')
    preboot = _uuid(_define(raw_text, 'SWITCH_RAW_PREBOOT_UUID', raw_mount), 'SWITCH_RAW_PREBOOT_UUID')
    if preboot != _uuid(_define(h_text, 'SWITCH_PREBOOT_UUID', cryptex_h), 'SWITCH_PREBOOT_UUID'):
        raise SystemExit('SwitchRawMount.h and SwitchCryptex.h disagree on the Preboot UUID')
    system_cryptex = _uuid(_define(h_text, 'SWITCH_SYSTEM_CRYPTEX_UUID', cryptex_h), 'SWITCH_SYSTEM_CRYPTEX_UUID')
    app_cryptex = _uuid(_define(h_text, 'SWITCH_APP_CRYPTEX_UUID', cryptex_h), 'SWITCH_APP_CRYPTEX_UUID')
    paths = re.search(r'paths\[SWITCH_CRYPTEX_COUNT\]\s*=\s*\{([^}]*)\}', cpp_text)
    if not paths:
        raise SystemExit(f'{cryptex_cpp}: no cryptex paths table')
    images = re.findall(r'"(/private/preboot/[^"]+)"', paths.group(1))
    if len(images) != 2:
        raise SystemExit(f'{cryptex_cpp}: expected two cryptex paths, found {images}')
    root = re.search(r'kRootUUID\[\]\s*=\s*"([0-9A-Fa-f-]{36})"', block_text)
    if not root:
        raise SystemExit(f'{block_cpp}: no kRootUUID')
    apfs_type = re.search(r'kApfsType\[16\]\s*=\s*\{([^}]*)\}', block_text)
    if not apfs_type:
        raise SystemExit(f'{block_cpp}: no kApfsType')
    type_bytes = bytes(int(b, 16) for b in re.findall(r'0x([0-9a-fA-F]{2})', apfs_type.group(1)))
    if len(type_bytes) != 16:
        raise SystemExit(f'{block_cpp}: kApfsType is not 16 bytes')
    name = re.search(r'kPartitionName\[\]\s*=\s*"([^"]+)"', block_text)
    if not name:
        raise SystemExit(f'{block_cpp}: no kPartitionName')
    return {
        'partition_type': str(uuid.UUID(bytes_le=type_bytes)).upper(),
        'partition_name': name.group(1),
        'volumes': {
            'system': {'uuid': _uuid(root.group(1), 'kRootUUID'), 'role': 0x0001},
            'data': {'uuid': data, 'role': 0x0040, 'name': 'SwitchDataTrial'},
            'preboot': {'uuid': preboot, 'role': 0x0010, 'name': 'SwitchPrebootTrial'},
        },
        'cryptexes': [
            {'path': images[0], 'volume_uuid': system_cryptex, 'member': 'system_cryptex'},
            {'path': images[1], 'volume_uuid': app_cryptex, 'member': 'app_cryptex'},
        ],
        'sources': {
            k: Path(v).name
            for k, v in (
                ('raw_mount', raw_mount),
                ('cryptex_h', cryptex_h),
                ('cryptex_cpp', cryptex_cpp),
                ('block_cpp', block_cpp),
            )
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('raw-mount', 'cryptex-h', 'cryptex-cpp', 'block-cpp', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    spec = parse(args.raw_mount, args.cryptex_h, args.cryptex_cpp, args.block_cpp)
    Path(args.out).write_text(json.dumps(spec, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
