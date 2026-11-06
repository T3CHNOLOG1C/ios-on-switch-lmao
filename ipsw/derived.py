"""derived.py <file> <output> <sha256>: a file derived offline from the IPSW, copied after checking it against its pin.

Run by ipsw_pretranslated (ipsw/defs.bzl) on the repository's copy, so an edited or stale file fails the build.
"""

import hashlib
import shutil
import sys

source, output, expected = sys.argv[1:]
with open(source, 'rb') as f:
    digest = hashlib.sha256(f.read()).hexdigest()
if digest != expected:
    sys.exit(f'{source}: sha256 {digest}, pinned {expected} (ipsw/pins.bzl derived)')
shutil.copyfile(source, output)
