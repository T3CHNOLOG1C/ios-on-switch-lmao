#!/usr/bin/env python3
"""Prepare the explicit four-A57/raw-APFS native layout; no device access.

Existing V24 is the single-core fallback. This helper has no reduced-core mode.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts import native_oled_adt as oled, native_xnu_layout as layout
from scripts.build_native_loader import NATIVE_FULL_RAM_EXCLUSIONS
from scripts.package_native_handoff import safe_output, read_regular

BASE = ROOT / 'out/native-a57/root-v1/passes/native-root-vmregions-v1'
DEFAULT_BUILD = ROOT / 'out/native-a57/root-v1/passes/native-root-smp-v1'


def command_line(previous):
    tokens = previous.split()
    prefixes = ('cpus=', 'sheng-smp=', 'ioswitch-smp=', 'ioswitch-storage=')
    if (any(token.startswith('cpumask=') for token in tokens)
            or any(sum(token.startswith(prefix) for token in tokens) > 1 for prefix in prefixes)
            or [token for token in tokens if token.startswith('cpus=')] != ['cpus=1']
            or any(token in tokens for token in ('armcpu-skip-gic=1', 'sheng-smp-gic=1', 'armcpu-spi-ipi=1'))):
        raise ValueError('reviewed single-core GICv2 baseline command required')
    result = [token for token in tokens if not token.startswith(prefixes)]
    return ' '.join(result + ['cpus=4', 'sheng-smp=1', 'ioswitch-smp=1', 'ioswitch-storage=raw'])


def validate_receipt(receipt, previous):
    smp = receipt.get('native_smp', {})
    if (smp.get('enabled') is not True or smp.get('max_cpus') != 4
            or len(receipt['components']) != 33 or receipt['components'] != previous['components']
            or receipt['full_native_ram'] is not True or receipt['native_ram_aperture_bytes'] != 4 << 30
            or receipt['native_ram_runtime_reservations'] is not True
            or receipt['native_storage_dma_required'] is not True or receipt['native_pio_fallback'] is not False
            or receipt['native_virtual_reservations_disjoint'] is not True
            or receipt['early_fault_visibility_enabled'] is not True
            or receipt['early_panic_visibility_enabled'] is not True):
        raise ValueError('new four-core SMP build with unchanged original closure/native RAM/DMA policy required')


def build_layout(inherited, kernel, adt, output, command):
    # Required seed/trust bytes are consumed for the new image, while their old
    # identities are inherited rather than separately rehashed or rescanned.
    borrowed = {}
    for key in ('TrustCache', 'NativeDataSeed'):
        row = inherited['sources'][key]; path = str(Path(row['path']).resolve())
        if (path != row['path'] or path in borrowed or row['size_bytes'] <= 0
                or not re.fullmatch('[0-9a-f]{64}', row['sha256'])):
            raise ValueError('invalid inherited sidecar identity')
        borrowed[path] = row
    fresh = {str(kernel.resolve()), str(adt.resolve())}
    if len(fresh) != 2 or fresh & borrowed.keys(): raise ValueError('fresh and inherited layout inputs alias')
    original = layout.source_record; used = set()
    def record(path, data):
        actual = str(Path(path).resolve())
        if actual in fresh: return original(path, data)
        if actual not in borrowed or len(data) != borrowed[actual]['size_bytes']:
            raise ValueError('inherited sidecar path or size changed')
        used.add(actual)
        return dict(borrowed[actual], identity_evidence='inherited-V23-layout-manifest', identity_freshly_revalidated=False)
    layout.source_record = record
    try:
        result = layout.build_layout(kernel, adt, output, physical_base=0xb207c000,
            window_size=768 << 20, full_ram=True, command_line=command,
            reserved_ranges=[dict(name=n, start=s, size=z) for n,s,z in NATIVE_FULL_RAM_EXCLUSIONS],
            trustcache_path=Path(inherited['sources']['TrustCache']['path']),
            data_seed_path=Path(inherited['sources']['NativeDataSeed']['path']),
            video=[0xf5a00000,0,2880,720,1280,0x320])
    finally:
        layout.source_record = original
    if used != set(borrowed): raise ValueError('inherited sidecars were not consumed')
    return result


def prepare(build_pass, output):
    build_pass = Path(build_pass).resolve(); output = safe_output(Path(output))
    if build_pass != DEFAULT_BUILD: raise ValueError('explicit native-root-smp-v1 build pass required')
    receipt_bytes = read_regular(build_pass / 'build-receipt.json'); receipt = json.loads(receipt_bytes)
    previous = json.loads(read_regular(BASE / 'build-receipt.json'))
    inherited = json.loads(read_regular(BASE / 'layout/manifest.json'))
    validate_receipt(receipt, previous)
    if (inherited['full_ram'] is not True or inherited['owned_window_size'] != 768 << 20
            or inherited['boot_memory_size'] != 4 << 30 or inherited['boot_phys_base'] != 0x80000000):
        raise ValueError('full-RAM native baseline layout required')
    command = command_line(inherited['command_line'])
    tree = oled.generate_adt(root_boot=True, storage_probe=True, full_ram=True, native_smp=True)
    output.mkdir(parents=True); adt = output / 'four-a57.adt'; adt.write_bytes(tree)
    prepared = build_layout(inherited, build_pass / 'kernel.macho', adt, output / 'layout', command)
    if (prepared['sources']['kernel']['sha256'] != receipt['kernel_sha256']
            or prepared['sources']['kernel']['size_bytes'] != receipt['kernel_bytes']):
        raise ValueError('new kernel differs from build receipt')
    result = dict(profile='native-four-a57-raw-apfs', stage='prepared', active_cpus=4,
        physical_mpidrs=[0,1,2,3], homogeneous_cluster=True, shared_l2_cache_id=0,
        timebase_frequency_hz=oled.TIMEBASE_HZ, psci_conduit='smc',
        command_line=command, build_pass=str(build_pass),
        build_receipt_sha256=hashlib.sha256(receipt_bytes).hexdigest(),
        adt=dict(path=str(adt.resolve()), size_bytes=len(tree), sha256=hashlib.sha256(tree).hexdigest()),
        layout=str((output / 'layout').resolve()), original_component_count=33,
        hardware_smp_qualified=False, physical_device_access=False, old_inputs_rehashed=False,
        single_core_fallback='Existing installed V24; unchanged by this helper',
        dyld_cache_directory='/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld',
        dyld_environment_source='existing native raw mount launchd environment',
        qualification='All four are requested. CPU_ON/MPIDR, GIC/timer/IPI and scheduler arrival still require native qualification.')
    (output / 'profile.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-pass', type=Path, default=DEFAULT_BUILD)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.build_pass, args.output), indent=2))
