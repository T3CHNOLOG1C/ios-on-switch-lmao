"""The kernel's live log: XNU's msgbuf, read through a core's MMU, for runs that end without a panic record."""

import re
import struct
import subprocess
import tempfile
from pathlib import Path

from tools.emu.monitor import Monitor

MSG_MAGIC = 0x063061
KERNEL_VA = 0xFFFFFFF000000000


def symbol(kernel: Path, name: str) -> int | None:
    out = subprocess.run(['/usr/bin/nm', '-gU', str(kernel)], capture_output=True, text=True, check=False).stdout
    found = re.search(rf'^([0-9a-f]+) \w {re.escape(name)}$', out, re.MULTILINE)
    return int(found.group(1), 16) if found else None


def kernel_cpu(registers: str) -> int | None:
    """Return a core that was running the kernel at EL1, whose page tables reach the kernel's heap."""
    for index, block in enumerate(registers.split('CPU#')[1:]):
        pc = re.search(r'PC=([0-9a-f]+)', block)
        if pc and int(pc.group(1), 16) >= KERNEL_VA and 'EL1' in block:
            return index
    return None


def _read(monitor: Monitor, cpu: int, address: int, size: int) -> bytes:
    with tempfile.NamedTemporaryFile() as f:
        monitor.command('memsave', val=address, size=size, filename=f.name, **{'cpu-index': cpu})
        return Path(f.name).read_bytes()


def read(monitor: Monitor, kernel: Path, registers: str) -> bytes | None:
    """Return the msgbuf ring, oldest byte first, or None when no core or symbol reaches it."""
    pointer, cpu = symbol(kernel, '_msgbufp'), kernel_cpu(registers)
    if pointer is None or cpu is None:
        return None
    try:
        (msgbuf,) = struct.unpack('<Q', _read(monitor, cpu, pointer, 8))
        magic, size, write, _, ring = struct.unpack('<iiiiQ', _read(monitor, cpu, msgbuf, 24))
        if magic != MSG_MAGIC or not 0 < size <= 16 << 20 or not 0 <= write < size:
            return None
        data = _read(monitor, cpu, ring, size)
    except RuntimeError:
        return None
    return data[write:] + data[:write]
