#!/usr/bin/env python3
"""Stage the prepared APFS image and installer on the identified USB FAT volume.

This writes new FAT files only. Android is erased only by the separate payload
after an on-device target selection and physical confirmation.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import plistlib
import stat
import subprocess
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from scripts.make_native_install_manifest import encode, geometry

MOUNT=Path('/Volumes/SWITCH SD')
VOLUME_UUID='6A93F13E-EC7C-373F-8778-968D6D3750DD'
PARTITION_UUID='2FBBA66A-A356-00C1-CD9F-1DCBB51C2CA9'
CONTAINER_UUID='50F8A67F-AFC1-4A1B-BCBB-82162674C67F'
STAGING='IOSWITCH/INSTALL/RAW24'
MANIFEST='IOSWITCH/INSTALL/RAW24.IWI'
PAYLOAD='bootloader/payloads/IOSWAPFS.BIN'
MENU='bootloader/ini/ioswitch-apfs-installer.ini'
MENU_BYTES=b'[IOSwitch APFS Installer]\npayload=bootloader/payloads/IOSWAPFS.BIN\n'
PART_BYTES=2<<30
CHUNK=8<<20
FREE_MARGIN=32<<20

def regular(path):
    path=Path(path)
    if any(p.is_symlink() for p in (path,*path.parents)) or not stat.S_ISREG(path.stat().st_mode):
        raise ValueError('expected nonsymlink regular file: '+str(path))
    return path

def identity(st):
    return dict(device=st.st_dev,inode=st.st_ino,size=st.st_size,mtime_ns=st.st_mtime_ns,
                ctime_ns=st.st_ctime_ns,allocated_bytes=st.st_blocks*512,uid=st.st_uid)

def detached(image):
    result=subprocess.run(['hdiutil','info','-plist'],check=True,capture_output=True)
    for row in plistlib.loads(result.stdout).get('images',[]):
        if row.get('image-path') and Path(row['image-path']).resolve()==image:
            raise ValueError('candidate is attached; detach it before staging')

def disk_info():
    result=subprocess.run(['diskutil','info','-plist',str(MOUNT)],check=True,capture_output=True)
    info=plistlib.loads(result.stdout)
    if (info.get('VolumeUUID')!=VOLUME_UUID or info.get('DiskUUID')!=PARTITION_UUID or
        info.get('MountPoint')!=str(MOUNT) or info.get('FilesystemType')!='msdos' or
        info.get('Internal') is not False or info.get('WritableVolume') is not True or
        info.get('BusProtocol')!='USB' or info.get('WholeDisk') is not False):
        raise ValueError('not the identified Switch USB FAT partition')
    return info

def payload_bytes(build):
    build=Path(build).resolve()
    if not build.is_relative_to(ROOT/'out'):raise ValueError('installer build must be below workspace out')
    meta=json.loads(regular(build/'manifest.json').read_text())
    if (meta['profile']!='hekate-bpmp-apfs-installer' or
        meta['commit']!='45cd81ed08e30a26ba380958c7e20b55c342834b' or
        meta['device_access'] is not False or meta['storage_writes_implemented'] is not True or
        meta['linked_storage_writes_refused'] is not True or
        meta['destructive_dma_requires_on_device_confirmation'] is not True):
        raise ValueError('full reviewed APFS installer required')
    required={'native/installer/main.c','native/installer/install.c','native/installer/install.h',
              'native/installer/storage.c','native/installer/storage.h',
              'native/storage/raw_apfs.c','native/storage/raw_apfs.h'}
    if not required<=meta.get('inputs',{}).keys():raise ValueError('installer header/source identities missing')
    for name,digest in meta['inputs'].items():
        path=ROOT/name
        if not path.resolve().is_relative_to(ROOT) or hashlib.sha256(regular(path).read_bytes()).hexdigest()!=digest:
            raise ValueError('installer build input changed: '+name)
    data=regular(build/'payload.bin').read_bytes()
    if len(data)!=meta['payload_bytes'] or hashlib.sha256(data).hexdigest()!=meta['payload_sha256']:
        raise ValueError('installer payload identity differs')
    return data,meta

def candidate(receipt):
    row=json.loads(regular(receipt).read_text())
    if (row['state']!='full_data_and_cryptex_staged_detached' or
        row['full_data_population_complete'] is not True or row['final_candidate_detached'] is not True or
        row['detached_metadata']['container_uuid']!=CONTAINER_UUID or
        row['detached_metadata']['metadata_checksum_failures']!=0):
        raise ValueError('complete detached candidate receipt required')
    image=regular(Path(row['image'])).resolve()
    if not image.is_relative_to(ROOT/'out') or identity(image.stat())!=row['candidate_stat']:
        raise ValueError('prepared candidate identity changed')
    size=row['logical_bytes']
    if size!=row['candidate_stat']['size'] or size!=row['android_target']['TotalSize']:
        raise ValueError('candidate and recorded Android target capacities differ')
    with image.open('rb') as stream:first=stream.read(65536)
    block=geometry(first,size)
    if str(uuid.UUID(bytes=first[72:88])).upper()!=CONTAINER_UUID:
        raise ValueError('unexpected APFS container identity')
    return image,row,block

def verify_new(path,size,digest,check):
    sha=hashlib.sha256();count=0
    with regular(path).open('rb') as stream:
        while data:=stream.read(CHUNK):
            check();sha.update(data);count+=len(data)
    if count!=size or sha.hexdigest()!=digest:raise ValueError('new file readback differs: '+str(path))

def write_new(path,data,check):
    check()
    with path.open('xb') as stream:
        stream.write(data);stream.flush();os.fsync(stream.fileno())
    verify_new(path,len(data),hashlib.sha256(data).hexdigest(),check)

def stream_parts(source,directory,total,check,part_bytes=PART_BYTES,boundary=None):
    """Copy once while hashing, then read back only each newly written part."""
    whole=hashlib.sha256();rows=[];offset=0
    while offset<total:
        if boundary:boundary()
        count=min(part_bytes,total-offset);name=f'APFS{len(rows):04d}.BIN';path=directory/name
        check();part=hashlib.sha256();remaining=count
        with path.open('xb') as target:
            while remaining:
                check();data=source.read(min(CHUNK,remaining))
                if not data:raise ValueError('candidate ended during staging')
                target.write(data);whole.update(data);part.update(data);remaining-=len(data)
            target.flush();os.fsync(target.fileno())
        verify_new(path,count,part.hexdigest(),check)
        offset+=count;rows.append(dict(name=name,size_bytes=count,sha256=part.hexdigest()))
        print(json.dumps({'part_readback_verified':len(rows),'parts':(total+part_bytes-1)//part_bytes,
                          'bytes_complete':offset,'image_bytes':total}),flush=True)
    if source.read(1):raise ValueError('candidate grew during staging')
    return rows,whole.digest()

def stage(receipt,build,output,execute):
    image,row,block=candidate(receipt);payload,meta=payload_bytes(build)
    total=row['logical_bytes'];required=total+len(payload)+64+256*((total+PART_BYTES-1)//PART_BYTES)+len(MENU_BYTES)+FREE_MARGIN
    plan=dict(image=str(image),image_bytes=total,parts=(total+PART_BYTES-1)//PART_BYTES,
              required_FAT_free_bytes=required,new_staging=STAGING,manifest=MANIFEST,
              installer_payload=PAYLOAD,new_menu=MENU,android_writes_by_host=False,
              old_SD_contents_copied_or_hashed=False,installer_sha256=meta['payload_sha256'])
    if not execute:return plan
    output=Path(output)
    if not output.resolve().is_relative_to(ROOT/'out') or output.exists() or output.is_symlink():
        raise ValueError('new host receipt below workspace out required')
    detached(image)
    info=disk_info();device=MOUNT.stat().st_dev
    for parent in (MOUNT,MOUNT/'bootloader',MOUNT/'bootloader/ini',MOUNT/'bootloader/payloads',MOUNT/'IOSWITCH'):
        if not parent.is_dir() or any(p.is_symlink() for p in (parent,*parent.parents)):
            raise ValueError('invalid target ancestry: '+str(parent))
    install=MOUNT/'IOSWITCH/INSTALL'
    if install.exists() and (not install.is_dir() or install.is_symlink()):raise ValueError('invalid install directory')
    for name in (STAGING,MANIFEST,PAYLOAD,MENU):
        path=MOUNT/name
        if path.exists() or path.is_symlink():raise ValueError('new output already exists: '+name)
    fs=os.statvfs(MOUNT)
    if fs.f_bavail*fs.f_frsize<required:raise ValueError('insufficient FAT space for new image files and margin')
    state=dict(plan,state='staging',entry_published=False,all_new_files_verified=False,
               disk_uuid=PARTITION_UUID,volume_uuid=VOLUME_UUID,files=[])
    def save():
        output.parent.mkdir(parents=True,exist_ok=True)
        output.write_text(json.dumps(state,indent=2)+'\n')
    def session(full=False):
        if MOUNT.stat().st_dev!=device:raise ValueError('USB mount session changed')
        if identity(image.stat())!=row['candidate_stat']:raise ValueError('candidate changed while staging')
        if full:
            current=disk_info()
            if any(current[k]!=info[k] for k in ('DeviceIdentifier','ParentWholeDisk','TotalSize')):
                raise ValueError('USB device identity changed')
    save()
    try:
        install.mkdir(exist_ok=True);directory=MOUNT/STAGING;directory.mkdir()
        with regular(image).open('rb') as source:
            def check():
                session()
                if identity(os.fstat(source.fileno()))!=row['candidate_stat']:raise ValueError('open candidate changed')
            check();rows,digest=stream_parts(source,directory,total,check,part_bytes=PART_BYTES,
                                           boundary=lambda:session(True))
        for part in rows:part['path']=STAGING+'/'+part.pop('name')
        state['files']=rows;state['image_sha256']=digest.hex();save()
        session(True);detached(image)
        manifest=encode([(part['path'],part['size_bytes']) for part in rows],total,block,digest)
        write_new(MOUNT/MANIFEST,manifest,session)
        session(True);write_new(MOUNT/PAYLOAD,payload,session)
        # The visible installer entry is the final new file, after every part passed readback.
        session(True);write_new(MOUNT/MENU,MENU_BYTES,session)
        state.update(state='complete',entry_published=True,all_new_files_verified=True)
        state['files']+= [dict(path=name,size_bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
                         for name,data in ((MANIFEST,manifest),(PAYLOAD,payload),(MENU,MENU_BYTES))]
        save();subprocess.run(['sync'],check=True)
    except BaseException as error:
        state.update(state='failed',error=str(error));save();raise
    return state

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate-receipt',type=Path,default=ROOT/'out/native-single-apfs-trial-v1/candidate.json')
    parser.add_argument('--installer-build',type=Path,default=ROOT/'out/native-apfs-installer-v24')
    parser.add_argument('--receipt',type=Path,default=ROOT/'out/switch/native-apfs-installer-v24-stage.json')
    parser.add_argument('--stage',action='store_true',help='write NEW FAT staging files; no Android partition writes')
    args=parser.parse_args()
    print(json.dumps(stage(args.candidate_receipt,args.installer_build,args.receipt,args.stage),indent=2))
