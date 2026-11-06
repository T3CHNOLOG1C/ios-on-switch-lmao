"""Which partition of a Switch card becomes (or already is) iOS. boot/installer/target.c applies the same rules.

A fresh card has the partition the user made in hekate's partition manager: Linux ("l4t") or Android ("userdata", or
"UDA" in the legacy layout). An installed card has one APFS-typed or "iOS"-named entry; "iOS-setup" marks one that
install-ios.bin started writing and has not finished.
"""

from dataclasses import dataclass

FRESH_NAMES = ('l4t', 'userdata', 'UDA')
SETUP_NAME = 'iOS-setup'
# kern_exec.c native_owner_data_prepare refuses a Data store above 1 TiB or not 4 KiB aligned.
MAX_BYTES = 1 << 40
ALIGN_BYTES = 4096


class TargetError(Exception):
    pass


@dataclass
class Candidate:
    index: int
    name: str
    apfs: bool
    bytes: int = 0


def installed(entry: Candidate, ios_name: str) -> bool:
    return entry.apfs or entry.name in (ios_name, SETUP_NAME)


def fresh(entry: Candidate) -> bool:
    return entry.name in FRESH_NAMES


def select(entries: list[Candidate], ios_name: str, partition: str | None = None) -> Candidate:
    """Return the one target among `entries` (Candidates, the FAT and emuMMC already left out), or raise TargetError."""
    ios = [e for e in entries if installed(e, ios_name)]
    if len(ios) > 1:
        raise TargetError('more than one APFS or iOS partition: {}'.format(', '.join(e.name for e in ios)))
    if partition is not None:
        named = [e for e in entries if e.name == partition and (installed(e, ios_name) or fresh(e))]
        if len(named) != 1:
            raise TargetError(f'no single Linux, Android or iOS partition named {partition!r}')
        target = named[0]
        if ios and ios[0] is not target:
            raise TargetError(f'the card already has iOS on {ios[0].name!r}')
    elif ios:
        target = ios[0]
    else:
        choices = [e for e in entries if fresh(e)]
        if len(choices) != 1:
            raise TargetError(
                'expected one Linux (l4t) or Android (userdata, UDA) partition, found {}{}'.format(
                    [e.name for e in choices] or 'none', '; choose one with --partition' if choices else ''
                )
            )
        target = choices[0]
    check_size(target.bytes)
    return target


def check_size(size: int) -> None:
    if size <= 0 or size % ALIGN_BYTES or size > MAX_BYTES:
        raise TargetError(
            f'a partition of {size} bytes cannot hold iOS '
            f'(the kernel takes up to {MAX_BYTES} bytes in {ALIGN_BYTES}-byte units)'
        )
