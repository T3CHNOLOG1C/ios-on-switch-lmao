#!/usr/bin/env python3
"""Mark launchd jobs disabled in the Data volume's launchd override databases (user 501 and system), merging with what
is there. Usage: launchd-disable.py <Data mount> <label>...  Jobs: CPU-waste audit 10-31 (tailspind's always-on ktrace,
webbookmarksd's launch-constraint respawn loop every 10 s)."""
import plistlib, sys
from pathlib import Path
data, labels = Path(sys.argv[1]), sys.argv[2:]
folder = data / 'db/com.apple.xpc.launchd'
folder.mkdir(parents=True, exist_ok=True)
for name in ('disabled.501.plist', 'disabled.plist'):
    path = folder / name
    entries = plistlib.loads(path.read_bytes()) if path.exists() else {}
    for label in labels:
        entries[label] = True
    path.write_bytes(plistlib.dumps(entries, fmt=plistlib.FMT_BINARY))
    print(path, sorted(k for k, v in entries.items() if v is True))
