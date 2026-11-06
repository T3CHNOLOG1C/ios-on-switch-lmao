"""The drivers run in the order the dyld cache needs, with the other roles' cdhashes, and leave the boot package's
files behind when their deltas go."""

import hashlib
import json
import stat
import sys
import tempfile
import unittest
from pathlib import Path

from tools.install import host, overlay_index, pretranslate

FAKE_DRIVER = """#!{python}
import argparse, hashlib, json, sys
from pathlib import Path
ap = argparse.ArgumentParser()
for name in ('--role', '--tool', '--volume', '--out'):
    ap.add_argument(name, required=True)
ap.add_argument('--apple-trustcache', action='append', default=[])
ap.add_argument('--cdhash-pairs', action='append', default=[])
a = ap.parse_args()
out = Path(a.out)
(out / 'files').mkdir(parents=True, exist_ok=True)
(out / 'files' / 'tool').write_bytes(b'arm64')
entry = dict(path='/usr/bin/tool', file='files/tool', sha256_in=hashlib.sha256(b'arm64e').hexdigest(),
             sha256_out=hashlib.sha256(b'arm64').hexdigest(), cdhash=['00'])
(out / (a.role + '.manifest.json')).write_text(json.dumps(dict(role=a.role, translator='t', files=[entry])))
(out / (a.role + '.trustcache')).write_bytes(b'trust ' + a.role.encode())
(out / (a.role + '.cdhash-pairs')).write_text(a.role + ' old new\\n')
if a.role == 'system_cryptex':
    (out / 'a64x-cache-pretranslated.bin').write_bytes(b'cache')
with open(sys.argv[0] + '.calls', 'a') as calls:
    calls.write(json.dumps(dict(role=a.role, pairs=a.cdhash_pairs, trust=a.apple_trustcache, volume=a.volume)) + '\\n')
"""


class Quiet:
    def __call__(self, *parts):
        pass

    def quiet(self, *parts):
        pass


class RolesTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.driver = self.dir / 'translate_tree'
        self.driver.write_text(FAKE_DRIVER.format(python=sys.executable))
        self.driver.chmod(self.driver.stat().st_mode | stat.S_IXUSR)
        spec = {'driver': str(self.driver), 'tool': '/bin/echo', 'trustcaches': ['/apple.im4p']}
        self.roles = pretranslate.Roles({r: spec for r in pretranslate.ORDER}, self.dir / 'scratch', Quiet())

    def calls(self):
        return [json.loads(line) for line in Path(str(self.driver) + '.calls').read_text().splitlines()]

    def test_system_cryptex_takes_the_other_roles_pairs(self):
        for role in pretranslate.ORDER:
            entries = self.roles.run(role, self.dir / role)
            self.assertEqual(entries[0]['sha256_out'], hashlib.sha256(b'arm64').hexdigest())
            self.roles.done(role)
        calls = {c['role']: c for c in self.calls()}
        self.assertEqual(calls['os']['pairs'], [])
        self.assertEqual(calls['app_cryptex']['pairs'], [])
        kept = self.dir / 'scratch/pretranslated'
        self.assertEqual(calls['system_cryptex']['pairs'],
                         [str(kept / 'os.cdhash-pairs'), str(kept / 'app_cryptex.cdhash-pairs')])  # fmt: skip
        self.assertEqual(calls['os']['trust'], ['/apple.im4p'])

    def test_kept_files_outlive_the_deltas(self):
        for role in pretranslate.ORDER:
            self.roles.run(role, self.dir / role)
            self.roles.done(role)
            self.assertFalse(self.roles.out(role).exists())
        names = [p.name for p in self.roles.artifacts()]
        for role in pretranslate.ORDER:
            self.assertIn(role + '.trustcache', names)
        self.assertIn('a64x-cache-pretranslated.bin', names)

    def test_system_cryptex_refuses_to_go_first(self):
        with self.assertRaisesRegex(host.HostError, 'after os, app_cryptex'):
            self.roles.run('system_cryptex', self.dir / 'system')

    def test_a_failing_driver_stops(self):
        self.driver.write_text('#!/bin/sh\necho broken >&2\nexit 3\n')
        with self.assertRaisesRegex(host.HostError, 'translating os failed: broken'):
            self.roles.run('os', self.dir / 'os')

    def test_system_files_go_to_the_data_overlay_with_their_modes(self):
        system = self.dir / 'system'
        (system / 'usr/libexec').mkdir(parents=True)
        (system / 'usr/libexec/tool').write_bytes(b'arm64e')
        (system / 'usr/libexec/tool').chmod(0o750)
        entries = self.roles.run('os', system)
        data = self.dir / 'data'
        data.mkdir()
        entries[0]['path'] = '/usr/libexec/tool'
        manifest = self.roles.overlay_system(data, system, entries, owners=False)
        placed = data / pretranslate.OVERLAY / 'usr/libexec/tool'
        self.assertEqual(placed.read_bytes(), b'arm64')
        self.assertEqual(placed.stat().st_mode & 0o7777, 0o750)
        self.assertEqual((system / 'usr/libexec/tool').read_bytes(), b'arm64e')
        self.assertFalse(entries[0]['source'].exists())
        self.assertEqual(manifest['files'][0]['mode'], '0750')
        self.assertEqual(json.loads((data / pretranslate.OVERLAY_MANIFEST).read_text()), manifest)
        index = (data / pretranslate.OVERLAY_INDEX).read_bytes()
        self.assertEqual(overlay_index.decode(index), ['/usr/libexec/tool'])
        self.assertIn('os.overlay.json', [p.name for p in self.roles.artifacts()])

    def test_a_translated_file_that_does_not_match_is_refused(self):
        system = self.dir / 'system'
        (system / 'usr/bin').mkdir(parents=True)
        (system / 'usr/bin/tool').write_bytes(b'arm64e')
        entries = self.roles.run('os', system)
        entries[0]['source'].write_bytes(b'corrupt')
        with self.assertRaisesRegex(host.HostError, 'does not match'):
            self.roles.overlay_system(self.dir / 'data', system, entries, owners=False)

    def test_pins_refuse_another_translation(self):
        for role in pretranslate.ORDER:
            self.roles.run(role, self.dir / role)
        digest = hashlib.sha256(b'trust os').hexdigest()
        self.roles.check_pins({'os.trustcache': digest})
        with self.assertRaisesRegex(host.HostError, 'os.trustcache'):
            self.roles.check_pins({'os.trustcache': '0' * 64})
        with self.assertRaisesRegex(host.HostError, 'missing.bin'):
            self.roles.check_pins({'missing.bin': digest})

    def test_unknown_roles_refused(self):
        with self.assertRaises(host.HostError):
            pretranslate.Roles({'kernel': {}}, self.dir, Quiet())


if __name__ == '__main__':
    unittest.main()
