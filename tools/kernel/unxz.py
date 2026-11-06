"""unxz.py IN.xz OUT [SHA256]: decompress a pinned .xz input, checked against the sha256 of its content."""

import hashlib
import lzma
import sys
from pathlib import Path

data = lzma.decompress(Path(sys.argv[1]).read_bytes())
if len(sys.argv) > 3 and hashlib.sha256(data).hexdigest() != sys.argv[3]:
    sys.exit(f'{sys.argv[1]}: sha256 {hashlib.sha256(data).hexdigest()} is not the pinned {sys.argv[3]}')
Path(sys.argv[2]).write_bytes(data)
