"""The extent plan, applied to a partition full of someone else's bytes, gives back the image wherever APFS reads."""

import os
import struct
import tempfile
import unittest
import uuid

from tools.install import extents, manifest

BLOCK = 4096
SIZE = 64 << 20


def nxsb(desc_base, desc_blocks, data_base, data_blocks):
    block = bytearray(BLOCK)
    block[32:36] = b'NXSB'
    struct.pack_into('<IQ', block, 36, BLOCK, SIZE // BLOCK)
    block[72:88] = bytes(range(16))
    struct.pack_into('<IIQQ', block, 104, desc_blocks, data_blocks, desc_base, data_base)
    return bytes(block)


def apply(plan, image_fd, target):
    """What install-ios.bin does: copy extents from the concatenated sources, zero extents as zeros."""
    out = bytearray(target)
    for at, source, n in plan:
        out[at : at + n] = bytes(n) if source == manifest.ZERO else os.pread(image_fd, n, at)
    return bytes(out)


class ExtentsTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.mkdtemp()
        self.path = os.path.join(directory, 'partition.img')
        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT)
        self.addCleanup(os.close, self.fd)
        os.ftruncate(self.fd, SIZE)

    def write(self, at, data):
        os.pwrite(self.fd, data, at)

    def test_container_reads_checkpoint_areas(self):
        block, count, container, areas = extents.container(nxsb(1, 8, 9, 32))
        self.assertEqual((block, count, container), (BLOCK, SIZE // BLOCK, uuid.UUID(bytes=bytes(range(16)))))
        self.assertEqual(areas, [(BLOCK, 9 * BLOCK), (9 * BLOCK, 41 * BLOCK)])
        with self.assertRaises(extents.ExtentError):
            extents.container(nxsb(1, 8 | extents.NX_NONCONTIGUOUS, 9, 32))
        with self.assertRaises(extents.ExtentError):
            extents.container(bytes(BLOCK))

    def test_plan_rebuilds_data_and_zeroes_checkpoints(self):
        self.write(0, nxsb(1, 8, 9, 32))
        self.write(3 * BLOCK, b'descriptor')
        self.write(5 << 20, os.urandom(3 * BLOCK + 17))
        self.write(40 << 20, os.urandom(1 << 20))
        data = extents.data_ranges(self.fd, 0, SIZE, BLOCK)
        _, _, _, areas = extents.container(os.pread(self.fd, BLOCK, 0))
        plan = extents.plan(data, areas, SIZE, BLOCK)
        self.assertEqual(plan[0][:2], (0, 0))
        stale = os.urandom(SIZE)
        rebuilt = apply(plan, self.fd, stale)
        image = os.pread(self.fd, SIZE, 0)
        for start, end in data + areas:
            self.assertEqual(rebuilt[start:end], image[start:end])
        written = sum(n for _, _, n in plan)
        self.assertLess(written, 4 << 20)
        source = 0
        for _at, src, n in plan:
            if src != manifest.ZERO:
                self.assertEqual(src, source)
                source += n

    def test_plan_joins_extents_to_fit_the_limit(self):
        self.write(0, nxsb(1, 1, 2, 1))
        for i in range(1, 64):
            self.write(i * (1 << 20), b'x')
        data = extents.data_ranges(self.fd, 0, SIZE, BLOCK)
        plan = extents.plan(data, [], SIZE, BLOCK, limit=8)
        self.assertLessEqual(len(plan), 8)
        rebuilt = apply(plan, self.fd, bytes(SIZE))
        self.assertEqual(rebuilt, os.pread(self.fd, SIZE, 0))

    def test_plan_refuses_an_image_without_a_first_block(self):
        self.write(1 << 20, b'x')
        with self.assertRaises(extents.ExtentError):
            extents.plan(extents.data_ranges(self.fd, 0, SIZE, BLOCK), [], SIZE, BLOCK)

    def test_plan_round_trips_through_the_manifest(self):
        self.write(0, nxsb(1, 8, 9, 32))
        self.write(7 << 20, b'y' * BLOCK)
        plan = extents.plan(extents.data_ranges(self.fd, 0, SIZE, BLOCK), [(BLOCK, 41 * BLOCK)], SIZE, BLOCK)
        sizes = manifest.split(plan)
        m = manifest.Manifest(
            SIZE,
            BLOCK,
            extents.container(os.pread(self.fd, BLOCK, 0))[2],
            'test',
            [manifest.Part(f'p{i}', n) for i, n in enumerate(sizes)],
            plan,
            [bytes(32)] * -(-sum(sizes) // manifest.HASH_CHUNK),
        )
        self.assertEqual(manifest.decode(manifest.encode(m)).extents, plan)


if __name__ == '__main__':
    unittest.main()
