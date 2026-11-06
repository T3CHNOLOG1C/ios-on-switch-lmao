#!/usr/bin/env python3
"""caspec.py makes every Core Animation state of the corpus, the device's own journals, from its name alone."""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import caspec  # noqa: E402

CORPUS = os.path.join(HERE, '..', '..', '..', 'precompile', 'corpus.log')


class CorpusStates(unittest.TestCase):
    def test_every_state_from_its_name(self):
        tables = caspec.Tables(os.path.join(HERE, 'qc-21H461.json'))
        checked = 0
        with open(CORPUS) as f:
            for line in f:
                name, want = caspec.corpus_fields(line)
                if name:
                    self.assertEqual(caspec.state_fields(tables, name), want, name)
                    # the functions' part as the probe keys it, blanks and all (raw_metal_pipeline_key's spacing)
                    got = caspec.state_line(tables, name)
                    journal = re.sub(r' lib=\S+', '', line.rstrip('\n'))
                    self.assertEqual(got[got.index(' | v ') :], journal[journal.index(' | v ') :], name)
                    checked += 1
        self.assertGreater(checked, 600)


if __name__ == '__main__':
    unittest.main()
