#!/usr/bin/env python3
"""U-Boot's half of the handoff: boot/u-boot-script.txt with this build's window and slot values.

As text and as the legacy script image bl33.py stages in bl33.bin (loader_abi.SCRIPT_SLOT), where the patched U-Boot
sources it.

    boot_script.py --template T --constants C --dtb DTB --text OUT --image OUT
"""

import argparse
import json
import re
import struct
from pathlib import Path

from loader_abi import BOOT_ARGS_BYTES, BOOT_ARGS_PAGE, DTB_SLOT, EXPORTER_SLOT, LOADER_SLOT, STAGE_SOURCE
from uimage import Header, legacy_image


# U-Boot's board test per board (hekate's sku: 0 icosa, 1 iowa, 2 hoag, 3 aula; t210b01 is set only on a T210B01)
BOARD_TESTS = {
    'aula': 'test "${t210b01}" != "1" -o "${sku}" != "3"',
    'icosa': 'test "${t210b01}" = "1" -o "${sku}" != "0"',
}


def boot_script(text: str, constants: dict, dtb_bytes: int, board: str = 'aula') -> str:
    """Fill every @NAME@ of the template.

    The values: the staged window's header and trailer words (boot/loader/stage.h), the slots and the bootargs.txt
    page.
    """
    staged = constants['PACKED_BYTES']
    trailer = STAGE_SOURCE + staged
    values = {
        'ARGS_PA': BOOT_ARGS_PAGE,
        'ARGS_BYTES': BOOT_ARGS_BYTES,
        'DTB': DTB_SLOT[0],
        'DTB_BYTES': dtb_bytes,
        'EXPORTER': EXPORTER_SLOT[0],
        'LOADER': LOADER_SLOT[0],
        'STAGE_STREAM': constants['STREAM_BYTES'],
        'STAGE_BYTES': staged,
        'STAGE_ID_LOW': constants['STAGE_ID'] & 0xFFFFFFFF,
        'STAGE_ID_HIGH': constants['STAGE_ID'] >> 32,
        **{f'TRAILER_{i}': trailer + 4 * i for i in range(6)},
    }
    for name, value in values.items():
        if f'@{name}@' not in text:
            raise SystemExit(f'boot script template has no @{name}@')
        text = text.replace(f'@{name}@', hex(value))
    if '@BOARD_TEST@' not in text:
        raise SystemExit('boot script template has no @BOARD_TEST@')
    text = text.replace('@BOARD_TEST@', BOARD_TESTS[board])
    if re.search(r'@[A-Z_0-9]+@', text):
        raise SystemExit('boot script template has a value this tool does not fill')
    return text


def script_image(script: str) -> bytes:
    """Wrap the script as U-Boot's source command takes it: a legacy script image of one part."""
    data = script.encode('ascii')
    return legacy_image(struct.pack('>II', len(data), 0) + data, Header(os_type=17, kind=6, name='Boot'))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('template', 'constants', 'dtb', 'text', 'image'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--board', choices=sorted(BOARD_TESTS), default='aula')
    args = parser.parse_args()
    script = boot_script(args.template.read_text(), json.loads(args.constants.read_text()), args.dtb.stat().st_size, args.board)
    args.text.write_text(script)
    args.image.write_bytes(script_image(script))


if __name__ == '__main__':
    main()
