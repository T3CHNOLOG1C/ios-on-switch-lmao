"""Each kind's full-RAM boot base, from its reviewed virtual base, and the exporter beside the kernel window."""

import unittest

from build_native_loader import FULL_RAM_BANK, NATIVE_PROFILES, PHYSICAL_BASE, _check_exporter, boot_virt_base
from loader_abi import EXPORTER_RUN

BLOCK = 0x200000


class NativeProfileTest(unittest.TestCase):
    def test_the_rule_reproduces_the_measured_21h461_bases(self):
        virt = NATIVE_PROFILES['21H461']['virt_base']
        self.assertEqual(virt, 0xFFFFFFF00427C000)
        self.assertEqual(boot_virt_base(virt, PHYSICAL_BASE, FULL_RAM_BANK[0]), 0xFFFFFFEFD2200000)

    def test_24b119_is_derived_and_its_boot_base_follows(self):
        profile = NATIVE_PROFILES['24B119']
        self.assertFalse(profile['measured'])
        self.assertEqual(boot_virt_base(profile['virt_base'], PHYSICAL_BASE, FULL_RAM_BANK[0]), 0xFFFFFFEFD1000000)

    def test_every_reviewed_base_is_congruent_with_the_window_modulo_2_mib(self):
        for kind, profile in NATIVE_PROFILES.items():
            with self.subTest(kind=kind):
                self.assertEqual(profile['virt_base'] % BLOCK, PHYSICAL_BASE % BLOCK)


class ExporterTest(unittest.TestCase):
    def test_the_largest_window_the_loader_copies_clears_the_exporter(self):
        _check_exporter(PHYSICAL_BASE, 0x6000000)

    def test_a_window_over_the_exporter_is_refused(self):
        with self.assertRaises(ValueError):
            _check_exporter(EXPORTER_RUN[0] - 0x1000, 0x2000)


if __name__ == '__main__':
    unittest.main()
