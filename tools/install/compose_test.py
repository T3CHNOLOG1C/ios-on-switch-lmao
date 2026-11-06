"""compose._attach_os: the APFS physical store of an OS image, as hdiutil attach lists it."""

import unittest
from unittest import mock

from tools.install import compose

APFS = '7C3457EF-0000-11AA-AA11-00306543ECAC'
APFS_CONTENT = '41504653-0000-11AA-AA11-00306543ECAC'
CONTAINER = 'EF57347C-0000-11AA-AA11-00306543ECAC'

# 21H461's OS dmg: a GPT disk with one APFS partition, and the container macOS synthesizes for it.
GPT_IMAGE = [
    {'dev-entry': '/dev/disk32', 'content-hint': 'GUID_partition_scheme', 'potentially-mountable': False},
    {'dev-entry': '/dev/disk32s1', 'content-hint': APFS, 'potentially-mountable': False},
    {'dev-entry': '/dev/disk33', 'content-hint': CONTAINER, 'unmapped-content-hint': CONTAINER},
    {'dev-entry': '/dev/disk33s1', 'volume-kind': 'apfs', 'potentially-mountable': True},
]
# 24B119's OS dmg: a bare APFS container, whose volume carries the APFS hint as well.
BARE_CONTAINER = [
    {'dev-entry': '/dev/disk32', 'content-hint': APFS_CONTENT, 'potentially-mountable': False},
    {
        'dev-entry': '/dev/disk33s1',
        'content-hint': APFS_CONTENT,
        'unmapped-content-hint': APFS_CONTENT,
        'potentially-mountable': True,
        'volume-kind': 'apfs',
    },
    {'dev-entry': '/dev/disk33', 'content-hint': CONTAINER, 'unmapped-content-hint': CONTAINER},
]


class AttachOS(unittest.TestCase):
    def attach(self, entities: list[dict]) -> tuple[str, str]:
        with mock.patch.object(compose.host, 'plist', return_value={'system-entities': entities}):
            return compose._attach_os('os.dmg', log=None)

    def test_a_gpt_image_gives_its_apfs_partition_and_the_disk_around_it(self) -> None:
        self.assertEqual(self.attach(GPT_IMAGE), ('/dev/disk32', '/dev/disk32s1'))

    def test_a_bare_container_image_is_its_own_store(self) -> None:
        self.assertEqual(self.attach(BARE_CONTAINER), ('/dev/disk32', '/dev/disk32'))


if __name__ == '__main__':
    unittest.main()
