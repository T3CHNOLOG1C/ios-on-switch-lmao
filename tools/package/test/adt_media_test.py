"""adt_edit.py over a minimal Switch tree: the IPSW's own iPad's answers to what iOS reads before it uses a feature."""

import importlib.util
import struct
import unittest
from pathlib import Path
from types import ModuleType

from python.runfiles import runfiles


def load_adt_edit() -> ModuleType:
    path = runfiles.Create().Rlocation('_main/tools/package/adt_edit.py')
    spec = importlib.util.spec_from_file_location('adt_edit', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prop(name: str, value: bytes) -> bytes:
    return name.encode().ljust(32, b'\0') + struct.pack('<I', len(value)) + value + bytes(-len(value) % 4)


def node(props: list[bytes], children: list[bytes]) -> bytes:
    return struct.pack('<II', len(props), len(children)) + b''.join(props) + b''.join(children)


def switch_tree() -> bytes:
    arm_io = node([prop('name', b'arm-io\0'), prop('device_type', b'nvidia,tegra210b01\0')], [])
    return node([prop('name', b'device-tree\0'), prop('compatible', b'nvidia,fric\0nintendo,nx\0')], [arm_io])


def apple_tree() -> bytes:
    """Apple's device tree of the selected IPSW kind's iPad (//ipsw:device_tree)."""
    return Path(runfiles.Create().Rlocation('_main/ipsw/device_tree.adt')).read_bytes()


def properties(node: list) -> dict[str, bytes]:
    return {p[0]: p[2] for p in node[0]}


def find(tree: list, *path: str) -> list:
    for name in path:
        tree = next(c for c in tree[1] if any(p[0] == 'name' and p[2] == name.encode() + b'\0' for p in c[0]))
    return tree


# The iPads the pinned IPSW kinds are for: model -> (platform-name, chip-id, soc-generation).
SOCS = {b'iPad7,1\0': (b't8011\0', 0x8011, b'H9\0'), b'iPad12,1\0': (b't8030\0', 0x8030, b'H12\0')}


class MediaKeys(unittest.TestCase):
    def test_arm_io_has_iommu_present(self):
        adt_edit = load_adt_edit()
        tree, _ = adt_edit.adt_parse(adt_edit.ipad_adt(switch_tree()))
        props = properties(find(tree, 'arm-io'))
        self.assertEqual(props['iommu-present'], b'')
        self.assertEqual(props['device_type'], b'nvidia,tegra210b01\0')


class Identity(unittest.TestCase):
    def setUp(self):
        self.adt_edit = load_adt_edit()
        self.apple, _ = self.adt_edit.adt_parse(apple_tree())
        adt = self.adt_edit.with_ipad_identity(self.adt_edit.ipad_adt(switch_tree()), apple_tree())
        self.tree, _ = self.adt_edit.adt_parse(adt)

    def test_root_is_apples_ipad_before_the_switch(self):
        root, apple = properties(self.tree), properties(self.apple)
        for key in ('model', 'target-type', 'target-sub-type'):
            self.assertEqual(root[key], apple[key], key)
        board = apple['compatible'].split(b'\0')[0]
        self.assertEqual(root['compatible'], board + b'\0nvidia,fric\0nintendo,nx\0')

    def test_soc_is_the_ipads(self):
        platform, chip, generation = SOCS[properties(self.apple)['model']]
        self.assertEqual(properties(self.tree)['platform-name'], platform)
        self.assertEqual(properties(find(self.tree, 'chosen'))['chip-id'], struct.pack('<I', chip))
        self.assertEqual(properties(find(self.tree, 'arm-io'))['soc-generation'], generation)
        self.assertEqual(properties(find(self.apple, 'arm-io'))['soc-generation'], generation)

    def test_product_is_apples_but_the_gamut(self):
        product, apple = properties(find(self.tree, 'product')), properties(find(self.apple, 'product'))
        for key in self.adt_edit.PRODUCT_IDENTITY:
            self.assertEqual(product.get(key), apple.get(key), key)
        self.assertEqual(product['model'], properties(self.apple)['model'])
        self.assertEqual(product['artwork-display-gamut'], b'sRGB\0')


class Capabilities(unittest.TestCase):
    def test_buttons_and_maps(self):
        adt_edit = load_adt_edit()
        tree, _ = adt_edit.adt_parse(adt_edit.ipad_adt(switch_tree()))
        buttons = properties(find(tree, 'buttons'))
        self.assertIn('function-button_volup', buttons)
        self.assertNotIn('function-button_halleffect', buttons)
        self.assertEqual(properties(find(tree, 'product', 'maps'))['3d-maps'], struct.pack('<I', 0))


if __name__ == '__main__':
    unittest.main()
