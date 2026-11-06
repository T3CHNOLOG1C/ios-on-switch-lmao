"""inflate.py <raw deflate stream> <output> <sha256>: a deflated zip member's content, checked against its pin.

Run by ipsw_inflate (ipsw/ipsw.bzl) on the bytes Bazel fetched by range request."""

import hashlib
import sys
import zlib

source, output, expected = sys.argv[1:]
inflate, digest = zlib.decompressobj(-15), hashlib.sha256()
with open(source, 'rb') as stored, open(output, 'wb') as out:
    while chunk := stored.read(16 << 20):
        data = inflate.decompress(chunk)
        out.write(data)
        digest.update(data)
    data = inflate.flush()
    out.write(data)
    digest.update(data)
if not inflate.eof or digest.hexdigest() != expected:
    sys.exit('{}: inflated content is {} (eof {}), pinned {}'.format(output, digest.hexdigest(), inflate.eof, expected))
