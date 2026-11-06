"""Function lists and constants of hand-made metallibs."""

import struct
import unittest

from tools.gpu.shaders.metallib import FRAGMENT, KERNEL, Constant, functions, thin


def _tag(name: bytes, value: bytes) -> bytes:
    return name + struct.pack('<H', len(value)) + value


def _library(entries: list[tuple[str, int, list[tuple[str, int, int, int]]]]) -> bytes:
    """Make a thin MTLB with these functions (name, type, constants), CNSTs in the public metadata."""
    public, offsets = b'', []
    for _, _, constants in entries:
        offsets.append(len(public))
        body = b''
        if constants:
            cnst = struct.pack('<H', len(constants))
            for name, kind, index, required in constants:
                cnst += name.encode() + b'\0' + bytes([kind]) + struct.pack('<H', index) + bytes([required])
            body = _tag(b'CNST', cnst)
        body += b'ENDT'
        public += struct.pack('<I', len(body)) + body
    listing = struct.pack('<I', len(entries))
    for (name, kind, _), offset in zip(entries, offsets, strict=True):
        body = _tag(b'NAME', name.encode() + b'\0') + _tag(b'TYPE', bytes([kind]))
        body += _tag(b'OFFT', struct.pack('<QQQ', offset, 0, 0)) + b'ENDT'
        listing += struct.pack('<I', 4 + len(body)) + body
    header = bytearray(0x58)
    header[:4] = b'MTLB'
    struct.pack_into('<4Q', header, 0x18, 0x58, len(listing), 0x58 + len(listing), len(public))
    struct.pack_into('<Q', header, 0x10, 0x58 + len(listing) + len(public))
    return bytes(header) + listing + public


class MetallibTest(unittest.TestCase):
    def test_functions_and_constants(self):
        data = _library([('blur', KERNEL, [('radius', 33, 0, 1), ('fast', 53, 1, 0)]), ('shade', FRAGMENT, [])])
        found = functions(data)
        self.assertEqual([f.name for f in found], ['blur', 'shade'])
        self.assertEqual(found[0].constants, (Constant('radius', 33, 0, True), Constant('fast', 53, 1, False)))
        self.assertEqual(found[0].required(), (Constant('radius', 33, 0, True),))
        self.assertEqual(found[1].constants, ())

    def test_fat_library_reads_its_mtlb_slice(self):
        library = _library([('k', KERNEL, [])])
        fat = b'\xca\xfe\xba\xbe' + struct.pack('>I', 1) + struct.pack('>5I', 0, 0, 28, len(library), 0) + library
        self.assertEqual(thin(fat), library)
        self.assertEqual([f.name for f in functions(fat)], ['k'])

    def test_not_a_library(self):
        self.assertIsNone(functions(b'\xcf\xfa\xed\xfe' + bytes(100)))


if __name__ == '__main__':
    unittest.main()
