"""The datamigrator sentinel the deploy writes when the overlay's /Applications changes."""

import os
import plistlib
import tempfile
import unittest
from pathlib import Path

from tools.deploy import system_apps
from tools.install import card as cards


class Log:
    def __init__(self) -> None:
        self.lines = []

    def __call__(self, *parts: object) -> None:
        self.lines.append(' '.join(map(str, parts)))


def bundle(app: Path, bundle_id: str, executable: bytes | None = b'arm64e') -> Path:
    app.mkdir(parents=True)
    (app / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': bundle_id, 'CFBundleExecutable': app.stem}))
    if executable is not None:
        (app / app.stem).write_bytes(executable)
        (app / app.stem).chmod(0o755)
    return app


def package(root: Path, executable: bytes | None) -> Path:
    root = Path(root)
    tool = root / 'overlay/usr/sbin/BlueTool'
    tool.parent.mkdir(parents=True, exist_ok=True)
    tool.write_bytes(b'tool')
    if executable is not None:
        app = root / 'overlay/Applications/SwitchHWTest.app/SwitchHWTest'
        app.parent.mkdir(parents=True, exist_ok=True)
        app.write_bytes(executable)
    return root


class SystemAppsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.data = self.tmp / 'data'
        self.data.mkdir()
        self.sentinel = self.data / system_apps.SENTINEL

    def deploy(self, executable: bytes | None) -> bool:
        package_root = package(self.tmp / f'package-{executable!r}', executable)
        before = system_apps.digest(self.data)
        cards.sync_overlay(self.data, package_root, Log())
        return system_apps.request_migration(self.data, before, Log())

    def test_a_new_app_asks_for_the_migration(self):
        self.assertTrue(self.deploy(b'v1'))
        self.assertTrue(self.sentinel.is_file())

    def test_an_unchanged_app_does_not(self):
        self.deploy(b'v1')
        self.sentinel.unlink()
        self.assertFalse(self.deploy(b'v1'))
        self.assertFalse(self.sentinel.exists())

    def test_a_rebuilt_app_does(self):
        self.deploy(b'v1')
        self.sentinel.unlink()
        self.assertTrue(self.deploy(b'v2'))

    def test_a_removed_app_does(self):
        self.deploy(b'v1')
        self.sentinel.unlink()
        self.assertTrue(self.deploy(None))
        self.assertTrue(self.sentinel.is_file())

    def test_no_apps_ever_leaves_migration_alone(self):
        self.assertFalse(self.deploy(None))
        self.assertFalse(self.sentinel.exists())

    def test_a_request_clears_the_kernels_one_try_marker(self):
        marker = self.data / system_apps.MARKER
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.touch()
        self.assertTrue(self.deploy(b'v1'))
        self.assertFalse(marker.exists())

    def test_force_asks_with_unchanged_apps(self):
        self.deploy(b'v1')
        self.sentinel.unlink()
        before = system_apps.digest(self.data)
        self.assertTrue(system_apps.request_migration(self.data, before, Log(), force=True))
        self.assertTrue(self.sentinel.is_file())


class RestageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.system, self.data = self.tmp / 'system', self.tmp / 'data'
        staged = self.system / 'private/var' / system_apps.STAGED
        self.facetime = bundle(staged / 'FaceTime.app', 'com.apple.facetime')
        (self.facetime / 'en.lproj').mkdir()
        (self.facetime / 'en.lproj/InfoPlist.strings').write_bytes(b'strings')
        (self.facetime / 'Current').symlink_to('en.lproj')
        bundle(staged / 'Fitness.app', 'com.apple.Fitness')
        bundle(staged / 'Files.app', 'com.apple.DocumentsApp')
        (self.data / system_apps.STAGED).mkdir(parents=True)
        self.states({'com.apple.facetime': 1, 'com.apple.Fitness': 2, 'com.apple.DocumentsApp': 1})

    def states(self, states: dict) -> None:
        path = self.data / system_apps.APP_STATES
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(plistlib.dumps(states))

    def install(self, name: str, executable: bytes | None) -> None:
        bundle(self.data / system_apps.INSTALLED / name.upper() / name, 'id', executable)

    def test_consumed_apps_the_migration_still_wants_come_back_whole(self):
        log = Log()
        self.assertTrue(system_apps.restage(self.system, self.data, log))
        copy = self.data / system_apps.STAGED / 'FaceTime.app'
        self.assertEqual((copy / 'FaceTime').read_bytes(), b'arm64e')
        self.assertEqual(os.stat(copy / 'FaceTime').st_mode & 0o777, 0o755)
        self.assertEqual((copy / 'en.lproj/InfoPlist.strings').read_bytes(), b'strings')
        self.assertEqual(os.readlink(copy / 'Current'), 'en.lproj')
        self.assertTrue((self.data / system_apps.STAGED / 'Files.app').is_dir())
        self.assertIn('FaceTime.app', log.lines[0])

    def test_an_app_marked_uninstalled_stays_out(self):
        system_apps.restage(self.system, self.data, Log())
        self.assertFalse((self.data / system_apps.STAGED / 'Fitness.app').exists())

    def test_restaging_twice_copies_nothing_more(self):
        self.assertTrue(system_apps.restage(self.system, self.data, Log()))
        self.assertFalse(system_apps.restage(self.system, self.data, Log()))

    def test_an_installed_app_is_left_alone(self):
        self.install('FaceTime.app', b'arm64e')
        self.install('Files.app', b'arm64e')
        self.assertEqual(system_apps.missing(self.system, self.data), [])

    def test_a_placeholder_without_its_executable_is_not_installed(self):
        self.install('FaceTime.app', None)
        self.install('Files.app', b'arm64e')
        self.assertEqual(system_apps.missing(self.system, self.data), ['FaceTime.app'])

    def test_without_mobile_installations_states_nothing_is_restaged(self):
        (self.data / system_apps.APP_STATES).unlink()
        self.assertFalse(system_apps.restage(self.system, self.data, Log()))

    def test_a_copy_cut_short_is_replaced(self):
        partial = self.data / system_apps.STAGED / '.FaceTime.app.partial'
        partial.mkdir()
        (partial / 'junk').write_bytes(b'')
        system_apps.restage(self.system, self.data, Log())
        self.assertFalse(partial.exists())
        self.assertFalse((self.data / system_apps.STAGED / 'FaceTime.app/junk').exists())

    def test_restaging_asks_for_the_migration(self):
        before = system_apps.digest(self.data)
        restaged = system_apps.restage(self.system, self.data, Log())
        self.assertTrue(system_apps.request_migration(self.data, before, Log(), force=restaged))
        self.assertTrue((self.data / system_apps.SENTINEL).is_file())


if __name__ == '__main__':
    unittest.main()
