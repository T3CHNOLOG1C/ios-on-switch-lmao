"""What the packaging reads of the kernel's configuration: the boot-args (boot_args.py's output) and the display mode
(IOSwitchPortrait.h)."""

import re
from pathlib import Path


def read_command_line(path) -> str:
    """The boot-args file boot_args.py wrote."""
    return Path(path).read_text().rstrip('\n')


def boot_arg_on(command_line: str, name: str) -> bool:
    """True when the command line sets `name` to a non-zero value."""
    return any(arg.startswith(name + '=') and arg.split('=', 1)[1] not in ('0', '') for arg in command_line.split())


def portrait_native(header) -> bool:
    """True when the kernel is built portrait-native: IOSwitchPortrait.h's default for IOSWITCH_PORTRAIT_NATIVE."""
    found = re.search(r'^#define IOSWITCH_PORTRAIT_NATIVE ([01])\b', Path(header).read_text(), re.M)
    if not found:
        raise SystemExit(f'{header}: no IOSWITCH_PORTRAIT_NATIVE default')
    return found.group(1) == '1'
