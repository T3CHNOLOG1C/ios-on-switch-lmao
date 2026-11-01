"""Keep native boot output limited to stage progress and diagnostic failures."""
from __future__ import annotations

# A success-only branch still needs a command for U-Boot's hush parser.
_QUIET_SUCCESS = {
    'Pinned firmware instruction fingerprint matches',
    'Live framebuffer address, geometry and format match',
    'Native image FAT metadata size matches',
    'OLED DTB FAT metadata size matches',
    'Kernel FAT metadata size matches',
}
_REMOVE = {
    'IOSWITCH XNU STAGE 0: SCRIPT ENTERED',
    'IOSWITCH HANDOFF 2 STAGE 0: SCRIPT ENTERED',
    'IOSwitch native XNU kernel experiment - OLED only',
    'This starts the experimental XNU kernel. iPadOS is not ready.',
    'IOSwitch native kernel handoff diagnostic - OLED only',
    'This diagnostic does not start XNU or iPadOS',
    'Recovery: hold POWER at least 20 seconds, then start Hekate normally',
    'The kernel must replace the loader screen to prove XNU entry',
    'The next screen must be drawn by the native diagnostic',
    'Photograph the last stage. Hold POWER at least 20 seconds to turn off.',
    'This script holds without selecting another boot target.',
}
_LABELS = {
    'STAGE 1: SCREEN OUTPUT SELECTED': 'BOOT',
    ': SCREEN SELECTION FAILED - HOLDING': 'CONSOLE FAILED',
    'STAGE 2: EXACT NATIVE IMAGE VERIFIED': 'LOADER READY',
    'STAGE 3: EXACT OLED DTB VERIFIED': 'DTB READY',
    'STAGE 3B: EXACT KERNEL IMAGE VERIFIED': 'KERNEL READY',
    'STAGE 3B: KERNEL IMAGE LOADED': 'KERNEL LOADED',
    'STAGE 4: FDT PADDED; FRAMEBUFFER RESERVED': 'FDT READY',
    'STAGE 5: NATIVE IMAGE LOADED': 'LOADER LOADED',
    'STAGE 6: BEFORE FDT PREPARATION': 'PREPARE FDT',
    'STAGE 7: FDT PREPARED; BEFORE NATIVE HANDOFF': 'HANDOFF',
    'REFUSED: ${handoff_status}': 'REFUSED: ${handoff_status}',
    'IOSWITCH SD: CARD METADATA EXPORTER READY': 'SD EXPORTER READY',
    'IOSWITCH SD: BEFORE CARD METADATA EXPORT': 'READ SD METADATA',
    'IOSWITCH SD: SECTOR ZERO AND CARD CAPACITY READY': 'SD METADATA READY',
    'IOSWITCH SD: SAME-BOOT SECTOR ZERO REFERENCE READY': 'SD REFERENCE READY',
    'Next two firmware helper rc=1 values are expected': 'CACHE HELPERS: RC=1 EXPECTED',
}


def minimal_boot_text(script: str) -> str:
    lines = []
    for line in script.splitlines():
        stripped = line.strip()
        command = next((cmd for cmd in ('echoe', 'echo') if stripped.startswith(cmd + ' "') and stripped.endswith('"')), None)
        if command is None:
            lines.append(line)
            continue
        message = stripped[len(command) + 2:-1]
        indent = line[:len(line) - len(line.lstrip())]
        if message in _REMOVE:
            continue
        if message in _QUIET_SUCCESS:
            lines.append(indent + 'true')
            continue
        for old, new in _LABELS.items():
            if message == old or any(message == prefix + old for prefix in ('IOSWITCH XNU ', 'IOSWITCH HANDOFF 2 ', 'IOSWITCH XNU', 'IOSWITCH HANDOFF 2')):
                message = new
                break
        lines.append(indent + command + ' "' + message + '"')
    return '\n'.join(lines) + '\n'
