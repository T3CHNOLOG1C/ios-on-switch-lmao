"""Root-package refusal gates; no file in these tests reaches a device."""
from pathlib import Path
import hashlib
import tempfile
import unittest
from unittest.mock import patch
from scripts import package_native_xnu as package

class RootPackageTests(unittest.TestCase):
    def test_root_revision_requires_policies_before_reading_inputs(self):
        with self.assertRaisesRegex(ValueError,'exact signed policy'):
            package.build(Path('absent'),Path('absent'),Path('absent'),revision=10,exporter=Path('absent'))
    def test_non_root_revision_refuses_policy_mode(self):
        with self.assertRaisesRegex(ValueError,'root boot revision'):
            package.build(Path('absent'),Path('absent'),Path('absent'),revision=9,exporter=Path('absent'),root_policies=Path('absent'))
    def test_policy_names_and_identities(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); a=b'signed os fixture'; b=b'signed system fixture'
            (p/'OS.BIN').write_bytes(a); (p/'SYSTEMOS.BIN').write_bytes(b)
            expected={'OS.BIN':(len(a),hashlib.sha256(a).hexdigest()),'SYSTEMOS.BIN':(len(b),hashlib.sha256(b).hexdigest())}
            with patch.object(package,'ROOT_POLICIES',expected):
                self.assertEqual(package.root_policy_files(p),{'IOSWITCH/POLICY10/OS.BIN':a,'IOSWITCH/POLICY10/SYSTEMOS.BIN':b})
                (p/'OS.BIN').write_bytes(a[:-1]+b'x')
                with self.assertRaisesRegex(ValueError,'policy identity mismatch'):
                    package.root_policy_files(p)
    def test_exact_current_policy_inputs(self):
        p=package.ROOT/'out/native-a57/root-v1/policy-input/IOSWITCH/POLICY10'
        if not p.exists(): self.skipTest('local signed policy inputs not prepared')
        files=package.root_policy_files(p)
        self.assertEqual(sum(map(len,files.values())),73287008)

if __name__=='__main__':unittest.main()
