"""tools/trustcache: the module's layout, order, deduplication and refusals."""

import hashlib
import struct
import unittest

from tools.trustcache.trustcache import trust_module


def h(n: int) -> bytes:
    """Return a CDHash of `n` repeated."""
    return bytes([n]) * 20


class TrustModuleTest(unittest.TestCase):
    def test_layout_sorted_with_default_uuid(self):
        module = trust_module([h(3), h(1), h(2)])
        version, uuid, count = struct.unpack_from('<I16sI', module)
        body = module[24:]
        self.assertEqual((version, count, len(body)), (2, 3, 3 * 24))
        self.assertEqual([body[i * 24 : i * 24 + 20] for i in range(3)], [h(1), h(2), h(3)])
        self.assertEqual(body[20:24], bytes([2, 0, 0, 0]))
        self.assertEqual(uuid, hashlib.sha256(body).digest()[:16])

    def test_repeated_cdhash_is_one_entry(self):
        self.assertEqual(trust_module([h(1), h(2), h(1)]), trust_module([h(2), h(1)]))
        self.assertEqual(trust_module([(h(1), 1, 3), (h(1), 1, 3)]), trust_module([(h(1), 1, 3)]))

    def test_flags_and_category(self):
        body = trust_module([(h(9), 1, 7)])[24:]
        self.assertEqual(body, h(9) + bytes([2, 1, 7, 0]))

    def test_conflicting_duplicates_refused(self):
        with self.assertRaises(ValueError):
            trust_module([(h(1), 0, 0), (h(1), 0, 2)])

    def test_explicit_uuid_and_bad_input(self):
        self.assertEqual(trust_module([h(1)], uuid=b'U' * 16)[4:20], b'U' * 16)
        with self.assertRaises(ValueError):
            trust_module([b'short'])
        with self.assertRaises(ValueError):
            trust_module([h(1)], uuid=b'x')


if __name__ == '__main__':
    unittest.main()
