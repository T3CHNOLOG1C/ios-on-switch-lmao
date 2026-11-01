"""Local QEMU observation. Captured frames and memory samples are evidence, not boot verdicts."""
import json
from pathlib import Path
import platform
import re
import socket
import struct
import subprocess
import time
import zlib


def memory_sample(pid):
    """KiB, with missing values left unknown. Sampling can miss short peaks."""
    if platform.system() == "Linux":
        try:
            status = Path(f"/proc/{pid}/status").read_text()
        except OSError:
            return None
        values = {}
        for key, label in (("VmRSS", "rss_kib"), ("VmHWM", "rss_high_water_kib"), ("VmSwap", "swap_kib")):
            match = re.search(rf"^{key}:\s+(\d+) kB$", status, re.M)
            if match:
                values[label] = int(match[1])
        try:
            rollup = Path(f"/proc/{pid}/smaps_rollup").read_text()
            for key, label in (("Pss", "pss_kib"), ("SwapPss", "swap_pss_kib")):
                match = re.search(rf"^{key}:\s+(\d+) kB$", rollup, re.M)
                if match:
                    values[label] = int(match[1])
        except OSError:
            pass
        return values or None
    if platform.system() == "Darwin":
        try:
            output = subprocess.run(["/bin/ps", "-o", "rss=", "-p", str(pid)],
                                    capture_output=True, text=True, timeout=2)
            if output.returncode == 0 and output.stdout.strip().isdigit():
                return {"rss_kib": int(output.stdout.strip())}
        except (OSError, subprocess.TimeoutExpired):
            pass
    return None


def ppm_to_png(source, destination):
    """Convert QEMU's unmodified RGB P6 screenshot to a lossless viewable file."""
    if source.stat().st_size > 64 * 1024 * 1024:
        raise ValueError("QEMU screenshot exceeds 64 MiB")
    data = source.read_bytes()
    offset = 0
    tokens = []
    while len(tokens) < 4:
        while offset < len(data):
            if data[offset:offset + 1].isspace():
                offset += 1
            elif data[offset:offset + 1] == b"#":
                end = data.find(b"\n", offset)
                if end == -1:
                    raise ValueError("Truncated PPM comment")
                offset = end + 1
            else:
                break
        start = offset
        while offset < len(data) and not data[offset:offset + 1].isspace():
            offset += 1
        if start == offset:
            raise ValueError("Truncated PPM header")
        tokens.append(data[start:offset])
    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise ValueError("Expected QEMU's RGB P6 screenshot")
    width, height = int(tokens[1]), int(tokens[2])
    if not (1 <= width <= 4096 and 1 <= height <= 4096):
        raise ValueError("Invalid screenshot dimensions")
    if offset == len(data) or not data[offset:offset + 1].isspace():
        raise ValueError("Missing PPM raster separator")
    # Consume exactly the separator; a first pixel may itself be whitespace.
    offset += 1
    pixels = memoryview(data)[offset:]
    if len(pixels) != width * height * 3:
        raise ValueError("PPM raster length does not match dimensions")
    rows = bytearray()
    stride = width * 3
    for row in range(height):
        rows += b"\0" + pixels[row * stride:(row + 1) * stride]

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)

    result = b"\x89PNG\r\n\x1a\n"
    result += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    result += chunk(b"IDAT", zlib.compress(rows, 3)) + chunk(b"IEND", b"")
    with destination.open("xb") as stream:
        stream.write(result)


class QMP:
    """Only a private local Unix socket; no network listener or guest credentials."""
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(2)
        self.stream = None
        self.sequence = 0
        try:
            self.socket.connect(str(path))
            self.stream = self.socket.makefile("rb")
            if "QMP" not in self.receive():
                raise ValueError("No QMP greeting")
            self.command("qmp_capabilities")
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.stream:
            self.stream.close()
        self.socket.close()

    def receive(self, deadline=None):
        if deadline is not None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("QMP command deadline exceeded")
            self.socket.settimeout(min(2, remaining))
        line = self.stream.readline(1024 * 1024 + 1)
        if not line or len(line) > 1024 * 1024:
            raise ValueError("QMP response missing or too large")
        response = json.loads(line)
        if not isinstance(response, dict):
            raise ValueError("QMP response must be an object")
        return response

    def command(self, name, arguments=None):
        deadline = time.monotonic() + 2
        self.sequence += 1
        request = {"execute": name, "id": self.sequence}
        if arguments is not None:
            request["arguments"] = arguments
        self.socket.settimeout(2)
        self.socket.sendall(json.dumps(request).encode() + b"\n")
        for _ in range(128):
            response = self.receive(deadline)
            if response.get("id") == self.sequence:
                if "error" in response:
                    raise ValueError(f"QMP {name}: {response['error']}")
                if "return" not in response:
                    raise ValueError(f"QMP {name}: missing return")
                return response["return"]
        raise ValueError("Too many unrelated QMP events")

    def screenshot(self, destination):
        raw = destination.with_suffix(".ppm")
        if raw.exists() or destination.exists():
            raise ValueError("Refusing to overwrite a screenshot")
        try:
            self.command("screendump", {"filename": str(raw)})
            ppm_to_png(raw, destination)
        finally:
            # Only our intermediate screenshot, not an input or user file.
            raw.unlink(missing_ok=True)
