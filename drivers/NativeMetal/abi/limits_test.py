#!/usr/bin/env python3
"""LimitsTable.h's fields are at the offsets every kind's Metal.framework reads them from (kinds/<kind>.json)."""

import json
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


class LimitsTable(unittest.TestCase):
    def test_every_kind_reads_the_table_at_its_offsets(self):
        header = (HERE.parent / 'LimitsTable.h').read_text()
        table = {name: int(offset, 16) for name, offset in re.findall(r'\{"(\w+)", (0x[0-9a-f]+)\}', header)}
        kinds = sorted((HERE / 'kinds').glob('*.json'))
        self.assertGreaterEqual(len(kinds), 2)
        for path in kinds:
            limits = json.loads(path.read_text())['MTLDevice']['limits']
            for name, offset in limits.items():
                self.assertEqual(table.get(name), offset, f'{path.stem}: {name}')


if __name__ == '__main__':
    unittest.main()
