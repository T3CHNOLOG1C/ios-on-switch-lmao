import json
import io
from pathlib import Path
import socket
import struct
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch
import zlib

from scripts.observe import QMP, memory_sample, ppm_to_png


class ScreenshotTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.raw = self.base / "frame.ppm"
        self.png = self.base / "frame.png"

    def test_rgb_pixels_are_preserved_including_leading_whitespace(self):
        pixels = b"\x0a\x20\x09\xff\x00\x7f"
        self.raw.write_bytes(b"P6\n# QEMU image\n2 1\n255\n" + pixels)
        ppm_to_png(self.raw, self.png)
        png = self.png.read_bytes()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        offset = 8
        raster = b""
        while offset < len(png):
            size = struct.unpack_from(">I", png, offset)[0]
            kind = png[offset + 4:offset + 8]
            body = png[offset + 8:offset + 8 + size]
            crc = struct.unpack_from(">I", png, offset + 8 + size)[0]
            self.assertEqual(crc, zlib.crc32(kind + body) & 0xffffffff)
            if kind == b"IDAT":
                raster += body
            offset += 12 + size
        self.assertEqual(zlib.decompress(raster), b"\0" + pixels)

    def test_bad_raster_produces_no_png(self):
        for data in (b"P6\n1 1\n255\n\x00", b"P6\n4097 1\n255\n", b"P6\n1 1\n65535\n"):
            self.raw.write_bytes(data)
            with self.assertRaises(ValueError):
                ppm_to_png(self.raw, self.png)
            self.assertFalse(self.png.exists())

    def test_existing_capture_is_preserved(self):
        self.raw.write_bytes(b"P6\n1 1\n255\n\x00\x00\x00")
        self.png.write_bytes(b"keep")
        with self.assertRaises(FileExistsError):
            ppm_to_png(self.raw, self.png)
        self.assertEqual(self.png.read_bytes(), b"keep")


class QMPTests(unittest.TestCase):
    def test_non_object_reply_is_a_recordable_error(self):
        for reply in (b"[]\n", b"true\n", b"null\n"):
            client = QMP.__new__(QMP)
            client.stream = io.BytesIO(reply)
            with self.subTest(reply=reply), self.assertRaisesRegex(ValueError, "must be an object"):
                client.receive()

    def test_unrelated_events_share_one_command_deadline(self):
        client = QMP.__new__(QMP)
        client.sequence = 0
        client.socket = Mock()
        client.stream = io.BytesIO(b'{"event":"RESET"}\n' * 2)
        with patch("scripts.observe.time.monotonic", side_effect=[0, 1, 3]):
            with self.assertRaisesRegex(TimeoutError, "deadline exceeded"):
                client.command("screendump")

    def test_failed_screendump_removes_its_partial_intermediate(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "frame.png"
            raw = destination.with_suffix(".ppm")
            client = QMP.__new__(QMP)

            def partial_dump(*_):
                raw.write_bytes(b"P6\n")
                raise ValueError("dump failed")

            client.command = partial_dump
            with self.assertRaisesRegex(ValueError, "dump failed"):
                client.screenshot(destination)
            self.assertFalse(raw.exists())
            self.assertFalse(destination.exists())

    def test_events_and_command_ids_and_errors(self):
        with tempfile.TemporaryDirectory(prefix="iqmp-", dir="/tmp") as temporary:
            path = Path(temporary) / "qmp"
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.addCleanup(server.close)
            server.bind(str(path))
            server.listen(1)
            errors = []

            def serve():
                try:
                    connection, _ = server.accept()
                    with connection, connection.makefile("rb") as incoming:
                        connection.sendall(b'{"QMP": {}}\n')
                        for _ in range(2):
                            request = json.loads(incoming.readline())
                            connection.sendall(b'{"event":"RESET"}\n')
                            reply = {"id": request["id"]}
                            if request["execute"] == "qmp_capabilities":
                                reply["return"] = {}
                            else:
                                reply["error"] = {"class": "GenericError", "desc": "no display"}
                            connection.sendall(json.dumps(reply).encode() + b"\n")
                except BaseException as exc:
                    errors.append(exc)

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            client = QMP(path)
            try:
                with self.assertRaisesRegex(ValueError, "no display"):
                    client.command("screendump", {"filename": str(Path(temporary) / "frame.ppm")})
            finally:
                client.close()
            thread.join(5)
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])


class MemoryTests(unittest.TestCase):
    def test_linux_samples_use_kib_and_leave_unavailable_unknown(self):
        def read(path):
            if path.name == "status":
                return "VmRSS:\t512 kB\nVmHWM:\t1024 kB\nVmSwap:\t256 kB\n"
            return "Pss: 400 kB\nSwapPss: 200 kB\n"
        with patch("scripts.observe.platform.system", return_value="Linux"), patch.object(Path, "read_text", read):
            self.assertEqual(memory_sample(123), {"rss_kib": 512, "rss_high_water_kib": 1024,
                                                "swap_kib": 256, "pss_kib": 400, "swap_pss_kib": 200})
        with patch("scripts.observe.platform.system", return_value="Linux"), patch.object(Path, "read_text", side_effect=FileNotFoundError):
            self.assertIsNone(memory_sample(123))


if __name__ == "__main__":
    unittest.main()
