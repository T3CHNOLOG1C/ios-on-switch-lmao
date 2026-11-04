#!/usr/bin/env python3
"""Show the launchd override database on a Data volume; read-only. Usage: launchd-disable.py <Data mount> [label...]

21H461 launchd reads one override database, db/com.apple.xpc.launchd/disabled.plist (bare label -> true), once in early
boot. The system and user/501 domains share it, and launchd never reads a disabled.<uid>.plist. The kernel rewrites
disabled.plist at every boot (XNU bsd/kern/kern_exec.c native_ioswitch_watchdog_policy), so anything written here at
deploy time is lost. To disable a job, add it to the NATIVE_IOSWITCH_POLICY_* lists in that function. This tool no longer
writes: it prints the jobs the database disables and exits 1 if one of the given labels is not among them. It also
reports a disabled.501.plist left by earlier versions of this tool: launchd ignores it."""
import plistlib, sys
from pathlib import Path
if len(sys.argv) < 2:
    sys.exit(__doc__)
data, labels = Path(sys.argv[1]), sys.argv[2:]
folder = data / 'db/com.apple.xpc.launchd'
path = folder / 'disabled.plist'
entries = plistlib.loads(path.read_bytes()) if path.exists() else {}
disabled = sorted(k for k, v in entries.items() if v is True or (isinstance(v, dict) and v.get('Disabled') is True))
print(path, disabled)
stale = folder / 'disabled.501.plist'
if stale.exists():
    print(stale, 'is never read by launchd (no per-user database on iOS)')
missing = [label for label in labels if label not in disabled]
if missing:
    sys.exit(f'not disabled: {missing}: add them to native_ioswitch_watchdog_policy in kern_exec.c')
