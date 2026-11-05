#!/usr/bin/env python3
"""Make the UI scale's screen class's Wallpaper.plist for the two J120 wallpapers, from the 21H461 OS volume's own.

WallpaperKit loads a wallpaper bundle only when the logicalScreenClass in its Wallpaper.plist is 'any' or the
device's (+[WKWallpaperBundle shouldLoadWallpaperBundleAtURL:]). WKLogicalScreenClassString builds the device's from
MobileGestalt's standard display zoom size, which follows ADT /product artwork-device-subtype and artwork-scale-factor
(2732 and 2: 1024x1366), and UIScreen.scale as an NSNumber: '1024w-1366h@1.6x~ipad' at the 1.6 UI scale
(kernel/xnu-10063.141.1/bsd/vm/vm_unix.c, IOSwitch UI scale), while 21H461 ships only @2x iPad bundles. Without
these the iPadOS 17 and Lake collections load empty, the iPad's default wallpaper (7570) is "not found on disk", and
WKDefaultWallpaperManager falls back to its synthesized 'Solid Black' (400).

iPad7,1 (J120) has its own bundles: the animated ones list it in unsupportedProductTypes, the _Flat ones in
productTypes (J120.HEIC, J99-Flat.HEIC: one 2x image each). This copies each _Flat bundle's Wallpaper.plist with
logicalScreenClass set to the UI scale's class and nothing else changed. The overlay (overlay/manifest.json) shows it
over the sealed bundle's own plist, with the bundle directory as its own union, so its .ca and HEIC are the sealed
volume's: no artwork is copied, and the 2x image is drawn into the 720x1280 panel (scaled down, never up).

The output is metadata only (identifiers, names, file names) and is pinned in bazel/pinned/overlay/wallpaper-1x/.

Usage: wallpaper_1x.py <OS volume .dmg> <output directory>   (the IPSW's OS member: @ipsw//:os, ipsw/pins.bzl)
"""
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path

COLLECTIONS = 'Library/Wallpaper/Collections'
# (collection, bundle, output name): the J120 bundles of the iPad's default collection and of Lake.
BUNDLES = (
    ('iPadOS_17~ipad.wallpaperCollection', '7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.wallpaper', '7570.iPadOS_17_Flat.plist'),
    ('Lake~ipad.wallpaperCollection', '7560.Lake_Flat-1024w-1366h@2x~ipad.wallpaper', '7560.Lake_Flat.plist'),
)
FROM_CLASS, TO_CLASS = '1024w-1366h@2x~ipad', '1024w-1366h@1.6x~ipad'
PRODUCT = 'iPad7,1'


def one_x(data, name):
    """The plist `data` with logicalScreenClass FROM_CLASS -> TO_CLASS; refuses anything but a J120 bundle's."""
    plist = plistlib.loads(data)
    if plist.get('logicalScreenClass') != FROM_CLASS:
        raise SystemExit(f'{name}: logicalScreenClass {plist.get("logicalScreenClass")!r}, not {FROM_CLASS!r}')
    if PRODUCT not in plist.get('productTypes', ()) or PRODUCT in plist.get('unsupportedProductTypes', ()):
        raise SystemExit(f'{name}: not a {PRODUCT} bundle')
    plist['logicalScreenClass'] = TO_CLASS
    return plistlib.dumps(plist, fmt=plistlib.FMT_BINARY)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    dmg, out = Path(sys.argv[1]), Path(sys.argv[2])
    with tempfile.TemporaryDirectory() as mount:
        subprocess.run(['hdiutil', 'attach', '-readonly', '-nobrowse', '-noverify', '-mountpoint', mount, str(dmg)],
                       check=True, stdout=subprocess.DEVNULL)
        try:
            sources = {name: (Path(mount) / COLLECTIONS / collection / 'Wallpapers' / bundle / 'Wallpaper.plist')
                       .read_bytes() for collection, bundle, name in BUNDLES}
        finally:
            subprocess.run(['hdiutil', 'detach', mount], check=True, stdout=subprocess.DEVNULL)
    out.mkdir(parents=True, exist_ok=True)
    for collection, bundle, name in BUNDLES:
        (out / name).write_bytes(one_x(sources[name], name))
        print(f'{out / name}: /{COLLECTIONS}/{collection}/Wallpapers/{bundle}/Wallpaper.plist at {TO_CLASS}')


if __name__ == '__main__':
    main()
