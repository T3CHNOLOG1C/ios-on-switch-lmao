#!/usr/bin/env python3
"""Replace only the small installer payload; stat retained image parts only."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'out/native-apfs-installer-v26'
MOUNT = Path('/Volumes/SWITCH SD')
PAYLOAD = 'bootloader/payloads/IOSWAPFS.BIN'
PREVIOUS = ROOT / 'out/switch/native-installer-ui-v25-install.json'
RECEIPT = ROOT / 'out/switch/native-installer-fast-v26-install.json'
V25_SHA256 = '6b56e8620c9ed84d24a0a90a7f0a3aec0c70c6f07f75cf31a04f6110fc1c59ad'


def main():
    if sys.argv[1:] != ['--install']:
        raise SystemExit('Usage: update_native_installer_fast_v26.py --install')
    spec = importlib.util.spec_from_file_location('staging', ROOT / 'scripts/stage_native_apfs_installer.py')
    api = importlib.util.module_from_spec(spec); spec.loader.exec_module(api)
    stage = json.loads(api.regular(ROOT / 'out/switch/native-apfs-installer-v24-stage.json').read_text())
    previous = json.loads(api.regular(PREVIOUS).read_text())
    if (stage['state'] != 'complete' or stage['all_new_files_verified'] is not True
            or stage['entry_published'] is not True or stage['android_writes_by_host'] is not False
            or stage['image_bytes'] != 32 << 30 or stage['parts'] != 16
            or stage['volume_uuid'] != api.VOLUME_UUID or stage['disk_uuid'] != api.PARTITION_UUID):
        raise ValueError('Completed identified sixteen-part image staging required')
    if (previous['revision'] != 25 or previous['payload'] != PAYLOAD
            or previous['payload_published'] is not True
            or previous['new_payload_readback_verified'] is not True
            or previous['usb_disk_safely_ejected'] is not True
            or previous['new_sha256'] != V25_SHA256 or previous['new_size_bytes'] != 75792):
        raise ValueError('Successful reviewed V25 payload installation receipt required')
    data, manifest = api.payload_bytes(BUILD)
    expected = previous['new_sha256']
    if manifest['payload_sha256'] == expected:
        raise ValueError('V26 payload must differ from installed V25')
    info = api.disk_info(); device = MOUNT.stat().st_dev
    def check():
        current_info = api.disk_info()
        if (MOUNT.stat().st_dev != device or any(current_info[k] != info[k]
                for k in ('ParentWholeDisk', 'DeviceIdentifier', 'TotalSize'))):
            raise ValueError('USB identity or mount session changed')
    def old_payload():
        current = api.regular(MOUNT / PAYLOAD)
        record = current.stat()
        if record.st_dev != device or record.st_size != previous['new_size_bytes']:
            raise ValueError('Installed V25 payload geometry differs')
        if hashlib.sha256(current.read_bytes()).hexdigest() != expected:
            raise ValueError('Installed payload differs from reviewed V25')
    old_payload()
    expected_parts = {f'IOSWITCH/INSTALL/RAW24/APFS{i:04d}.BIN' for i in range(16)}
    rows = [row for row in stage['files'] if row['path'] in expected_parts]
    if len(rows) != 16 or {row['path'] for row in rows} != expected_parts:
        raise ValueError('Expected exactly sixteen retained image part records')
    for row in rows:
        part = api.regular(MOUNT / row['path']); record = part.stat()
        if record.st_dev != device or record.st_size != row['size_bytes'] or record.st_size != 2 << 30:
            raise ValueError('Retained image part geometry differs')
    pending = MOUNT / (PAYLOAD + '.v26-pending')
    if pending.exists() or pending.is_symlink() or RECEIPT.exists() or RECEIPT.is_symlink():
        raise ValueError('Update output already exists')
    state = {'revision': 26, 'scope': 'installer source-check and transfer performance',
             'payload_published': False, 'new_payload_readback_verified': False,
             'usb_disk_safely_ejected': False, 'payload': PAYLOAD,
             'previous_sha256': expected, 'new_sha256': manifest['payload_sha256'],
             'new_size_bytes': len(data), 'retained_image_part_count': 16,
             'retained_image_parts_stat_only': True, 'image_parts_recopied_or_rehashed': False,
             'image_parts_read_back_or_backed_up': False, 'partition_changes': False,
             'Android_partition_written_by_host': False}
    def save():
        RECEIPT.write_text(json.dumps(state, indent=2) + '\n')
    save()
    try:
        api.write_new(pending, data, check)
        check(); old_payload()
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
