#!/usr/bin/env python3
"""What the raw pack generator keeps of a probe.

A texture3d argument is an entry with no slot, a state holds up to 32 set-0 entries, and the buffer m2v binds of its own
beside an 8-bit sRGB target is named as such.
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import layouts
import mkpack
import packfile


def handle(tex: tuple | None) -> dict:
    return {'off': 228, 'kind': 'handle', 'tex': tex, 'smp': None}


class RecordsTest(unittest.TestCase):
    def test_a_texture3d_argument_is_an_entry_with_no_slot(self):
        problems: list[str] = []
        ent = mkpack.handle_entry((228, 'handle', ('tex3d', 'R102'), None, 0), {}, [], problems)
        self.assertEqual(problems, [])
        self.assertEqual(ent['tex'], ('tex3d', 0xFF))
        off, kind, stage, slot = struct.unpack_from('<HBBB', packfile.entry_bytes([ent]))
        self.assertEqual((off, kind, stage, slot), (228, 4, 2, 0xFF))

    def test_a_pipeline_holds_32_entries(self):
        ents = packfile.entry_bytes([handle(('slot', 3))] * 27)
        self.assertEqual(len(ents), 8 * 32)
        with self.assertRaises(ValueError):
            packfile.entry_bytes([handle(('slot', 3))] * 33)

    def test_srgb_targets(self):
        key = 'samples=1 topo=0 ds=0/0 c0=81:15:0:1:0:0:1:0:0 c1=115:15:0:1:0:0:1:0:0  | v f'
        self.assertTrue(layouts.srgb_target(key))
        self.assertTrue(layouts.pointer_problem(key, 176, 0).startswith("a buffer of m2v's own at 176"))
        self.assertFalse(layouts.srgb_target('samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v f'))


if __name__ == '__main__':
    unittest.main()
