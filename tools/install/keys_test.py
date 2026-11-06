"""An unchanged input skips its stage; a changed one reruns from that stage, and only the head for new boot files."""

import tempfile
import unittest
from pathlib import Path

from tools.install import keys

INPUTS = {
    'compose': {'os': 'aa', 'volumes': {'data': 'D'}, 'nominal': 1 << 40},
    'populate': {'cryptexes': ['bb', 'cc'], 'overlay': 'dd', 'gpu': 'ee', 'seed': 'ff', 'pretranslate': None},
}


class KeysTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.kernel = self.dir / 'bl33.bin'
        self.kernel.write_bytes(b'kernel 1')
        self.boot = [('bl33.bin', self.kernel)]

    def make(self, **changes: dict) -> keys.Keys:
        inputs = {stage: dict(values, **changes.get(stage, {})) for stage, values in INPUTS.items()}
        return keys.compute(inputs, self.boot, 'iPadOS 17.7.11 (21H461)')

    def done(self, k: keys.Keys) -> tuple[dict, dict]:
        return {'compose': k.compose}, {'pack': k.pack, 'head': k.head}

    def test_an_unchanged_run_does_nothing(self):
        k = self.make()
        scratch, record = self.done(k)
        self.assertEqual(self.make(), k)
        self.assertIsNone(keys.first_stage(self.make(), scratch, record))

    def test_new_boot_files_rewrite_the_head_only(self):
        scratch, record = self.done(self.make())
        self.kernel.write_bytes(b'kernel 2')
        self.assertEqual(keys.first_stage(self.make(), scratch, record), 'head')

    def test_a_changed_overlay_repopulates_the_kept_composed_container(self):
        scratch, record = self.done(self.make())
        changed = self.make(populate={'overlay': 'd2'})
        self.assertEqual(changed.compose, self.make().compose)
        self.assertEqual(keys.first_stage(changed, scratch, record), 'populate')

    def test_a_changed_ipsw_starts_again(self):
        scratch, record = self.done(self.make())
        self.assertEqual(keys.first_stage(self.make(compose={'os': 'a2'}), scratch, record), 'compose')

    def test_a_populated_container_left_by_an_interrupted_run_is_packed(self):
        k = self.make()
        self.assertEqual(keys.first_stage(k, {'compose': k.compose, 'populate': k.populate}, None), 'pack')

    def test_a_stage_version_bump_reruns_it(self):
        before = self.make()
        keys.VERSIONS['pack'] += 1
        try:
            after = self.make()
        finally:
            keys.VERSIONS['pack'] -= 1
        self.assertEqual((after.compose, after.populate), (before.compose, before.populate))
        self.assertNotEqual(after.pack, before.pack)

    def test_tree_digest_sees_contents_and_modes(self):
        tree = self.dir / 'tree'
        (tree / 'bin').mkdir(parents=True)
        (tree / 'bin/tool').write_bytes(b'x')
        first = keys.tree_digest(tree)
        (tree / 'bin/tool').chmod(0o755)
        self.assertNotEqual(keys.tree_digest(tree), first)
        self.assertIsNone(keys.tree_digest(self.dir / 'absent'))


if __name__ == '__main__':
    unittest.main()
