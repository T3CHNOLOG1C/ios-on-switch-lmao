"""An IPSW member's Apple Encrypted Archive key: the symmetric key macOS's `aea decrypt -key-value` takes.

The archive's auth data carries the key wrapped with HPKE (P-256, HKDF-SHA256, AES-256-GCM) to a private key that
Apple serves at the archive's fcs-key-url.
"""

import base64
import json
import struct
import urllib.request
from collections.abc import Callable

from cryptography.hazmat.primitives import hpke, serialization

MAGIC = b'AEA1'
PROLOGUE = 12
SYMMETRIC_PROFILE = 1


def prologue(head: bytes) -> tuple[int, int]:
    """Return the archive's profile and the length of the auth data that follows the 12-byte prologue."""
    if head[:4] != MAGIC:
        raise ValueError(f'not an AEA archive: {head[:4]!r}')
    return int.from_bytes(head[4:7], 'little'), struct.unpack_from('<I', head, 8)[0]


def auth_fields(blob: bytes) -> dict[str, bytes]:
    """Return the auth data's fields: each is a u32 length (its own four bytes included), a key, NUL, the value."""
    fields, at = {}, 0
    while at < len(blob):
        length = struct.unpack_from('<I', blob, at)[0]
        if length < 5 or at + length > len(blob):
            raise ValueError(f'bad AEA auth field length {length} at {at}')
        key, separator, value = blob[at + 4 : at + length].partition(b'\0')
        if not separator:
            raise ValueError(f'AEA auth field at {at} has no key')
        fields[key.decode()] = value
        at += length
    return fields


def unwrap(fields: dict[str, bytes], private_key_pem: bytes) -> bytes:
    """Return the archive's symmetric key, unwrapped from the fcs-response with the fcs-key-url's private key."""
    response = json.loads(fields['com.apple.wkms.fcs-response'])
    sealed = base64.b64decode(response['enc-request']) + base64.b64decode(response['wrapped-key'])
    suite = hpke.Suite(hpke.KEM.P256, hpke.KDF.HKDF_SHA256, hpke.AEAD.AES_256_GCM)
    return suite.decrypt(sealed, serialization.load_pem_private_key(private_key_pem, None))


def key_value(fields: dict[str, bytes], fetch: Callable[[str], bytes] | None = None) -> str:
    """Return the key as `aea decrypt -key-value` takes it, fetching the private key from the fcs-key-url."""
    url = fields['com.apple.wkms.fcs-key-url'].decode()
    if fetch is None:
        with urllib.request.urlopen(url) as response:
            pem = response.read()
    else:
        pem = fetch(url)
    return 'base64:' + base64.b64encode(unwrap(fields, pem)).decode()
