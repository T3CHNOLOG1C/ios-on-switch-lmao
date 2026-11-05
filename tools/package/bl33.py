#!/usr/bin/env python3
"""switchroot's U-Boot (bl33.bin) as this boot entry ships it: its default environment edited, then the staged kernel
window after it (native/loader/stage.h).

    bl33.py patch BL33 OUT                                   the environment edit (once: it never reads the kernel)
    bl33.py stage --u-boot PATCHED --window W --constants C --out OUT
"""
import argparse
import json
from pathlib import Path

from build_native_loader import stage_into_bl33
from loader_abi import STAGE_SOURCE

# U-Boot's built-in default environment in switchroot's bl33.bin (env/common.c default_environment, imported whole at
# start: a run of "name=value\0" ended by an empty string), rewritten in place to the same size so nothing else in the
# binary moves:
#   * boot_targets: SD (mmc1) only, so U-Boot never falls back to the eMMC, even when it cannot read the SD card and
#     hekate's env (bootcmd_mmc0=true above) is never imported; bootcmd_mmc0 itself goes.
#   * two boot timing samples (native/loader/stamps.h) in what runs before hekate's env: bootcmd's start (U-Boot
#     initialized) and mmc_boot once `mmc dev 1` has brought the SD card up.
#   * room for them: four variables nothing on this boot path reads (PXE, ramdisk and DHCP script addresses).
ENV_START = b'\0bootcmd=run distro_bootcmd\0'
ENV_EDITS = {
    b'bootcmd=run distro_bootcmd': b'bootcmd=setexpr.l ios_u_main *0x60005010; run distro_bootcmd',
    b'mmc_boot=if mmc dev ${devnum}; then setenv devtype mmc; run scan_dev_for_boot_part; fi':
        b'mmc_boot=if mmc dev ${devnum}; then setexpr.l ios_u_sd *0x60005010; setenv devtype mmc; '
        b'run scan_dev_for_boot_part; fi',
    b'boot_targets=mmc1 mmc0 ': b'boot_targets=mmc1',
    # r7o: 1016 ms from the SD card up to hekate's env imported, nearly all `part list -bootable` (U-Boot re-reads and
    # re-checks the GPT for every one of its 64 partition indexes). hekate's FAT is always partition 1, and r7o booted
    # from it: scan it directly (no part list, no fstype probe: test -e and load mount it themselves).
    b'scan_dev_for_boot_part=part list ${devtype} ${devnum} -bootable devplist; env exists devplist || setenv devplist 1; '
    b'for distro_bootpart in ${devplist}; do if fstype ${devtype} ${devnum}:${distro_bootpart} bootfstype; then '
    b'run scan_dev_for_boot; fi; done':
        b'scan_dev_for_boot_part=setenv distro_bootpart 1; run scan_dev_for_boot',
}
ENV_DROPS = (b'pxefile_addr_r=0x8ff00000', b'ramdisk_addr_r=0x92000000', b'boot_script_dhcp=boot.scr.uimg',
             b'bootcmd_mmc0=setenv devnum 0; run mmc_boot')


def bl33_patch(bl33: bytes) -> bytes:
    """switchroot's U-Boot with its default environment as ENV_EDITS / ENV_DROPS say; every other byte unchanged."""
    if bl33.count(ENV_START) != 1:
        raise SystemExit('bl33.bin: U-Boot default environment not found exactly once')
    start = bl33.index(ENV_START) + 1
    end = bl33.index(b'\0\0', start) + 1  # the empty string that ends it
    entries = bl33[start:end - 1].split(b'\0')
    if any(entries.count(key) != 1 for key in (*ENV_EDITS, *ENV_DROPS)):
        raise SystemExit('bl33.bin: U-Boot default environment is not the one this patch was written for')
    entries = [ENV_EDITS.get(e, e) for e in entries if e not in ENV_DROPS]
    block = b'\0'.join(entries) + b'\0\0'
    if len(block) > end + 1 - start:
        raise SystemExit('bl33.bin: patched U-Boot environment does not fit')
    return bl33[:start] + block.ljust(end + 1 - start, b'\0') + bl33[end + 1:]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    step = parser.add_subparsers(dest='step', required=True)
    p = step.add_parser('patch')
    p.add_argument('bl33', type=Path)
    p.add_argument('out', type=Path)
    p = step.add_parser('stage')
    for name in ('u-boot', 'window', 'constants', 'out'):
        p.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    if args.step == 'patch':
        args.out.write_bytes(bl33_patch(args.bl33.read_bytes()))
        return
    window, constants = args.window.read_bytes(), json.loads(args.constants.read_text())
    if constants['SOURCE'] != STAGE_SOURCE or constants['PACKED_BYTES'] != len(window):
        raise SystemExit('loader was not built for the bl33-staged window')
    args.out.write_bytes(stage_into_bl33(args.u_boot.read_bytes(), window, constants['STREAM_BYTES']))


if __name__ == '__main__':
    main()
