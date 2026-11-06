"""The install spec parsed from the current kernel sources matches what the boot path needs."""
import unittest

from python.runfiles import runfiles

from tools.install import spec

TREE = "_main/"


class SpecTest(unittest.TestCase):
    def test_current_sources(self):
        r = runfiles.Create()
        s = spec.parse(r.Rlocation(TREE + "diagnostics/switch/SwitchRawMount.h"),
                       r.Rlocation(TREE + "drivers/NativeSwitchBlock/SwitchCryptex.h"),
                       r.Rlocation(TREE + "drivers/NativeSwitchBlock/SwitchCryptex.cpp"),
                       r.Rlocation(TREE + "drivers/NativeSwitchBlock/NativeSwitchBlock.cpp"))
        self.assertNotIn("partition_bytes", s)
        self.assertEqual(s["partition_name"], "iOS")
        self.assertEqual(s["partition_type"], "7C3457EF-0000-11AA-AA11-00306543ECAC")
        self.assertEqual(s["volumes"]["system"]["uuid"], "04769D26-2CA0-4825-B443-E2E5F88300F2")
        self.assertEqual(s["volumes"]["data"]["uuid"], "D1B1D053-D0A6-496C-8625-A947A3DA863E")
        self.assertEqual(s["volumes"]["preboot"]["uuid"], "4AA4A761-5138-439D-9A30-D4138F6887FA")
        self.assertEqual([c["path"] for c in s["cryptexes"]],
                         ["/private/preboot/Switch-Staging/044-76034-038.dmg",
                          "/private/preboot/Switch-Staging/044-75188-038.dmg"])


if __name__ == "__main__":
    unittest.main()
