#!/usr/bin/env python3
"""Shrink only the staged IOSwitch image and replace its small installer."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts import stage_native_apfs_installer as api
from scripts import prepare_native_sparse_install as sparse

MOUNT = api.MOUNT
PLAN = ROOT / 'out/native-sparse-v27-plan/plan.json'
BUILD = ROOT / 'out/native-apfs-installer-v27-final'
RECEIPT = ROOT / 'out/switch/native-installer-sparse-v27-install.json'
NEW_MANIFEST = 'IOSWITCH/INSTALL/RAW27.IWI'
OLD_MANIFEST = 'IOSWITCH/INSTALL/RAW24.IWI'
RETIRED = OLD_MANIFEST + '.old-v27'
PAYLOAD = api.PAYLOAD
MAP_SHA = 'd4be30b40c42de224ed904304fb81539d5bd758e5962e6b6914eb71618a9c74d'
OLD_SHA = '6b56e8620c9ed84d24a0a90a7f0a3aec0c70c6f07f75cf31a04f6110fc1c59ad'


def need(ok, message):
    if not ok:
        raise ValueError(message)


def main():
    need(sys.argv[1:] == ['--install'], 'Usage: install_native_sparse_v27.py --install')
    planned = json.loads(api.regular(PLAN).read_text())
    proof_bytes = api.regular(ROOT / planned['copy_map']).read_bytes()
    need(hashlib.sha256(proof_bytes).hexdigest() == MAP_SHA == planned['copy_map_sha256'],
         'Reviewed exact APFS allocation map required')
    proof = json.loads(proof_bytes)
    candidate = json.loads(api.regular(ROOT / 'out/native-single-apfs-trial-v1/candidate.json').read_text())
    stage = json.loads(api.regular(ROOT / 'out/switch/native-apfs-installer-v24-stage.json').read_text())
    previous = json.loads(api.regular(ROOT / 'out/switch/native-installer-ui-v25-install.json').read_text())
    image = api.regular(candidate['image'])
    need(proof['status'] == 'pass' and proof['complete'] is True
         and proof['content_scan'] is False and proof['image_writes'] == 0
         and set(proof['required_union']) == sparse.REQUIRED_UNION
         and proof['candidate_stat'] == candidate['candidate_stat'] == planned['candidate_stat']
         and api.identity(image.stat()) == proof['candidate_stat']
         and candidate['state'] == 'full_data_and_cryptex_staged_detached'
         and candidate['final_candidate_detached'] is True, 'Candidate or allocation proof changed')
    need(stage['state'] == 'complete' and stage['all_new_files_verified'] is True
         and stage['entry_published'] is True and stage['android_writes_by_host'] is False
         and stage['image'] == str(image) and stage['image_bytes'] == 32 << 30
         and stage['image_sha256'] == planned['staged_image_sha256']
         and stage['volume_uuid'] == api.VOLUME_UUID and stage['disk_uuid'] == api.PARTITION_UUID,
         'Completed staging on the identified USB card required')
    names = [f'IOSWITCH/INSTALL/RAW24/APFS{i:04d}.BIN' for i in range(16)]
    rows = {row['path']: row for row in stage['files']}
    need(all(rows[name]['size_bytes'] == 2 << 30 for name in names), 'Original part sizes differ')
    canonical = sparse.plan([(name, rows[name]['size_bytes']) for name in names],
                            proof['logical_bytes'], proof['block_size'], proof['copy_extents'])
    need(json.loads(json.dumps(canonical)) == {key: planned[key] for key in canonical},
         'Sparse plan differs from the reviewed allocation map')
    manifest = sparse.encode(canonical)
    need(manifest == api.regular(PLAN.parent / 'RAW27.IWI').read_bytes()
         and hashlib.sha256(manifest).hexdigest() == planned['manifest_sha256']
         and canonical['copied_bytes'] == proof['copy_bytes'], 'Sparse encoding changed')
    need(previous['revision'] == 25 and previous['payload_published'] is True
         and previous['new_payload_readback_verified'] is True
         and previous['usb_disk_safely_ejected'] is True
         and previous['new_sha256'] == OLD_SHA and previous['new_size_bytes'] == 75792,
         'Successful previous installer receipt required')
    data, built = api.payload_bytes(BUILD)
    api.detached(image)
    info = api.disk_info()
    device = MOUNT.stat().st_dev

    def check():
        current = api.disk_info()
        need(MOUNT.stat().st_dev == device and all(current[key] == info[key]
             for key in ('ParentWholeDisk', 'DeviceIdentifier', 'TotalSize')),
             'USB identity or mount session changed')
        need(api.identity(image.stat()) == proof['candidate_stat'], 'Candidate changed')

    snapshots = {}
    for name in [*names, OLD_MANIFEST, PAYLOAD, api.MENU]:
        record = api.regular(MOUNT / name).stat()
        need(record.st_dev == device, 'Staged file is on another device')
        snapshots[name] = api.identity(record)
    need(all(snapshots[name]['size'] == 2 << 30 for name in names), 'Staged source geometry changed')
    need(snapshots[PAYLOAD]['size'] == 75792
         and hashlib.sha256((MOUNT / PAYLOAD).read_bytes()).hexdigest() == OLD_SHA,
         'Installed payload is not V25')
    need((MOUNT / api.MENU).read_bytes() == api.MENU_BYTES, 'Installer menu changed')
    old_manifest_row = rows[OLD_MANIFEST]
    need(snapshots[OLD_MANIFEST]['size'] == old_manifest_row['size_bytes']
         and hashlib.sha256((MOUNT / OLD_MANIFEST).read_bytes()).hexdigest() == old_manifest_row['sha256'],
         'Previous small manifest differs')
    pending_payload = MOUNT / (PAYLOAD + '.v27-pending')
    pending_manifest = MOUNT / (NEW_MANIFEST + '.v27-pending')
    directory = MOUNT / 'IOSWITCH/INSTALL/RAW27'
    for path in (RECEIPT, MOUNT / NEW_MANIFEST, MOUNT / RETIRED,
                 pending_payload, pending_manifest, directory):
        need(not path.exists() and not path.is_symlink(), 'New update output already exists: ' + str(path))
    small_files = []
    with image.open('rb') as source:
        need(api.identity(os.fstat(source.fileno())) == proof['candidate_stat'], 'Open candidate differs')
        for row in canonical['new_files']:
            need(0 < row['bytes'] <= 1 << 20, 'New copy exceeds the small-file bound')
            source.seek(row['image_offset']); content = source.read(row['bytes'])
            need(len(content) == row['bytes'], 'Short new tail read')
            small_files.append((row['path'], content))
        need(api.identity(os.fstat(source.fileno())) == proof['candidate_stat'], 'Candidate changed while reading tail')

    def unchanged(name):
        need(api.identity(api.regular(MOUNT / name).stat()) == snapshots[name],
             'Staged file changed: ' + name)

    state = dict(revision=27, state='preparing', payload_published=False,
        new_payload_readback_verified=False, sparse_manifest_published=False,
        source_geometry_verified=False, usb_disk_safely_ejected=False,
        logical_image_bytes=canonical['logical_bytes'], retained_source_bytes=canonical['source_bytes'],
        image_bytes_to_write=canonical['copied_bytes'], target_bytes_skipped=canonical['skipped_target_bytes'],
        FAT_bytes_released=canonical['released_FAT_bytes'], new_image_bytes_copied=sum(len(b) for _, b in small_files),
        manifest=NEW_MANIFEST, manifest_sha256=planned['manifest_sha256'],
        payload=PAYLOAD, payload_sha256=built['payload_sha256'], payload_bytes=len(data),
        volume_uuid=api.VOLUME_UUID, disk_uuid=api.PARTITION_UUID, copy_map_sha256=MAP_SHA,
        image_files_rehashed=False, image_files_read_back=False, full_image_recopied=False,
        Android_partition_written_by_host=False, partition_changes=False, actions_completed=[])

    def save():
        RECEIPT.write_text(json.dumps(state, indent=2) + '\n')

    save()
    try:
        check(); directory.mkdir()
        for name, content in small_files:
            api.write_new(MOUNT / name, content, check)
            snapshots[name] = api.identity(api.regular(MOUNT / name).stat())
        api.write_new(pending_manifest, manifest, check)
        api.write_new(pending_payload, data, check)
        check(); unchanged(OLD_MANIFEST)
        os.rename(MOUNT / OLD_MANIFEST, MOUNT / RETIRED)
        retired_stat = api.identity(api.regular(MOUNT / RETIRED).stat())
        need(all(retired_stat[key] == snapshots[OLD_MANIFEST][key]
                 for key in ('device', 'inode', 'size', 'mtime_ns', 'uid')), 'Retired manifest identity differs')
        state['old_dense_manifest_retired'] = True; save()
        check(); unchanged(PAYLOAD)
        os.replace(pending_payload, MOUNT / PAYLOAD)
        state['payload_published'] = True; save()
        api.verify_new(MOUNT / PAYLOAD, len(data), built['payload_sha256'], check)
        state['new_payload_readback_verified'] = True; save()
        for action in canonical['actions']:
            name = action['path']; check(); unchanged(name)
            if action['action'] == 'truncate':
                with (MOUNT / name).open('r+b') as target:
                    need(api.identity(os.fstat(target.fileno())) == snapshots[name], 'Opened part changed')
                    target.truncate(action['retained_bytes']); target.flush(); os.fsync(target.fileno())
                    current = api.identity(os.fstat(target.fileno()))
                    need(current['device'] == device and current['inode'] == snapshots[name]['inode']
                         and current['size'] == action['retained_bytes'], 'Truncated source identity differs')
                    snapshots[name] = current
            elif action['action'] == 'delete':
                (MOUNT / name).unlink()
            elif action['action'] != 'keep':
                raise ValueError('Unexpected source action')
            state['actions_completed'].append(dict(path=name, action=action['action'], retained_bytes=action['retained_bytes']))
            save()
        check()
        for name, length in canonical['parts']:
            record = api.regular(MOUNT / name).stat()
            need(record.st_dev == device and record.st_size == length, 'Retained source geometry differs')
            unchanged(name)
        state['source_geometry_verified'] = True; save()
        check()
        os.replace(pending_manifest, MOUNT / NEW_MANIFEST)
        api.verify_new(MOUNT / NEW_MANIFEST, len(manifest), planned['manifest_sha256'], check)
        state['sparse_manifest_published'] = True; save()
        check()
        need(api.identity(api.regular(MOUNT / RETIRED).stat()) == retired_stat, 'Retired manifest changed')
        (MOUNT / RETIRED).unlink()
        subprocess.run(['/bin/sync'], check=True)
        check()
        result = subprocess.run(['/usr/sbin/diskutil', 'eject', '/dev/' + info['ParentWholeDisk']],
                                check=True, capture_output=True, text=True)
        need(not os.path.ismount(MOUNT), 'SD remains mounted after eject')
        state.update(state='complete', usb_disk_safely_ejected=True, eject_stdout=result.stdout.strip()); save()
    except BaseException as error:
        state.update(state='failed', error=str(error), do_not_boot_installer_until_resolved=True); save(); raise
    print(json.dumps(state, indent=2))


if __name__ == '__main__':
    main()
