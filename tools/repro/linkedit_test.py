"""linkedit.py on tiny kernels: renamed symbols and a new UUID pass, a changed code byte or a lost symbol does not."""

import dataclasses
import struct
import unittest
from collections.abc import Collection

from tools.repro import linkedit

UUID_OLD = bytes(range(16))
UUID_NEW = bytes(range(16, 32))


def segment(name: bytes, off: int, size: int) -> bytes:
    return struct.pack('<II16s4Q4I', 0x19, 72, name, 0, size, off, size, 7, 7, 0, 0)


@dataclasses.dataclass
class Kernel:
    """A Mach-O with __TEXT (header and code), __DATA (info, e.g. a UUID string) and __LINKEDIT (symbols, strings)."""

    names: list[str]
    uuid: bytes = UUID_OLD
    code: bytes = b'\x1f\x20\x03\xd5' * 4
    values: tuple[int, ...] = (0x1000, 0x1004)
    info: bytes = b''
    absolute: Collection[str] = ()

    def __bytes__(self) -> bytes:
        text_size, data_off = 0x200, 0x200
        data = self.info.ljust(0x40, b'\0')
        strings = b'\0' + b''.join(n.encode() + b'\0' for n in self.names)
        symoff = data_off + len(data)
        stroff = symoff + 16 * len(self.names)
        linkedit = b''
        at = 1
        for name, value in zip(self.names, self.values, strict=True):
            kind, sect = (0x03, 0) if name in self.absolute else (0x0F, 1)
            linkedit += struct.pack('<IBBHQ', at, kind, sect, 0, value)
            at += len(name) + 1
        linkedit += strings
        commands = (
            segment(b'__TEXT', 0, text_size)
            + segment(b'__DATA', data_off, len(data))
            + segment(b'__LINKEDIT', symoff, len(linkedit))
            + struct.pack('<6I', 0x2, 24, symoff, len(self.names), stroff, len(strings))
            + struct.pack('<II', 0x1B, 24)
            + self.uuid
        )
        header = struct.pack('<IiiIIIII', 0xFEEDFACF, 0x0100000C, 0, 2, 5, len(commands), 0, 0)
        text = (header + commands).ljust(0x180, b'\0') + self.code
        return text.ljust(text_size, b'\0') + data + linkedit


def kernel(names: list[str], **fields: object) -> bytes:
    return bytes(Kernel(names, **fields))


class LinkeditTest(unittest.TestCase):
    def test_renamed_symbols_and_a_new_uuid_pass(self):
        old = kernel(['_opaque211', '_keep'])
        new = kernel(['_platformPipeForward', '_keep'], uuid=UUID_NEW)
        report, failures = linkedit.compare(old, new)
        self.assertEqual(failures, [])
        self.assertIn('0x1000 type 0xf: _opaque211 -> _platformPipeForward', report)

    def test_the_uuid_string_in_data_is_allowed(self):
        old_text = linkedit.uuid_spans(UUID_OLD)[1]
        new_text = linkedit.uuid_spans(UUID_NEW)[1]
        old = kernel(['_a', '_b'], info=b'<key>UUID</key>' + old_text)
        new = kernel(['_a', '_b'], uuid=UUID_NEW, info=b'<key>UUID</key>' + new_text)
        _, failures = linkedit.compare(old, new)
        self.assertEqual(failures, [])

    def test_the_prelink_kcid_is_allowed(self):
        old = kernel(['_a', '_b'], info=b'<key>_PrelinkKCID</key><data>2H2CaxTB+cesFGGN/C2x/w==</data>')
        new = kernel(['_a', '_b'], uuid=UUID_NEW, info=b'<key>_PrelinkKCID</key><data>BkMLdHbrqZ8I6Wa2SCWjjg==</data>')
        report, failures = linkedit.compare(old, new)
        self.assertEqual(failures, [])
        self.assertTrue(any('KCID' in line for line in report), report)

    def test_a_code_byte_is_refused(self):
        old = kernel(['_a', '_b'])
        new = kernel(['_a', '_b'], code=b'\x1f\x20\x03\xd5' * 3 + b'\xc0\x03\x5f\xd6')
        _, failures = linkedit.compare(old, new)
        self.assertTrue(any(f.startswith('__TEXT +0x18c') for f in failures), failures)

    def test_a_named_absolute_symbol_may_go(self):
        old = kernel(['_a', '_b'], absolute={'_b'})
        new = kernel(['_a'], values=(0x1000,))
        _, failures = linkedit.compare(old, new)
        self.assertTrue(failures, 'a lost symbol must be refused unless named')
        _, failures = linkedit.compare(old, new, removed={'_b'})
        self.assertEqual(failures, [])
        _, failures = linkedit.compare(kernel(['_a', '_b']), new, removed={'_b'})
        self.assertIn('_b is not an absolute symbol', failures)

    def test_a_moved_symbol_is_refused(self):
        old = kernel(['_a', '_b'])
        new = kernel(['_a', '_b'], values=(0x1000, 0x1008))
        _, failures = linkedit.compare(old, new)
        self.assertTrue(failures and failures[0].startswith('the symbols differ beyond names'), failures)


if __name__ == '__main__':
    unittest.main()
