"""The boot logo, one picture from hekate to backboardd from the Apple logo's SVG art, and hekate's menu icon.

    logo.py --svg ART --progressui PNG --bootlogo BMP --icon BMP

ProgressUI (backboardd's boot overlay) is the reference. Main-screen-scale 1.6 truncates to 1, so it loads
apple-logo~ipad.png, puts it at its own pixel size in the middle of a 1280x720 landscape layer and turns that layer
into the 720x1280 panel by DisplayBootRotation (see logo_test.py). hekate draws bootlogo.bmp centred on the same
panel. The loader and XNU (graphics mode) leave the framebuffer alone, so these two files are the whole picture.

The logo is opaque grey on black, so Core Animation's blending and colour matching cannot change a pixel. The menu
icon is the same art as Nyx's own _hue icons are drawn (nyx/resources): white, its coverage in alpha, the longer side
160 px centred in 192, so Nyx tints it with the theme colour inside its icon box, as it does switchroot's.
"""

import argparse
import struct
import zlib
from pathlib import Path

from raster import contours, rasterize, svg_path_data

CANVAS = 300
# ProgressUI's own 2x art in 21H461 (apple-logo@2x~ipad.png, 600x600), halved: the logo's 1000 units are 140 px and
# its origin sits at (94.1, 58.45) of the 300 px canvas (least squares against that art).
CANVAS_SCALE = 0.14
CANVAS_ORIGIN = (94.1, 58.45)
ICON = 192
ICON_GLYPH = 160


def canvas(d: str) -> list[bytes]:
    """Draw the boot logo as ProgressUI shows it, upright in landscape: rows of grey."""
    return rasterize(d, CANVAS, CANVAS, CANVAS_SCALE, CANVAS_ORIGIN)


def grey(rows: list[bytes]) -> list[bytes]:
    """Turn rows of grey into RGB rows."""
    return [bytes(v for v in row for _ in range(3)) for row in rows]


def hue_icon(d: str) -> list[bytes]:
    """Draw hekate's menu icon: the art's box scaled to ICON_GLYPH on its longer side, centred: coverage rows."""
    points = [p for contour in contours(d) for segment in contour for p in segment]
    left, right = min(x for x, _ in points), max(x for x, _ in points)
    top, bottom = min(y for _, y in points), max(y for _, y in points)
    scale = ICON_GLYPH / max(right - left, bottom - top)
    offset = (ICON / 2 - scale * (left + right) / 2, ICON / 2 - scale * (top + bottom) / 2)
    return rasterize(d, ICON, ICON, scale, offset)


def quarter_turn_left(rows: list[bytes]) -> list[bytes]:
    """Turn the picture 90 degrees counter-clockwise (landscape art into hekate's portrait framebuffer)."""
    width = len(rows[0])
    return [bytes(row[width - 1 - y] for row in rows) for y in range(width)]


def png_rgb(rows: list[bytes]) -> bytes:
    """Encode RGB rows as an opaque 8-bit PNG, untagged (sRGB to Core Graphics, as is the panel's colour space)."""

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    header = struct.pack('>IIBBBBB', len(rows[0]) // 3, len(rows), 8, 2, 0, 0, 0)
    raw = b''.join(b'\0' + row for row in rows)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')


def bmp_argb(rows: list[bytes]) -> bytes:
    """Encode RGB rows as an opaque 32-bit BMP, stored bottom-up: the only BMP hekate and Nyx read."""
    return bmp_bgra(
        [bytes(c for x in range(0, len(row), 3) for c in (row[x + 2], row[x + 1], row[x], 255)) for row in rows]
    )


def bmp_white(coverage: list[bytes]) -> bytes:
    """Encode coverage rows as white with that coverage in alpha: a 32-bit BMP, as Nyx's _hue icons are."""
    return bmp_bgra([bytes(c for a in row for c in (255, 255, 255, a)) for row in coverage])


def bmp_bgra(rows: list[bytes]) -> bytes:
    """Encode BGRA rows as a 32-bit BMP, stored bottom-up."""
    width, height = len(rows[0]) // 4, len(rows)
    pixels = b''.join(reversed(rows))
    info = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 32, 0, len(pixels), 2835, 2835, 0, 0)
    return b'BM' + struct.pack('<IHHI', 14 + len(info) + len(pixels), 0, 0, 14 + len(info)) + info + pixels


def main() -> None:
    """Write the three pictures."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('svg', 'progressui', 'bootlogo', 'icon'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    art = svg_path_data(args.svg)
    logo = canvas(art)
    args.progressui.write_bytes(png_rgb(grey(logo)))
    args.bootlogo.write_bytes(bmp_argb(grey(quarter_turn_left(logo))))
    args.icon.write_bytes(bmp_white(hue_icon(art)))


if __name__ == '__main__':
    main()
