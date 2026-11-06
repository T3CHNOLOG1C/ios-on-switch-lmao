"""A delta rewrites files in place (inode and mode kept), resumes, and refuses files it was not made from."""

import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path

from tools.install import delta


def sha(data: bytes):
    return hashlib.sha256(data).hexdigest()


def quiet(*parts: object):
    pass


class DeltaTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.volume = self.dir / 'volume'
        (self.volume / 'usr/libexec').mkdir(parents=True)
        (self.volume / 'private/var/staged').mkdir(parents=True)
        self.tool = self.volume / 'usr/libexec/tool'
        self.tool.write_bytes(b'arm64e')
        self.tool.chmod(0o751)
        (self.volume / 'private/var/staged/app').write_bytes(b'app arm64e')
        out = self.dir / 'delta'
        (out / 'files').mkdir(parents=True)
        (out / 'files/tool').write_bytes(b'arm64 translated')
        (out / 'files/app').write_bytes(b'app arm64')
        entries = [
            {'path': '/usr/libexec/tool', 'file': 'files/tool', 'sha256_in': sha(b'arm64e'),
             'sha256_out': sha(b'arm64 translated')},
            {'path': '/private/var/staged/app', 'file': 'files/app', 'sha256_in': sha(b'app arm64e'),
             'sha256_out': sha(b'app arm64')},
        ]  # fmt: skip
        (out / 'os.manifest.json').write_text(json.dumps({'role': 'os', 'translator': 'test', 'files': entries}))
        self.entries = delta.load(out / 'os.manifest.json')

    def test_rewrites_in_place(self):
        inode = os.stat(self.tool).st_ino
        self.assertEqual(delta.apply(self.volume, self.entries, quiet, 'System'), 2)
        self.assertEqual(self.tool.read_bytes(), b'arm64 translated')
        self.assertEqual(os.stat(self.tool).st_ino, inode)
        self.assertEqual(self.tool.stat().st_mode & 0o777, 0o751)

    def test_consume_deletes_each_replacement_once_written(self):
        delta.apply(self.volume, self.entries, quiet, 'System', consume=True)
        self.assertFalse(any(e['source'].exists() for e in self.entries))
        self.assertEqual(delta.apply(self.volume, self.entries, quiet, 'System'), 0)

    def test_resumes(self):
        delta.apply(self.volume, self.entries, quiet, 'System')
        self.assertEqual(delta.apply(self.volume, self.entries, quiet, 'System'), 0)

    def test_refuses_another_file(self):
        self.tool.write_bytes(b'something else')
        with self.assertRaisesRegex(delta.DeltaError, 'not the file the delta was made from'):
            delta.apply(self.volume, self.entries, quiet, 'System')

    def test_data_takes_the_entries_below_private_var(self):
        data = self.dir / 'data'
        (data / 'staged').mkdir(parents=True)
        (data / 'staged/app').write_bytes(b'app arm64e')
        below = delta.entries_under(self.entries, '/private/var')
        self.assertEqual([e['path'] for e in below], ['/staged/app'])
        self.assertEqual(delta.apply(data, below, quiet, 'Data'), 1)
        self.assertEqual((data / 'staged/app').read_bytes(), b'app arm64')

    def test_refuses_relative_paths(self):
        manifest = self.dir / 'bad.manifest.json'
        manifest.write_text(
            json.dumps({'files': [{'path': 'usr/../etc', 'file': 'x', 'sha256_in': '', 'sha256_out': ''}]})
        )
        with self.assertRaises(delta.DeltaError):
            delta.load(manifest)


if __name__ == '__main__':
    unittest.main()
