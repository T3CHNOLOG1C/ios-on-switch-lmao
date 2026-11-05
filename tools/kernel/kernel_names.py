"""Symbol names an archive defines (nm).

For the archives namespaced against the kernel's names (corecrypto, libpthread); the kernel's own list is
//kernel:xnu_symbols's kernel-names.txt.
"""

import subprocess
from pathlib import Path


def archive_names(path: Path | str, mode: str, env: dict[str, str], global_only: bool = True) -> set[str]:
    """Return the names nm lists for an archive (mode: -U for defined, -u for undefined; global_only: -g)."""
    command = ['xcrun', 'nm', *(['-g'] if global_only else []), mode, '-j', str(path)]
    output = subprocess.check_output(command, text=True, env=env)
    return {line.strip() for line in output.splitlines() if line.startswith('_')}
