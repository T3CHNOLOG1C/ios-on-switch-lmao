"""The drivers run in the order the dyld cache needs, with the other roles' cdhashes, and leave the boot package's
files behind when their deltas go."""

import hashlib
import json
import stat
import sys
import tempfile
import unittest
from pathlib import Path

from tools.install import host, pretranslate

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

    def test_unknown_roles_refused(self):
        with self.assertRaises(host.HostError):
            pretranslate.Roles({'kernel': {}}, self.dir, Quiet())


if __name__ == '__main__':
    unittest.main()
