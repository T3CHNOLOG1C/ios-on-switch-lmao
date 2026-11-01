#!/usr/bin/env python3
"""Package the reviewed fixed native XNU experiment; never installs or boots."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from scripts.package_native_handoff import (
    ROOT, REFERENCE, DTB_PATH, LOAD, MAX_RAW, legacy_image, pinned,
    read_regular, record, safe_output, tool, validate_elf,
)

PREFIX = 'switchroot/ioswitch-native-xnu/'
ENTRY = 'bootloader/ini/ioswitch-native-xnu.ini'
ROOT_POLICIES = {
    'OS.BIN': (47481664, 'b794233ad2418a8eb325b0c9f72c8a3937e01108e58d56932db1cd1befa3ef49'),
    'SYSTEMOS.BIN': (25805344, '81db43b73b6c83326d03cda92d466f4da8c9f1fb797983fc4577b701b796b91c'),
}

def root_policy_files(directory: Path) -> dict[str, bytes]:
    result = {}
    for name, (size, digest) in ROOT_POLICIES.items():
        data = read_regular(directory/name)
        if record(data) != {'size_bytes':size, 'sha256':digest}:
            raise ValueError('root policy identity mismatch: '+name)
        result['IOSWITCH/POLICY10/'+name] = data
    return result


def build(adapter: Path, layout: Path, output: Path, *, revision: int = 1,
          exporter: Path | None = None, root_policies: Path | None = None) -> dict:
    if type(revision) is not int or revision not in (1,2,3,4,5,6,7,8,9,10):
        raise ValueError('unsupported native experiment revision')
    if (revision==10) != (root_policies is not None):
        raise ValueError('root boot revision requires its exact signed policy files')
    if (revision>=7) != (exporter is not None):
        raise ValueError('file-test revision requires its separately built firmware exporter')
    prefix=PREFIX if revision==1 else f'switchroot/ioswitch-native-xnu-v{revision}/'
    output = safe_output(output)
    am_bytes, lm_bytes = read_regular(adapter/'manifest.json'), read_regular(layout/'manifest.json')
    am, lm = json.loads(am_bytes), json.loads(lm_bytes)
    if (am.get('profile') != 'native-xnu-entry-experiment' or am.get('model_only') is not False or
        am['constants']['NATIVE_GATES'] != 1 or am['layout_manifest_sha256'] != record(lm_bytes)['sha256'] or
        am['native_window_ownership_verified'] is not False):
        raise ValueError('not the exact native gated adapter/layout pair')
    if bool(am.get('root_boot',False)) != (revision==10):
        raise ValueError('root boot adapter and revision disagree')
    for name, expected in am['sources'].items():
        if hashlib.sha256(read_regular(ROOT/name)).hexdigest() != expected:
            raise ValueError('adapter source changed: '+name)
    for name, expected in am['files'].items():
        if record(read_regular(adapter/name)) != expected:
            raise ValueError('adapter output changed: '+name)
    raw, elf = read_regular(adapter/'loader.bin'), read_regular(adapter/'loader.elf')
    validate_elf(elf)
    if not 4 <= len(raw) <= MAX_RAW:
        raise ValueError('adapter footprint exceeds fixed bound')
    # Compare objcopy's exact bytes without trusting the manifest alone.
    import tempfile
    with tempfile.TemporaryDirectory(dir=ROOT/'out', prefix='xnu-package-') as tmp:
        binary = Path(tmp)/'loader.bin'
        subprocess.run([tool('llvm-objcopy'), '-O', 'binary', str(adapter/'loader.elf'), str(binary)], check=True)
        if binary.read_bytes() != raw:
            raise ValueError('adapter ELF/binary mismatch')
    payload = read_regular(layout/'kernel-window.bin')
    if (record(payload) != {k:lm['image'][k] for k in ('size_bytes','sha256')} or
        len(payload) != am['constants']['BYTES'] or zlib.crc32(payload) != am['constants']['CRC']):
        raise ValueError('kernel layout identity mismatch')
    if not 0 < len(payload) <= 0x6000000:
        raise ValueError('kernel exceeds native staging capacity')
    sidecar_names=('NativeDataSeed','TrustCache') if revision==10 else ('RAMDisk','TrustCache')
    sidecars={name:lm['sources'][name] for name in sidecar_names if name in lm['sources']}
    if revision>=3 and set(sidecars)!=set(sidecar_names):
        raise ValueError('bootstrap revision requires its exact pair of sidecars')
    if revision==10 and any(name in lm['sources'] for name in ('RAMDisk','PackedRAMDisk')):
        raise ValueError('root boot cannot include a diagnostic RAM root')
    if am['constants'].get('SD_REFERENCE',0) != int(revision>=4):
        raise ValueError('storage probe revision and loader reference contract differ')
    if am['constants'].get('SD_CARD',0) != int(revision>=7):
        raise ValueError('file-test revision and loader card contract differ')
    if revision>=7 and len(payload)>0x5000000:
        raise ValueError('kernel staging overlaps the exporter')
    if revision>=4 and (len(raw)>0x1f000 or int(am['elf_layout']['footprint_end_exclusive'],0)>0xa001f000):
        raise ValueError('SD reference page overlaps adapter code/data/stacks')
    image = legacy_image(raw, os_type=5, kind=2, name='IOSwitch native XNU loader', load=LOAD, entry=LOAD)
    dtb = pinned(DTB_PATH, 'oled-sku3.dtb')
    firmware = {n:pinned(REFERENCE/n,n) for n in ('bl31.bin','bl33.bin')}
    script = read_regular(ROOT/'native/loader/boot.cmd.in').decode('ascii')
    script=script.replace('native XNU experiment v1',f'native XNU experiment v{revision}')
    reference_name='native/storage/reference-files.cmd' if revision>=7 else 'native/storage/reference.cmd'
    reference_script=read_regular(ROOT/reference_name) if revision>=4 else b''
    if '@SD_REFERENCE_PREPARE@' not in script:
        raise ValueError('missing SD reference insertion point')
    script=script.replace('@SD_REFERENCE_PREPARE@',reference_script.decode('ascii').rstrip())
    exporter_script=b''; exporter_image=b''; exporter_manifest=None; exporter_sources={}
    if revision>=7:
        em_bytes=read_regular(exporter/'manifest.json');em=json.loads(em_bytes)
        if (em.get('profile')!='pinned-bl33-ram-card-metadata-export-v1' or
            em.get('bl33_sha256')!=record(firmware['bl33.bin'])['sha256'] or
            em.get('entry')!=0xa7000000 or em.get('record_pa')!=0xa001f400 or
            em.get('record_bytes')!=256 or not 0<em.get('footprint_bytes',0)<=0x10000):
            raise ValueError('unsupported firmware exporter contract')
        expected_sources={'export.c','export.h','native.c','entry.S','link.ld','signatures.h','build.py'}
        if set(em['source_sha256'])!=expected_sources or set(em['files'])!={'exporter.bin','exporter.elf','exporter.uimg'}:
            raise ValueError('unexpected firmware exporter file set')
        for name,digest in em['source_sha256'].items():
            path='native/storage/firmware_export/'+name
            if record(read_regular(ROOT/path))['sha256']!=digest:
                raise ValueError('firmware exporter source changed: '+name)
            exporter_sources[path]={'sha256':digest}
        for name,expected in em['files'].items():
            if record(read_regular(exporter/name))!=expected:
                raise ValueError('firmware exporter file changed: '+name)
        exporter_raw=read_regular(exporter/'exporter.bin')
        exporter_image=read_regular(exporter/'exporter.uimg')
        if len(exporter_raw)!=em['footprint_bytes'] or exporter_image!=legacy_image(
            exporter_raw,os_type=17,kind=1,name='IOSwitch MMC RAM Export v1',load=0xa7000000,entry=0xa7000000):
            raise ValueError('firmware exporter wrapper mismatch')
        with tempfile.TemporaryDirectory(dir=ROOT/'out',prefix='exporter-package-') as tmp:
            binary=Path(tmp)/'exporter.bin'
            subprocess.run([tool('llvm-objcopy'),'-O','binary',str(exporter/'exporter.elf'),str(binary)],check=True)
            if binary.read_bytes()!=exporter_raw:
                raise ValueError('firmware exporter ELF/binary mismatch')
        exporter_manifest={'path':str((exporter/'manifest.json').resolve()),**record(em_bytes)}
        exporter_script=read_regular(ROOT/'native/storage/exporter.cmd')
    if '@SD_EXPORTER_PREPARE@' not in script:
        raise ValueError('missing firmware exporter insertion point')
    script=script.replace('@SD_EXPORTER_PREPARE@',exporter_script.decode('ascii').rstrip())
    script=script.replace('/'+PREFIX,'/'+prefix)
    for name,data in [('IMAGE',image),('DTB',dtb),('KERNEL',payload)]:
        for token,value in [(f'@{name}_BYTES_HEX@',hex(len(data))), (f'@{name}_CRC32@',f'{zlib.crc32(data):08x}')]:
            if token not in script:
                raise ValueError('missing script token '+token)
            script=script.replace(token,value)
    if revision>=7:
        for name,data in [('EXPORTER',exporter_image),('EXPORTER_RAW',exporter_raw)]:
            for token,value in [(f'@{name}_BYTES_HEX@',hex(len(data))),(f'@{name}_CRC32@',f'{zlib.crc32(data):08x}')]:
                if token not in script:raise ValueError('missing exporter script token '+token)
                script=script.replace(token,value)
    if re.search(r'@[A-Z0-9_]+@',script):
        raise ValueError('unresolved script token')
    script=script.encode('ascii')
    script_image=legacy_image(struct.pack('>II',len(script),0)+script,os_type=17,kind=6,name='IOSwitch native XNU script')
    if len(script_image)>0x20000:
        raise ValueError('script exceeds reserved range')
    artifacts={prefix+n:d for n,d in firmware.items()}
    artifacts.update({prefix+'loader.bin':raw,prefix+'loader.uboot.bin':image,
        prefix+'kernel-window.bin':payload,prefix+'oled-sku3.dtb':dtb,
        prefix+'boot.cmd':script,prefix+'boot.scr':script_image,
        ENTRY:f'[IOSwitch Native XNU]\nl4t=1\nboot_prefixes=/{prefix}\nid=IOSWXNU\n'.encode('ascii')})
    if revision>=7:artifacts[prefix+'exporter.uimg']=exporter_image
    if revision==10:artifacts.update(root_policy_files(root_policies))
    manifest={'schema_version':1,'profile':'ioswitch-native-xnu-experiment','revision':revision,
        'status':'awaiting-independent-review','installs_or_reboots':False,
        'native_xnu_boot_verified':False,'native_window_ownership_verified':False,
        'bootstrap_sidecars':sidecars,
        'proof_boundary':('First native original-root startup attempt using exact owner-signed development policies, verified read-only OS/SystemOS reads and disposable RAM Data. Apple root-authentication errors remain recorded; no native Apple userspace or UI success is implied.' if revision==10 else 'Single-CPU XNU with own signed PID1, persistent read-only split-disk block devices and original APFS ordinary data-mount/file tests. Native block/APFS result pending; rd=md0 remains the root and iPadOS is not booted.' if revision>=9 else 'Single-CPU XNU with own signed PID1 and bounded read-only SDHC/GPT/FAT32/split-file verification. Native file-read result pending; iPadOS is not booted.' if revision>=7 else 'Single-CPU XNU with own signed PID1 and a bounded read-only SD sector-zero comparison against same-boot firmware data. Native SD result pending; no FAT, root-disk driver or iPadOS apps.' if revision>=4 else 'Single-CPU XNU with an own signed RAM-disk test process; native EL0 execution is pending. No native SD driver or iPadOS apps. Conditional memory exclusion checks precede kernel destination writes.' if revision==3 else 'Finite single-CPU early XNU experiment. No storage driver or iPadOS userspace. Conditional memory exclusion checks precede kernel destination writes.'),
        'firmware_exporter_manifest':exporter_manifest,
        'adapter_manifest':{'path':str((adapter/'manifest.json').resolve()),**record(am_bytes)},
        'layout_manifest':{'path':str((layout/'manifest.json').resolve()),**record(lm_bytes)},
        'constants':am['constants'],'exclusions':lm['reserved_ranges'],
        'sources':{**{k:{'sha256':v} for k,v in am['sources'].items()},
                   'native/loader/boot.cmd.in':record(read_regular(ROOT/'native/loader/boot.cmd.in')),
                   **({reference_name:record(reference_script)} if revision>=4 else {}),
                   **({'native/storage/exporter.cmd':record(exporter_script),**exporter_sources} if revision>=7 else {}),
                   'scripts/package_native_xnu.py':record(read_regular(Path(__file__)))},
        'files':{n:record(d) for n,d in artifacts.items()},
        'validation':{f'{name}_crc32':f'{zlib.crc32(data):08x}' for name,data in [('image',image),('dtb',dtb),('kernel',payload)]},
        'runtime_checks':['OLED SKU and firmware fingerprint','exact FAT metadata and CRCs',
            'post-cleanup EL, caches, timer and two DRAM banks','complete FDT reservations and reserved-memory nodes',
            'photographed MC carveouts and retained firmware stack range','source and destination CRC','physical boot arguments'],
        'limitations':['Native execution pending; generic CPU model does not emulate Tegra',
            'MMIO display and GIC behavior depends on inherited firmware state',
            'No persistent storage writes, saveenv or authored reset; kernel faults can halt before display',
            'Memory ownership inferred from the selected boot path and measured exclusion ledger, not certified by firmware']}
    output.mkdir(parents=True)
    for name,data in artifacts.items():
        path=output/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
        if record(read_regular(path))!=manifest['files'][name]:raise ValueError('package readback mismatch')
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--adapter',required=True,type=Path);p.add_argument('--layout',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--revision',type=int,choices=(1,2,3,4,5,6,7,8,9,10),default=1)
    p.add_argument('--exporter',type=Path)
    p.add_argument('--root-policies',type=Path,help='Directory containing exact OS.BIN and SYSTEMOS.BIN signed policies for revision 10')
    a=p.parse_args();print(json.dumps(build(a.adapter,a.layout,a.output,revision=a.revision,exporter=a.exporter,root_policies=a.root_policies),indent=2))
