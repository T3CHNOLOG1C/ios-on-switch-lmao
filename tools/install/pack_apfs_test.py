"""A real APFS container packed, rebuilt from its manifest onto a partition of random bytes, passes fsck_apfs.

Needs hdiutil and fsck_apfs (macOS), so it is tagged manual: bazel test //tools/install:pack_apfs_test.
"""

import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.install import compose, gpt, host, manifest, pack

MIB = 1 << 20


class Quiet:
    def __call__(self, *parts: object) -> None:
        pass

    def quiet(self, *parts: object):
        pass


def run(*command: str):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


class PackApfsTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)

    def container(self):
        udif, raw = self.dir / 'c.dmg', self.dir / 'c'
        run(
            '/usr/bin/hdiutil',
            'create',
            '-size',
            '128m',
            '-fs',
            'APFS',
            '-layout',
            'GPTSPUD',
            '-volname',
            'T',
            str(udif),
        )
        run('/usr/bin/hdiutil', 'convert', str(udif), '-format', 'UDTO', '-o', str(raw))
        dense = Path(str(raw) + '.cdr')
        mount = self.dir / 'mnt'
        mount.mkdir()
        attached = host.plist(
            'hdiutil',
            'attach',
            '-plist',
            '-nobrowse',
            '-mountpoint',
            str(mount),
            '-imagekey',
            'diskimage-class=CRawDiskImage',
            str(dense),
        )
        disk = host.whole_disk_of(attached['system-entities'])
        try:
            (mount / 'data').write_bytes(os.urandom(5 * MIB + 123))
            (mount / 'dir').mkdir()
            (mount / 'dir/small').write_text('small\n')
        finally:
            run('/usr/bin/hdiutil', 'detach', disk)
        sparse = self.dir / 'sparse.img'
        with open(dense, 'rb') as f, open(sparse, 'wb') as g:
            g.truncate(os.path.getsize(dense))
            while chunk := f.read(MIB):
                if chunk.strip(b'\0'):
                    g.seek(f.tell() - len(chunk))
                    g.write(chunk)
        return dense, sparse

    def test_rebuilt_partition_is_clean(self):
        dense, sparse = self.container()
        size = os.path.getsize(dense)
        fd = os.open(sparse, os.O_RDONLY)
        part = gpt.read(fd, size).used[0]
        os.close(fd)
        out = self.dir / 'install'
        out.mkdir()
        kernel = self.dir / 'bl33.bin'
        kernel.write_bytes(b'kernel')
        layout = {'container_bytes': part.bytes, 'minimum_bytes': part.bytes, 'pool_start': 0, 'pool_end': 0}
        source = pack.Source(sparse, part.first * 512, layout)
        m = pack.pack(source, pack.Target(out, 'switchroot/ios', 'IOS17', 'test', [('bl33.bin', kernel)]), Quiet())
        with open(out / 'IOS17.SII.001', 'rb') as f:
            self.assertEqual(manifest.decode(f.read(manifest.BOOT_OFFSET)), m)
        stream = b''.join((out / Path(p.path).name).read_bytes() for p in m.parts)
        source = stream[manifest.BODY_OFFSET :]
        chunk = manifest.HASH_CHUNK
        self.assertEqual(
            m.hashes, [hashlib.sha256(source[i : i + chunk]).digest() for i in range(0, len(source), chunk)]
        )
        rebuilt = self.dir / 'rebuilt.img'
        with open(dense, 'rb') as f, open(rebuilt, 'wb') as g:
            g.write(os.urandom(size))
            g.seek(0)
            g.write(f.read(part.first * 512))
            f.seek(size - 33 * 512)
            g.seek(size - 33 * 512)
            g.write(f.read())
            for at, start, n in m.extents:
                g.seek(part.first * 512 + at)
                g.write(bytes(n) if start == manifest.ZERO else source[start : start + n])
        disk = compose.attach_raw(rebuilt, Quiet())
        try:
            result = subprocess.run(
                ['/sbin/fsck_apfs', '-n', f'/dev/r{disk}s1'], capture_output=True, text=True, check=False
            )
        finally:
            run('/usr/bin/hdiutil', 'detach', disk)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('appears to be OK', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
