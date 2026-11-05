#!/usr/bin/env python3
"""The boot entry's U-Boot script: boot.cmd from the template (boot/raw-v24-boot.cmd, the last hardware-run raw script)
and boot.scr, the same as a legacy image.

    boot_script.py --template T --loader LOADER.UBOOT --bl33 BL33 --window W --constants C --cmd OUT --scr OUT

//boot:package writes them from loader_link.py (write_boot_script: the loader's size, bl33.bin's from the window's
constants, loader_abi.staged_bl33_bytes), one step fewer after the kernel window; this CLI is the same from the files.
"""
import argparse
import json
import re
import struct
from pathlib import Path

from loader_abi import STAGE_SOURCE, staged_bl33_bytes
from uimage import legacy_image

# The entry's folder on the SD card's FAT (switchroot/<NAME>/) and its hekate ini (bootloader/ini/<NAME>.ini).
NAME = 'ios'


def boot_script(text: str, loader_bytes: int, bl33: bytes | int, constants: dict, staged_bytes: int) -> str:
    """boot.cmd from the template (TEMPLATE): this entry's paths, the loader's size, and bl33.bin's size (bl33: the file
    or its size), header and trailer words for the staged kernel window (boot/loader/stage.h)."""
    bl33_bytes = bl33 if isinstance(bl33, int) else len(bl33)
    prefix = '/switchroot/' + NAME + '/'
    for old in ('/switchroot/ios-native-xnu-v23/', '/switchroot/ios-native-xnu-v10/'):
        text = text.replace(old, prefix)
    text = text.replace('0x1c040', hex(loader_bytes))
    trailer = STAGE_SOURCE + staged_bytes
    for name, value in (('BL33_BYTES', bl33_bytes), ('STAGE_STREAM', constants['STREAM_BYTES']),
                        ('STAGE_BYTES', staged_bytes), ('STAGE_ID_LOW', constants['STAGE_ID'] & 0xFFFFFFFF),
                        ('STAGE_ID_HIGH', constants['STAGE_ID'] >> 32),
                        *((f'TRAILER_{i}', trailer + 4 * i) for i in range(6))):
        if f'@{name}@' not in text:
            raise SystemExit(f'boot script template has no @{name}@')
        text = text.replace(f'@{name}@', hex(value))
    # The command line lives in the layout, so the template's fixed-address args patch goes.
    text, removed = re.subn(r'\n# Select raw only.*?\nfi\n', '\n', text, flags=re.S)
    if (removed != 1 or re.search(r'/switchroot/(?!' + NAME + '/)', text) or 'raw-args' in text or
            'crc32 -v' in text or '@' in text or 'kernel-window.bin' in text):
        raise SystemExit('boot script template did not match')
    return text


def write_boot_script(template: Path, loader_bytes: int, bl33: bytes | int, constants: dict, staged_bytes: int,
                      cmd: Path, scr: Path) -> None:
    """boot.cmd and boot.scr (the script as a legacy image) for these sizes."""
    script = boot_script(template.read_text(), loader_bytes, bl33, constants, staged_bytes).encode('ascii')
    cmd.write_bytes(script)
    scr.write_bytes(legacy_image(struct.pack('>II', len(script), 0) + script, os_type=17, kind=6, name='Boot'))


def write_for_loader(template: Path, loader_bytes: int, constants: dict, cmd: Path, scr: Path) -> None:
    """boot.cmd and boot.scr from the loader's size and the window's constants alone: bl33.bin's size follows from the
    staged window's (staged_bl33_bytes; bl33.py's stage refuses any other)."""
    staged = constants['PACKED_BYTES']
    write_boot_script(template, loader_bytes, staged_bl33_bytes(staged), constants, staged, cmd, scr)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('template', 'loader', 'bl33', 'window', 'constants', 'cmd', 'scr'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    write_boot_script(args.template, args.loader.stat().st_size, args.bl33.read_bytes(),
                      json.loads(args.constants.read_text()), args.window.stat().st_size, args.cmd, args.scr)


if __name__ == '__main__':
    main()
