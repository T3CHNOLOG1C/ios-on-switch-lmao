"""The AEA key unwrap against an archive this test wraps, and aea_decrypt.py against macOS's own aea."""

import base64
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

from cryptography.hazmat.primitives import hpke, serialization
from cryptography.hazmat.primitives.asymmetric import ec

from ipsw import aea

KEY_URL = 'https://wkms-public.apple.com/fcs-keys/test'


def field(key: str, value: bytes) -> bytes:
    """Return one auth data field: u32 length, key, NUL, value."""
    body = key.encode() + b'\0' + value
    return struct.pack('<I', 4 + len(body)) + body


def wrapped(symmetric: bytes) -> tuple[bytes, bytes]:
    """Return auth data wrapping symmetric to a new P-256 key, and that key's PEM."""
    private = ec.generate_private_key(ec.SECP256R1())
    suite = hpke.Suite(hpke.KEM.P256, hpke.KDF.HKDF_SHA256, hpke.AEAD.AES_256_GCM)
    sealed = suite.encrypt(symmetric, private.public_key())
    response = {
        'enc-request': base64.b64encode(sealed[:65]).decode(),
        'wrapped-key': base64.b64encode(sealed[65:]).decode(),
    }
    blob = field('com.apple.wkms.fcs-response', json.dumps(response).encode()) + field(
        'com.apple.wkms.fcs-key-url', KEY_URL.encode()
    )
    pem = private.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
    )
    return blob, pem


class Unwrap(unittest.TestCase):
    def test_key_value(self):
        symmetric = os.urandom(32)
        blob, pem = wrapped(symmetric)
        fetched = []
        key = aea.key_value(aea.auth_fields(blob), fetch=lambda url: fetched.append(url) or pem)
        self.assertEqual(key, 'base64:' + base64.b64encode(symmetric).decode())
        self.assertEqual(fetched, [KEY_URL])

    def test_prologue(self):
        self.assertEqual(aea.prologue(b'AEA1\x01\x00\x00\x00\x80\x08\x00\x00'), (1, 0x880))
        with self.assertRaises(ValueError):
            aea.prologue(b'AA01' + bytes(8))

    def test_truncated_field(self):
        with self.assertRaises(ValueError):
            aea.auth_fields(struct.pack('<I', 64) + b'key\0value')


@unittest.skipUnless(sys.platform == 'darwin', '/usr/bin/aea is macOS')
class Decrypt(unittest.TestCase):
    def run_tool(self, archive: str, output: str, key: str, sha256: str) -> subprocess.CompletedProcess:
        tool = os.path.join(os.path.dirname(__file__), '..', 'aea_decrypt.py')
        return subprocess.run([sys.executable, tool, archive, output, key, sha256], capture_output=True, check=False)

    def test_round_trip(self):
        key = 'base64:' + base64.b64encode(os.urandom(32)).decode()
        content = os.urandom(3 << 20)
        with tempfile.TemporaryDirectory() as d:
            plain, archive, out = (os.path.join(d, n) for n in ('plain', 'archive.aea', 'out'))
            with open(plain, 'wb') as f:
                f.write(content)
            subprocess.run(
                [
                    '/usr/bin/aea',
                    'encrypt',
                    '-profile',
                    'hkdf_sha256_aesctr_hmac__symmetric__none',
                    '-key-value',
                    key,
                    '-i',
                    plain,
                    '-o',
                    archive,
                ],
                check=True,
            )
            good = hashlib.sha256(content).hexdigest()
            self.assertEqual(self.run_tool(archive, out, key, good).returncode, 0)
            with open(out, 'rb') as f:
                self.assertEqual(f.read(), content)
            self.assertNotEqual(self.run_tool(archive, out, key, '0' * 64).returncode, 0)
            other = 'base64:' + base64.b64encode(os.urandom(32)).decode()
            self.assertNotEqual(self.run_tool(archive, out, other, good).returncode, 0)


if __name__ == '__main__':
    unittest.main()
