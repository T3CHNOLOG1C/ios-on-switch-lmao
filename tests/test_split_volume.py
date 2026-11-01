import json
from pathlib import Path
import tempfile
import unittest

from scripts.split_volume import FAT32_MAX, SplitVolume, pack, part_size


class SplitVolumeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "disk.raw"
        self.data = bytes((i * 11 + i // 512) % 256 for i in range(4096))
        self.source.write_bytes(self.data)
        self.output = self.root / "split"

    def test_exact_bytes_and_reads_spanning_three_parts(self):
        pack(self.source, self.output, 1536)
        volume = SplitVolume(self.output)
        self.assertTrue(volume.verify()["verified"])
        self.assertEqual(volume.read_at(1499, 2000), self.data[1499:3499])
        self.assertEqual(volume.read_at(4096, 0), b"")
        self.assertEqual([p.stat().st_size for p in volume.paths], [1536, 1536, 1024])
        self.assertEqual(self.source.read_bytes(), self.data)

    def test_fat32_limit_is_strict(self):
        for invalid in (0, -512, FAT32_MAX, 2**32, 4_000_000_001):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                part_size(invalid)
        self.assertEqual(part_size(4_000_000_000), 4_000_000_000)

    def test_never_overwrite_existing_output(self):
        self.output.mkdir()
        with self.assertRaises(FileExistsError):
            pack(self.source, self.output, 1024)

    def test_refuses_source_symlink(self):
        link = self.root / "link"
        link.symlink_to(self.source)
        with self.assertRaises(ValueError):
            pack(link, self.output)

    def test_detects_same_size_corruption(self):
        pack(self.source, self.output, 1024)
        target = self.output / "part-0001.bin"
        target.write_bytes(b"x" * 1024)
        with self.assertRaisesRegex(ValueError, "Digest mismatch"):
            SplitVolume(self.output).verify()

    def test_refuses_missing_part_or_gapped_mapping(self):
        pack(self.source, self.output, 1024)
        desc = self.output / "volume.json"
        data = json.loads(desc.read_text())
        data["parts"][1]["offset"] += 512
        desc.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "contiguous"):
            SplitVolume(self.output)

    def test_refuses_path_traversal_and_symlinked_parts(self):
        pack(self.source, self.output, 1024)
        desc = self.output / "volume.json"
        data = json.loads(desc.read_text())
        data["parts"][0]["file"] = "../disk.raw"
        desc.write_text(json.dumps(data))
        with self.assertRaises(ValueError):
            SplitVolume(self.output)
        data["parts"][0]["file"] = "part-0000.bin"
        desc.write_text(json.dumps(data))
        target = self.output / "part-0000.bin"
        target.unlink()
        target.symlink_to(self.source)
        with self.assertRaises(ValueError):
            SplitVolume(self.output)

    def test_refuses_out_of_range_reads(self):
        pack(self.source, self.output, 1024)
        volume = SplitVolume(self.output)
        for offset, length in ((-1, 1), (0, -1), (4095, 2), (4097, 0), (0, 5 * 1024**2)):
            with self.subTest(offset=offset, length=length), self.assertRaises(ValueError):
                volume.read_at(offset, length)


if __name__ == "__main__":
    unittest.main()
