"""What the packaging reads of the kernel's configuration: the display mode (SwitchPortrait.h)."""

import re
from pathlib import Path


def portrait_native(header) -> bool:
    """True when the kernel is built portrait-native: SwitchPortrait.h's default for SWITCH_PORTRAIT_NATIVE."""
    found = re.search(r'^#define SWITCH_PORTRAIT_NATIVE ([01])\b', Path(header).read_text(), re.M)
    if not found:
        raise SystemExit(f'{header}: no SWITCH_PORTRAIT_NATIVE default')
    return found.group(1) == '1'
