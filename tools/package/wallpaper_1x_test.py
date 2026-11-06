"""wallpaper_1x.py's plist edit, and the pinned plists it made (bazel/pinned/overlay/wallpaper-1x/)."""

import os
import plistlib
import unittest
from pathlib import Path

import wallpaper_1x

FLAT = {
    'appearanceAware': True,
    'assets': {
        'lockAndHome': {
            'default': {
                'backgroundAnimationFileName': '7570.iPadOS_17_Flat_Background-1024w-1366h@2x~ipad.ca',
                'identifier': 7570,
                'name': 'iPadOS 17',
                'type': 'LayeredAnimation',
            }
        }
    },
    'contentVersion': 2.0,
    'family': 'iPadOS 17',
    'identifier': 7570,
    'logicalScreenClass': '1024w-1366h@2x~ipad',
    'name': 'iPadOS 17',
    'productTypes': ['iPad7,1', 'iPad7,2'],
    'version': 1,
}


def pinned():
    """Find the pinned plists: runfiles under Bazel, the checkout otherwise."""
    for base in (os.environ.get('TEST_SRCDIR', ''), str(Path(__file__).resolve().parents[2])):
        for root in (Path(base) / '_main', Path(base)):
            found = sorted((root / 'bazel/pinned/overlay/wallpaper-1x').glob('*.plist'))
            if found:
                return found
    return []


class OneXTest(unittest.TestCase):
    def test_only_the_screen_class_changes(self):
        out = plistlib.loads(wallpaper_1x.one_x(plistlib.dumps(FLAT, fmt=plistlib.FMT_BINARY), 'flat'))
        self.assertEqual(out.pop('logicalScreenClass'), 'any')
        expected = dict(FLAT)
        expected.pop('logicalScreenClass')
        self.assertEqual(out, expected)

    def test_refuses_what_is_not_a_j120_2x_bundle(self):
        animated = dict(FLAT, unsupportedProductTypes=['iPad7,1', 'iPad7,2'])
        animated.pop('productTypes')
        for bad in (animated, dict(FLAT, logicalScreenClass='834w-1112h@2x~ipad')):
            with self.assertRaises(SystemExit):
                wallpaper_1x.one_x(plistlib.dumps(bad, fmt=plistlib.FMT_BINARY), 'bad')

    def test_pinned_plists_are_any_scale_j120_bundles(self):
        found = pinned()
        self.assertEqual([p.name for p in found], ['7560.Lake_Flat.plist', '7570.iPadOS_17_Flat.plist'])
        for path in found:
            plist = plistlib.loads(path.read_bytes())
            self.assertEqual(plist['logicalScreenClass'], 'any', path.name)
            self.assertIn('iPad7,1', plist['productTypes'])
            self.assertTrue(
                plist['assets']['lockAndHome']['default']['backgroundAnimationFileName'].endswith(
                    '-1024w-1366h@2x~ipad.ca'
                ),
                path.name,
            )


if __name__ == '__main__':
    unittest.main()
