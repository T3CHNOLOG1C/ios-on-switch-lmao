"""Check the selected kind's prelink report for the host-side USB class kexts.

Every host-side USB class kext an iPad has is prelinked, and on iOS 27 each one's kernel imports stay named: an unnamed
one is a trampoline that panics when called.
"""

import json
import unittest
from pathlib import Path

from python.runfiles import runfiles

REPORTS = ('_main/kernel/kernel.json', '_main/kernel/build/xnu-12377/kernel.json')

BOTH = {
    'com.apple.iokit.IOUSBHostFamily',
    'com.apple.driver.usb.AppleUSBXHCI',
    'com.apple.driver.usb.AppleUSBHub',
    'com.apple.driver.usb.AppleUSBHostCompositeDevice',
    'com.apple.driver.usb.AppleUSBHostiOSDevice',
    'com.apple.driver.AppleUSBHostMergeProperties',
    'com.apple.driver.usb.AppleUSBHostPacketFilter',
    'com.apple.driver.usb.IOUSBHostHIDDevice',
    'com.apple.driver.usb.IOUSBHostHIDDeviceSafeBoot',
    'com.apple.iokit.IOHIDEventDriver',
    'com.apple.iokit.IOHIDEventDriverSafeBoot',
    'com.apple.driver.AppleHIDKeyboard',
    'com.apple.driver.AppleHIDKeyboardEmbedded',
    'com.apple.driver.AppleTopCaseDriverV2',
    'com.apple.driver.AppleTopCaseHIDEventDriver',
    'com.apple.driver.AppleUSBTopCaseDriver',
    'com.apple.driver.AppleMultitouchDriver',
    'com.apple.driver.AppleMultitouchSPI',
    'com.apple.driver.AppleActuatorDriver',
    'com.apple.driver.ApplePencilDriver',
    'com.apple.driver.AppleGameControllerPersonality',
    'com.apple.driver.AppleUSBAudio',
    'com.apple.iokit.IOUSBMassStorageDriver',
    'com.apple.iokit.IOSCSIArchitectureModelFamily',
    'com.apple.iokit.IOSCSIBlockCommandsDevice',
    'com.apple.driver.AppleUSBCardReader',
    'com.apple.driver.AppleUSBMassStorageInterfaceNub',
    'com.apple.driver.USBStorageDeviceSpecifics',
    'com.apple.driver.SCSIDeviceSpecifics',
}
IOS17 = BOTH | {
    'com.apple.driver.AppleTopCaseActuatorHIDDriver',
    'com.apple.filesystems.lifs',
    'com.apple.filesystems.hfs.kext',
    'com.apple.driver.usb.cdc',
    'com.apple.driver.usb.cdc.acm',
    'com.apple.driver.usb.serial',
    'com.apple.driver.usb.networking',
    'com.apple.iokit.IOSerialFamily',
}
IOS27 = BOTH | {
    'com.apple.filesystems.lifs',
    'com.apple.driver.usb.AppleUSBHostDeviceSupport',
    'com.apple.iokit.IOGameControllerFamily',
}

# iOS 27 imports still unnamed, (functions, data) per kext: a kext may only go down. Every other kext above, lifs and
# IOGameControllerFamily among them (both load at every boot), has none.
UNNAMED_27 = {
    'com.apple.driver.usb.AppleUSBCommon': (0, 2),
    'com.apple.driver.usb.AppleUSBXHCI': (0, 1),
    'com.apple.iokit.IOAccessoryManager': (2, 1),
    'com.apple.iokit.IOHIDFamily': (3, 1),
    'com.apple.iokit.IOSCSIArchitectureModelFamily': (1, 1),
    'com.apple.iokit.IOStorageFamily': (3, 1),
    'com.apple.iokit.IOUSBHostFamily': (2, 1),
    'com.apple.iokit.IOUSBMassStorageDriver': (0, 1),
}


def report() -> dict:
    """Read the one prelink report the selected kind built."""
    found = [Path(p) for p in map(runfiles.Create().Rlocation, REPORTS) if p and Path(p).exists()]
    if len(found) != 1:
        raise AssertionError(f'expected one prelink report, found {found}')
    return json.loads(found[0].read_text())


class UsbPrelinkTest(unittest.TestCase):
    """The host-side USB roots against the report."""

    @classmethod
    def setUpClass(cls) -> None:
        """Read the report once."""
        cls.report = report()
        cls.ios27 = 'unresolved_by_component' in cls.report
        cls.kexts = set(cls.report['components']) | set(cls.report['codeless_components'])

    def test_every_usb_class_kext_is_prelinked(self) -> None:
        """An accidental drop from either kind's roots fails here."""
        expected = IOS27 if self.ios27 else IOS17
        self.assertEqual(sorted(expected - self.kexts), [])

    def test_ios27_imports_stay_named(self) -> None:
        """No USB kext gains an unnamed kernel import."""
        if not self.ios27:
            self.skipTest('iOS 17 prelinks only when every import binds')
        unnamed = self.report['unresolved_by_component']
        for kext in sorted(IOS27 | set(UNNAMED_27)):
            entry = unnamed.get(kext, {'functions': [], 'data': []})
            counts = (len(entry['functions']), len(entry['data']))
            allowed = UNNAMED_27.get(kext, (0, 0))
            with self.subTest(kext=kext):
                self.assertLessEqual(counts[0], allowed[0], entry['functions'])
                self.assertLessEqual(counts[1], allowed[1], entry['data'])


if __name__ == '__main__':
    unittest.main()
