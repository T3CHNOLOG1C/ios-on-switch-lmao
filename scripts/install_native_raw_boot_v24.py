#!/usr/bin/env python3
"""Install additive APFS and retained V19 fallback boot entries after staging."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MOUNT = Path('/Volumes/SWITCH SD')
RECEIPT = ROOT / 'out/switch/native-raw-v24-boot-install.json'
PACKAGES = (
    ('out/native-raw-v24-boot', 'native-root-raw-apfs-boot',
     'switchroot/ioswitch-native-raw-v24/', 'bootloader/ini/ioswitch-native-raw-v24.ini',
     {'bl31.bin', 'bl33.bin', 'boot.cmd', 'boot.scr', 'raw-args.bin'}),
    ('out/native-fat-v19-fallback', 'native-root-fat-v19-fallback',
     'switchroot/ioswitch-native-fat-v19/', 'bootloader/ini/ioswitch-native-fat-v19.ini',
     {'bl31.bin', 'bl33.bin', 'boot.cmd', 'boot.scr'}),
)


def main():
    if sys.argv[1:] != ['--install']:
        raise SystemExit('Usage: install_native_raw_boot_v24.py --install')
    spec = importlib.util.spec_from_file_location('staging', ROOT / 'scripts/stage_native_apfs_installer.py')
    api = importlib.util.module_from_spec(spec); spec.loader.exec_module(api)
    stage = json.loads(api.regular(ROOT / 'out/switch/native-apfs-installer-v24-stage.json').read_text())
    if (stage['state'] != 'complete' or stage['all_new_files_verified'] is not True
            or stage['entry_published'] is not True or stage['android_writes_by_host'] is not False
            or stage['image_bytes'] != 32 << 30 or stage['parts'] != 16
            or stage['volume_uuid'] != api.VOLUME_UUID or stage['disk_uuid'] != api.PARTITION_UUID
            or RECEIPT.exists()):
        raise ValueError('Completed APFS image staging and new receipt required')
    info = api.disk_info(); session_device = MOUNT.stat().st_dev
    def check():
        current = api.disk_info()
        if (MOUNT.stat().st_dev != session_device
                or any(current[k] != info[k] for k in ('DeviceIdentifier', 'ParentWholeDisk', 'TotalSize'))):
            raise ValueError('USB identity or mount session changed')
    sources, menus, directories, dependencies = {}, [], [], {}
    for package_name, profile, prefix, menu, names in PACKAGES:
        package = ROOT / package_name
        manifest = json.loads(api.regular(package / 'manifest.json').read_text())
        expected = {prefix + name for name in names} | {menu}
        if manifest['profile'] != profile or set(manifest['files']) != expected or manifest['publish_last'] != menu:
            raise ValueError('Unexpected additive boot package')
        if profile == 'native-root-raw-apfs-boot':
            if (manifest['revision'] != 24 or manifest['physical_store_bytes'] != 32 << 30
                    or manifest['native_storage_dma_required'] is not True
                    or manifest['native_pio_fallback'] is not False
                    or manifest['native_virtual_reservations_disjoint'] is not True
                    or manifest['selected_storage_profile'] != 'raw'):
                raise ValueError('V24 raw contract differs')
        directories.append(prefix.rstrip('/')); menus.append(menu)
        dependencies.update(manifest['inherited_dependencies'])
        for name, expected_record in manifest['files'].items():
            data = api.regular(package / name).read_bytes()
            if {'size_bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()} != expected_record:
                raise ValueError('New package file changed: ' + name)
            sources[name] = data
    for name in dependencies:
        path = api.regular(MOUNT / name)
        if path.stat().st_dev != session_device or path.stat().st_size != dependencies[name]['size_bytes']:
            raise ValueError('Retained boot dependency geometry differs: ' + name)
    expected_parts = {f'IOSWITCH/INSTALL/RAW24/APFS{i:04d}.BIN' for i in range(16)}
    staged_parts = {row['path']: row for row in stage['files'] if row['path'] in expected_parts}
    if set(staged_parts) != expected_parts:
        raise ValueError('Expected sixteen staged image parts')
    for name in expected_parts:
        path = api.regular(MOUNT / name)
        if path.stat().st_dev != session_device or path.stat().st_size != staged_parts[name]['size_bytes'] or path.stat().st_size != 2 << 30:
            raise ValueError('Staged image part geometry differs: ' + name)
    for name in directories + menus + [menu + '.pending' for menu in menus]:
        path = MOUNT / name
        if path.exists() or path.is_symlink() or any(p.is_symlink() for p in path.parents):
            raise ValueError('New output exists or ancestry is linked: ' + name)
    current_menu = api.regular(MOUNT / 'bootloader/ini/ioswitch-native-xnu.ini').read_bytes()
    state = {'revision': 24, 'all_new_files_verified': False, 'entries_published': False,
             'usb_disk_safely_ejected': False, 'files': [], 'partition_changes': False,
             'android_installation_performed': False, 'sd_contents_backed_up': False,
             'old_images_rehashed': False, 'volume_uuid': api.VOLUME_UUID,
             'partition_uuid': api.PARTITION_UUID}
    def save():
        RECEIPT.write_text(json.dumps(state, indent=2) + '\n')
    save()
    try:
        check()
        for name in directories:
            (MOUNT / name).mkdir()
        for name in sorted(set(sources) - set(menus)):
            api.write_new(MOUNT / name, sources[name], check)
            state['files'].append(name); save()
        for menu in menus:
            pending = MOUNT / (menu + '.pending')
            api.write_new(pending, sources[menu], check)
            check()
            if (MOUNT / menu).exists() or (MOUNT / menu).is_symlink():
                raise ValueError('Menu appeared before publication')
            pending.rename(MOUNT / menu)
            api.verify_new(MOUNT / menu, len(sources[menu]), hashlib.sha256(sources[menu]).hexdigest(), check)
            state['files'].append(menu); save()
        if api.regular(MOUNT / 'bootloader/ini/ioswitch-native-xnu.ini').read_bytes() != current_menu:
            raise ValueError('Original boot menu changed')
        state.update(all_new_files_verified=True, entries_published=True); save()
        subprocess.run(['/bin/sync'], check=True)
        check()
        result = subprocess.run(['/usr/sbin/diskutil', 'eject', '/dev/' + info['ParentWholeDisk']],
                                capture_output=True, text=True, check=True)
        if os.path.ismount(MOUNT):
            raise ValueError('SD remains mounted after eject')
        state.update(usb_disk_safely_ejected=True, eject_stdout=result.stdout.strip()); save()
    except BaseException as error:
        state['error'] = str(error); save(); raise
    print(json.dumps(state, indent=2))


if __name__ == '__main__':
    main()
