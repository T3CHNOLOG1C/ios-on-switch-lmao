"""aea_decrypt.py <archive> <output> <key> <sha256>: an AEA member's content, checked against its pin.

Run by ipsw_aea_decrypt (ipsw/ipsw.bzl) on the bytes Bazel fetched by range request. macOS's own /usr/bin/aea does the
decryption, so the image builder needs nothing beyond macOS.
"""

import hashlib
import subprocess
import sys

archive, output, key, expected = sys.argv[1:]
result = subprocess.run(
    ['/usr/bin/aea', 'decrypt', '-i', archive, '-o', output, '-key-value', key], capture_output=True, check=False
)
if result.returncode:
    sys.exit('{}: aea decrypt failed: {}'.format(archive, result.stderr.decode(errors='replace')))
digest = hashlib.sha256()
with open(output, 'rb') as f:
    while chunk := f.read(16 << 20):
        digest.update(chunk)
if digest.hexdigest() != expected:
    sys.exit(f'{output}: decrypted content is {digest.hexdigest()}, pinned {expected}')
