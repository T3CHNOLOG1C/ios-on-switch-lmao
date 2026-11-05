#!/usr/bin/env python3
"""Make ProgressUI's missing 1x boot logo from the 21H461 OS volume's own 2x one.

backboardd's boot overlay (BKDisplayRenderOverlayAppleLogo -> PUIProgressWindow) shows the Apple logo from
backboardd's first frame until SpringBoard's UI. At main-screen-scale 1 ProgressUI tries
apple-logo@1x~ipad.png, apple-logo@1x.png and apple-logo~ipad.png in ProgressUI.framework
(-[PUIProgressWindow _createImageWithName:]); 21H461 ships only the @2x ones, so it logs "No image found for
apple-logo" and the screen stays black. This takes apple-logo@2x~ipad.png (600x600, Apple's CgBI form: raw
deflate, BGRA, premultiplied, Adam7) from the OS volume image and averages each 2x2 block (supersampled, never
upscaled) into a standard 300x300 RGBA PNG. The overlay (tree/overlay/manifest.json) shows it in
ProgressUI.framework; the logo is rotated for landscape by ProgressUI itself (ADT /chosen display-boot-rotation,
package_raw_boot.py). The output stays under tree/build/ (not in git: it is Apple's artwork).

Usage: progressui_logo.py <OS volume .dmg> <output .png>   (the IPSW's OS member: @ipsw//:os, ipsw/pins.bzl)
"""
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

SOURCE = 'System/Library/PrivateFrameworks/ProgressUI.framework/apple-logo@2x~ipad.png'


def unfilter(raw, at, width, height):
    """PNG filters undone for one (sub)image of 4-byte pixels at raw[at:]; its rows and the offset after it."""
    stride, rows, prev = width * 4, [], bytearray(width * 4)
    for _ in range(height):
        kind, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b, c = prev[i], prev[i - 4] if i >= 4 else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return rows, at


def decode_cgbi(data):
    """Premultiplied BGRA rows of a CgBI PNG (8-bit RGBA, plain or Adam7)."""
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('not a PNG')
    at, idat, cgbi = 8, b'', False
    while at < len(data):
        length, kind = struct.unpack('>I4s', data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        at += 12 + length
        if kind == b'CgBI':
            cgbi = True
        elif kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body[:13])
        elif kind == b'IDAT':
            idat += body
    if not cgbi or depth != 8 or colour != 6:
        raise SystemExit(f'unexpected PNG: CgBI {cgbi}, depth {depth}, colour type {colour}')
    raw = zlib.decompress(idat, -15)
    if not interlace:
        return width, height, unfilter(raw, 0, width, height)[0]
    image, offset = [bytearray(width * 4) for _ in range(height)], 0
    for x0, y0, dx, dy in ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)):
        pw, ph = (width - x0 + dx - 1) // dx, (height - y0 + dy - 1) // dy
        if not pw or not ph:
            continue
        rows, offset = unfilter(raw, offset, pw, ph)
        for j, row in enumerate(rows):
            for i in range(pw):
                x = x0 + i * dx
                image[y0 + j * dy][4 * x:4 * x + 4] = row[4 * i:4 * i + 4]
    return width, height, image


def half(width, height, rows):
    """2x2 box average of premultiplied BGRA rows, as straight RGBA rows."""
    out = []
    for y in range(0, height, 2):
        line = bytearray()
        for x in range(0, width, 2):
            s = [sum(rows[y + j][4 * (x + i) + c] for j in (0, 1) for i in (0, 1)) for c in range(4)]
            b, g, r, a = ((v + 2) // 4 for v in s)
            straight = [min(255, (v * 255 + a // 2) // a) if a else 0 for v in (r, g, b)]
            line += bytes(straight + [a])
        out.append(bytes(line))
    return width // 2, height // 2, out


def write_png(path, width, height, rows):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''.join(b'\0' + row for row in rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    dmg, out = Path(sys.argv[1]), Path(sys.argv[2])
    with tempfile.TemporaryDirectory() as mount:
        subprocess.run(['hdiutil', 'attach', '-readonly', '-nobrowse', '-noverify', '-mountpoint', mount, str(dmg)],
                       check=True, stdout=subprocess.DEVNULL)
        try:
            data = (Path(mount) / SOURCE).read_bytes()
        finally:
            subprocess.run(['hdiutil', 'detach', mount], check=True, stdout=subprocess.DEVNULL)
    width, height, rows = half(*decode_cgbi(data))
    write_png(out, width, height, rows)
    print(f'{out}: {width}x{height} from {SOURCE} ({len(data)} bytes)')


if __name__ == '__main__':
    main()
