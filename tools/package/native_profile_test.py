"""The full-RAM boot base follows from each kind's reviewed virtual base by the rule the kernel window maps with."""

import unittest

from build_native_loader import FULL_RAM_BANK, NATIVE_PROFILES, PHYSICAL_BASE, boot_virt_base

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


if __name__ == '__main__':
    unittest.main()
