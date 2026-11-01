#!/usr/bin/env python3
"""Frozen four-core native build; reuse the reviewed 33-component closure."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'out/native-a57/root-v1'
TREE = WORK / 'tree'
BASE = WORK / 'passes/native-root-vmregions-v1'
DEST = WORK / 'passes/native-root-smp-v1'
CHANGED = [
    'drivers/ArmCpu/ArmCpu.cpp',
    'drivers/NativeIOSwitchBlock/NativeIOSwitchBlock.cpp',
    'drivers/NativeIOSwitchBlock/NativeIOSwitchBlock.h',
    '.cache/xnu-work/osfmk/arm64/start.s',
    '.cache/xnu-work/osfmk/arm64/ioswitch_smp.h',
    '.cache/xnu-work/osfmk/arm64/ioswitch_smp_tables.inc',
    '.cache/xnu-work/osfmk/arm64/cpu.c',
    '.cache/xnu-work/osfmk/arm/arm_init.c',
    '.cache/xnu-work/osfmk/arm/pmap/pmap.c',
    '.cache/xnu-work/osfmk/kern/machine.c',
    '.cache/xnu-work/iokit/bsddev/IOKitBSDInit.cpp',
    'patches/xnu/native-switch-smp-entry.patch',
    'patches/xnu/native-switch-smp-kernel.patch',
    'diagnostics/ioswitch/IOSwitchSD.c',
    'diagnostics/ioswitch/IOSwitchSD.h',
    'diagnostics/ioswitch/IOSwitchDiag.c',
]
TARGETS = ['osfmk/DEVELOPMENT/' + n + '.o' for n in
           ('start', 'cpu', 'arm_init', 'pmap', 'machine')]
TARGETS += ['iokit/DEVELOPMENT/IOKitBSDInit.cpo']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    if sys.argv[1:] not in (['--source-frozen'], ['--resume-kernel']):
        raise SystemExit('Usage: build-native-smp-v1.py --source-frozen | --resume-kernel')
    resume = sys.argv[1:] == ['--resume-kernel']
    prior = json.loads((BASE / 'build-receipt.json').read_text())
    assert prior['full_native_ram'] and not prior['native_pio_fallback']
    assert prior['native_virtual_reservations_disjoint'] and len(prior['components']) == 33
    driver = json.loads((ROOT / 'out/research-native/native-smp-driver-v1/result.json').read_text())
    entry = json.loads((ROOT / 'out/research-native/native-smp-entry-v1/result.json').read_text())
    assert driver['source_frozen'] and driver['status'] == entry['status'] == 'pass'
    assert sha(TREE / CHANGED[0]) == driver['source_sha256']
    assert sha(TREE / CHANGED[3]) == entry['source_sha256']
    assert sha(TREE / CHANGED[4]) == driver['header_sha256'] == entry['header_sha256']
    diagnostic = json.loads((ROOT / 'out/research-native/raw-profile-v24/root-wait-diag/result.json').read_text())
    assert diagnostic['source_frozen'] and diagnostic['status'] == 'pass'
    for relative, digest in diagnostic['changed_sources'].items():
        assert sha(TREE / relative) == digest, relative
    root_hooks = json.loads((ROOT / 'out/research-native/native-smp-v1/root-kernel-patch-result.json').read_text())
    assert root_hooks['patch_replay_exact']
    for relative, digest in root_hooks['root_source_sha256'].items():
        assert sha(TREE / relative) == digest, relative
    storage_gates = json.loads((ROOT / 'out/research-native/native-smp-v1/storage-profile-gates/result.json').read_text())
    assert storage_gates['source_frozen'] and storage_gates['status'] == 'pass'
    for relative, digest in storage_gates['changed_sources'].items():
        assert sha(TREE / relative) == digest, relative
    DEST.mkdir(exist_ok=resume)
    snapshots = {}
    for relative in CHANGED:
        path = TREE / relative
        target = DEST / 'source/tree' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if resume:
            assert target.is_file() and sha(path) == sha(target), relative
        else:
            shutil.copy2(path, target)
        snapshots[relative] = sha(target)
    emitter = DEST / 'source/prelink'
    if not resume:
        shutil.copytree(BASE / 'source/prelink', emitter)
    shutil.copy2(__file__, DEST / 'source/build-native-smp-v1.py')
    options = prior['options']
    (DEST / 'build-options.json').write_text(json.dumps(options, indent=2) + '\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith('XNU_')}
    env.update(options)
    env['PATH'] = '/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:' + env['PATH']
    xnu = TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    cxx = json.loads((xnu / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))

    def run(command, name, cwd=TREE):
        print('Building ' + name, flush=True)
        with (DEST / (name + '.log')).open('wb') as log:
            result = subprocess.run([str(v) for v in command], cwd=cwd, env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            print((DEST / (name + '.log')).read_text(errors='replace')[-6500:])
            raise SystemExit(result.returncode)

    objects = []
    if not resume:
        commands = []
        archive_commands = json.loads((WORK / 'passes/native-root-persistent-data-v1/archive-commands.json').read_text())
        c_record = json.loads((xnu / 'osfmk/DEVELOPMENT/arm_init.o.json').read_text().strip().rstrip(','))
        diagnostic_objects = []
        for name in ('IOSwitchSD.c', 'IOSwitchDiag.c'):
            command = next(c for c in archive_commands if str(TREE / 'diagnostics/ioswitch' / name) in c)
            run(command, Path(name).stem, cwd=c_record['directory'])
            commands.append(command)
            obj = Path(command[command.index('-o') + 1])
            diagnostic_objects.append(obj)
            objects.append(obj)
        diag_archive = TREE / 'build/ioswitch-diag/libIOSwitchDiag.a'
        (DEST / 'before/ioswitch-diag').mkdir(parents=True, exist_ok=True)
        shutil.copy2(diag_archive, DEST / 'before/ioswitch-diag/libIOSwitchDiag.a')
        run(['xcrun', 'ar', 'rcs', diag_archive, *diagnostic_objects], 'diagnostics-archive')
        for relative, archive in (
            ('armcpu', 'libArmCpu.a'),
            ('native-ioswitch-root', 'libNativeIOSwitchBlockDevice.a'),
        ):
            directory = TREE / 'build' / relative
            command = json.loads((directory / 'compile-command.json').read_text())
            run(command, relative, cwd=cxx['directory'])
            commands.append(command)
            obj = Path(command[command.index('-o') + 1])
            objects.append(obj)
            backup = DEST / 'before' / relative / archive
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(directory / archive, backup)
            run(['xcrun', 'ar', 'rcs', directory / archive, obj], relative + '-archive')
        (DEST / 'changed-compile-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    else:
        for command in json.loads((DEST / 'changed-compile-commands.json').read_text()):
            objects.append(Path(command[command.index('-o') + 1]))
    for relative in TARGETS:
        (xnu / relative).unlink(missing_ok=True)
    (xnu / 'mach.development.bcm2837.unstripped.noctf').unlink(missing_ok=True)
    recipe = TREE / 'scripts/build-xnu-persistent-data-v17-fast-active.sh'
    shutil.copy2(recipe, DEST / recipe.name)
    run(['bash', recipe], 'kernel')
    shutil.copy2(xnu / 'mach.development.bcm2837.unstripped.noctf', DEST / 'source-kernel.macho')
    command = json.loads((BASE / 'prelink-command.json').read_text())
    command[1] = str(emitter / 'emit_prelink.py')
    command[command.index('--kernel') + 1] = str(DEST / 'source-kernel.macho')
    command[command.index('--output') + 1] = str(DEST / 'kernel.macho')
    (DEST / 'prelink-command.json').write_text(json.dumps(command, indent=2) + '\n')
    run(command, 'prelink')
    metadata = json.loads((DEST / 'kernel.json').read_text())
    assert metadata['components'] == prior['components']
    for relative, digest in snapshots.items():
        assert sha(TREE / relative) == digest, 'Source changed: ' + relative
    for path in objects + [xnu / relative for relative in TARGETS]:
        target = DEST / 'objects' / path.relative_to(TREE / 'build')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    receipt = dict(prior)
    receipt.update(
        build_kind='incremental-native-four-a57-smp', based_on_pass=str(BASE),
        kernel_sha256=sha(DEST / 'kernel.macho'), kernel_bytes=(DEST / 'kernel.macho').stat().st_size,
        source_kernel_sha256=sha(DEST / 'source-kernel.macho'), changed_source_sha256=snapshots,
        recompiled_units=['IOSwitchSD.c', 'IOSwitchDiag.c', 'ArmCpu.cpp', 'NativeIOSwitchBlock.cpp', *TARGETS],
        normal_header_dependency_rebuilds_enabled=True,
        native_smp=dict(enabled=True, max_cpus=4, physical_mpidrs=[0, 1, 2, 3],
                        conduit='smc', hardware_qualified=False),
        raw_root_wait_diagnostic=True,
        audit_fixes=prior['audit_fixes'] + ['four-A57 secondary entry, GICv2 and bounded online qualification',
                                         'exact CPU data and kernel table cache publication',
                                         'bounded raw-only root wait registry diagnostic'],
        proof_boundary='Native changed-unit compile/link/prelink and focused source fixtures only; no physical CPU_ON, SMP boot, raw-root repair or device access.',
        original_kernelcache_and_executable_hashes_recomputed=False,
        models_or_test_suites_run_by_builder=False, physical_device_access=False)
    (DEST / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: receipt[k] for k in ('kernel_bytes', 'kernel_sha256', 'native_smp')}, indent=2))


if __name__ == '__main__':
    main()
