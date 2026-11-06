"""inflate.py <raw deflate stream> <output> <sha256>: a deflated zip member's content, checked against its pin.

Run by ipsw_inflate (ipsw/ipsw.bzl) on the bytes Bazel fetched by range request. One deflate stream inflates on one
core; hashing and writing what it gives run on a second, so the two overlap.
"""

import hashlib
import queue
import sys
import threading
import zlib

source, output, expected = sys.argv[1:]
inflate, digest = zlib.decompressobj(-15), hashlib.sha256()
pending: queue.Queue = queue.Queue(maxsize=8)


def drain(out: object) -> None:
    while (data := pending.get()) is not None:
        out.write(data)
        digest.update(data)


with open(source, 'rb') as stored, open(output, 'wb') as out:
    writer = threading.Thread(target=drain, args=(out,))
    writer.start()
    try:
        while chunk := stored.read(16 << 20):
            pending.put(inflate.decompress(chunk))
        pending.put(inflate.flush())
    finally:
        pending.put(None)
        writer.join()
if not inflate.eof or digest.hexdigest() != expected:
    sys.exit(f'{output}: inflated content is {digest.hexdigest()} (eof {inflate.eof}), pinned {expected}')
