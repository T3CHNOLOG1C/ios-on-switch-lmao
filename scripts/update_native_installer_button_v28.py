#!/usr/bin/env python3
"""Replace only the small installer payload; stat the six sparse parts only."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'out/native-apfs-installer-v28'
PLAN = ROOT / 'out/native-sparse-v27-plan/plan.json'
PREVIOUS = ROOT / 'out/switch/native-installer-sparse-v27-install.json'
RECEIPT = ROOT / 'out/switch/native-installer-button-v28-install.json'
MOUNT = Path('/Volumes/SWITCH SD')
PAYLOAD = 'bootloader/payloads/IOSWAPFS.BIN'
V27_SHA256 = 'a77983f6071db624213557021628a19d98788986dd7ca13a956f359f80dedf3f'
MANIFEST_SHA256 = 'b142ef253dd3d44522c5e8948632565784e1c8c2ee2ffed76e018214354aecf9'


def main():
    if sys.argv[1:] != ['--install']:
        raise SystemExit('Usage: update_native_installer_button_v28.py --install')
    spec = importlib.util.spec_from_file_location('staging', ROOT / 'scripts/stage_native_apfs_installer.py')
    api = importlib.util.module_from_spec(spec); spec.loader.exec_module(api)
    previous = json.loads(api.regular(PREVIOUS).read_text())
    planned = json.loads(api.regular(PLAN).read_text())
    if (previous['revision'] != 27 or previous['state'] != 'complete'
            or any(previous[key] is not True for key in ('payload_published',
                'new_payload_readback_verified', 'sparse_manifest_published',
                'source_geometry_verified', 'usb_disk_safely_ejected'))
            or previous['payload'] != PAYLOAD or previous['payload_sha256'] != V27_SHA256
            or previous['payload_bytes'] != 77248
            or previous['manifest_sha256'] != MANIFEST_SHA256
            or previous['volume_uuid'] != api.VOLUME_UUID or previous['disk_uuid'] != api.PARTITION_UUID):
        raise ValueError('Successful reviewed V27 sparse installer receipt required')
    if (planned['manifest_sha256'] != MANIFEST_SHA256
            or planned['logical_bytes'] != previous['logical_image_bytes'] or planned['logical_bytes'] != 32 << 30
            or planned['source_bytes'] != previous['retained_source_bytes'] or planned['source_bytes'] != 9620553728):
        raise ValueError('Reviewed sparse source geometry required')
    expected = {f'IOSWITCH/INSTALL/RAW24/APFS{i:04d}.BIN' for i in range(5)} | {'IOSWITCH/INSTALL/RAW27/TAIL.BIN'}
    parts = planned['parts']
    if len(parts) != 6 or {name for name, size in parts} != expected or sum(size for name, size in parts) != planned['source_bytes']:
        raise ValueError('Exact six retained sparse parts required')
    data, built = api.payload_bytes(BUILD)
    if built['payload_sha256'] == V27_SHA256:
        raise ValueError('V28 payload must differ from installed V27')
    info = api.disk_info(); device = MOUNT.stat().st_dev
    def check():
        current = api.disk_info()
        if MOUNT.stat().st_dev != device or any(current[key] != info[key]
                for key in ('ParentWholeDisk', 'DeviceIdentifier', 'TotalSize')):
            raise ValueError('USB identity or mount session changed')
    def old_payload():
        path = api.regular(MOUNT / PAYLOAD); record = path.stat()
        if record.st_dev != device or record.st_size != previous['payload_bytes']:
            raise ValueError('Installed V27 payload geometry differs')
        if hashlib.sha256(path.read_bytes()).hexdigest() != V27_SHA256:
            raise ValueError('Installed payload differs from reviewed V27')
    old_payload()
    for name, size in parts:
        record = api.regular(MOUNT / name).stat()
        if record.st_dev != device or record.st_size != size:
            raise ValueError('Retained sparse part geometry differs')
    pending = MOUNT / (PAYLOAD + '.v28-pending')
    if pending.exists() or pending.is_symlink() or RECEIPT.exists() or RECEIPT.is_symlink():
        raise ValueError('Update output already exists')
    state = dict(revision=28, scope='fresh VOL UP confirmation', payload=PAYLOAD,
        payload_published=False, new_payload_readback_verified=False, usb_disk_safely_ejected=False,
        previous_sha256=V27_SHA256, new_sha256=built['payload_sha256'], new_size_bytes=len(data),
        retained_image_part_count=6, retained_image_parts_stat_only=True,
        image_parts_recopied_or_rehashed=False, image_parts_read_back_or_backed_up=False,
        partition_changes=False, Android_partition_written_by_host=False)
    def save():
        RECEIPT.write_text(json.dumps(state, indent=2) + '\n')
    save()
    try:
        api.write_new(pending, data, check)
        verified = api.identity(api.regular(pending).stat())
        check(); old_payload()
        os.replace(pending, MOUNT / PAYLOAD)
        state['payload_published'] = True; save()
        published = api.identity(api.regular(MOUNT / PAYLOAD).stat())
        if any(published[key] != verified[key] for key in ('device', 'inode', 'size', 'mtime_ns', 'uid')):
            raise ValueError('Published payload identity differs from verified pending file')
        state.update(new_payload_readback_verified=True, readback_before_atomic_publish=True); save()
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
