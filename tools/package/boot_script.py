#!/usr/bin/env python3
"""The boot entry's U-Boot script: boot.cmd from the template (boot/raw-v24-boot.cmd, the last hardware-run raw script)
and boot.scr, the same as a legacy image.

    boot_script.py --template T --loader LOADER.UBOOT --bl33 BL33 --window W --constants C --cmd OUT --scr OUT
"""
import argparse
import json
import re
import struct
from pathlib import Path

from loader_abi import STAGE_SOURCE
from uimage import legacy_image

# The entry's folder on the SD card's FAT (switchroot/<NAME>/) and its hekate ini (bootloader/ini/<NAME>.ini).
NAME = 'ioswitch'


def boot_script(text: str, loader_bytes: int, bl33: bytes, constants: dict, staged_bytes: int) -> str:
    """boot.cmd from the template (TEMPLATE): this entry's paths, the loader's size, and bl33.bin's size, header and
    trailer words for the staged kernel window (native/loader/stage.h)."""
    prefix = '/switchroot/' + NAME + '/'
    for old in ('/switchroot/ioswitch-native-xnu-v23/', '/switchroot/ioswitch-native-xnu-v10/'):
        text = text.replace(old, prefix)
    text = text.replace('0x1c040', hex(loader_bytes))
    trailer = STAGE_SOURCE + staged_bytes
    for name, value in (('BL33_BYTES', len(bl33)), ('STAGE_STREAM', constants['STREAM_BYTES']),
                        ('STAGE_BYTES', staged_bytes), ('STAGE_ID_LOW', constants['STAGE_ID'] & 0xFFFFFFFF),
                        ('STAGE_ID_HIGH', constants['STAGE_ID'] >> 32),
                        *((f'TRAILER_{i}', trailer + 4 * i) for i in range(6))):
        if f'@{name}@' not in text:
            raise SystemExit(f'boot script template has no @{name}@')
        text = text.replace(f'@{name}@', hex(value))
    # The command line now lives in the layout; drop the fixed-address args patch.
    text, removed = re.subn(r'\n# Select raw only.*?\nfi\n', '\n', text, flags=re.S)
    if (removed != 1 or re.search(r'/switchroot/(?!' + NAME + '/)', text) or 'raw-args' in text or
            'crc32 -v' in text or '@' in text or 'kernel-window.bin' in text):
        raise SystemExit('boot script template did not match')
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('template', 'loader', 'bl33', 'window', 'constants', 'cmd', 'scr'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    text = boot_script(
        args.template.read_text(),
        args.loader.stat().st_size,
        args.bl33.read_bytes(),
        json.loads(args.constants.read_text()),
        args.window.stat().st_size,
    )
    script = text.encode('ascii')
    args.cmd.write_bytes(script)
    args.scr.write_bytes(legacy_image(struct.pack('>II', len(script), 0) + script, os_type=17, kind=6, name='Boot'))


if __name__ == '__main__':
    main()
