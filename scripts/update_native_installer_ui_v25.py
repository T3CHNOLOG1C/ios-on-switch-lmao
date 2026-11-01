#!/usr/bin/env python3
"""Replace only the installer payload after the owner cancels its source check."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'out/native-apfs-installer-v25'
MOUNT = Path('/Volumes/SWITCH SD')
PAYLOAD = 'bootloader/payloads/IOSWAPFS.BIN'
RECEIPT = ROOT / 'out/switch/native-installer-ui-v25-install.json'


def main():
    if sys.argv[1:] != ['--install']:
        raise SystemExit('Usage: update_native_installer_ui_v25.py --install')
    spec = importlib.util.spec_from_file_location('staging', ROOT / 'scripts/stage_native_apfs_installer.py')
    api = importlib.util.module_from_spec(spec); spec.loader.exec_module(api)
    previous = json.loads(api.regular(ROOT / 'out/switch/native-apfs-installer-v24-stage.json').read_text())
    ui = json.loads(api.regular(ROOT / 'out/research-native/installer-ui-v25/result.json').read_text())
    if previous['state'] != 'complete' or not previous['all_new_files_verified'] or ui['status'] != 'pass':
        raise ValueError('Completed image staging and formatter checks required')
    if hashlib.sha256(api.regular(ROOT / 'native/installer/main.c').read_bytes()).hexdigest() != ui['main_source_sha256']:
        raise ValueError('Installer source changed after formatter checks')
    data, manifest = api.payload_bytes(BUILD)
    info = api.disk_info(); device = MOUNT.stat().st_dev
    expected = previous['installer_sha256']
    current = api.regular(MOUNT / PAYLOAD)
    if hashlib.sha256(current.read_bytes()).hexdigest() != expected:
        raise ValueError('Installed payload differs from the reviewed V24 payload')
    for row in previous['files']:
        if row['path'].startswith('IOSWITCH/INSTALL/RAW24/'):
            part = api.regular(MOUNT / row['path'])
            if part.stat().st_dev != device or part.stat().st_size != row['size_bytes']:
                raise ValueError('Retained image part geometry differs')
    pending = MOUNT / (PAYLOAD + '.v25-pending')
    if pending.exists() or pending.is_symlink() or RECEIPT.exists():
        raise ValueError('Update output already exists')
    state = {'revision': 25, 'scope': 'installer display correction',
             'payload_published': False, 'new_payload_readback_verified': False,
             'usb_disk_safely_ejected': False, 'payload': PAYLOAD,
             'previous_sha256': expected, 'new_sha256': manifest['payload_sha256'],
             'new_size_bytes': len(data), 'image_parts_recopied_or_rehashed': False,
             'partition_changes': False, 'Android_partition_written_by_host': False}
    def save():
        RECEIPT.write_text(json.dumps(state, indent=2) + '\n')
    def check():
        current_info = api.disk_info()
        if (MOUNT.stat().st_dev != device or any(current_info[k] != info[k]
                for k in ('ParentWholeDisk', 'DeviceIdentifier', 'TotalSize'))):
            raise ValueError('USB identity or mount session changed')
    save()
    try:
        api.write_new(pending, data, check)
        check()
        if hashlib.sha256(api.regular(MOUNT / PAYLOAD).read_bytes()).hexdigest() != expected:
            raise ValueError('Old payload changed before publication')
        os.replace(pending, MOUNT / PAYLOAD)
        state['payload_published'] = True; save()
        api.verify_new(MOUNT / PAYLOAD, len(data), manifest['payload_sha256'], check)
        state['new_payload_readback_verified'] = True; save()
        subprocess.run(['/bin/sync'], check=True)
        check()
        result = subprocess.run(['/usr/sbin/diskutil', 'eject', '/dev/' + info['ParentWholeDisk']],
                                check=True, capture_output=True, text=True)
        if os.path.ismount(MOUNT):
            raise ValueError('SD remains mounted after eject')
        state.update(usb_disk_safely_ejected=True, eject_stdout=result.stdout.strip()); save()
    except BaseException as error:
        state['error'] = str(error); save(); raise
    print(json.dumps(state, indent=2))


if __name__ == '__main__':
    main()
