#!/usr/bin/env python3
"""Host behavior tests for actual RAM driver/core; not kernel or hardware proof."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[2]
HELPER = TREE/'scripts/build-native-ram-block.py'
spec = importlib.util.spec_from_file_location('ram_build', HELPER)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seed', type=Path)
    parser.add_argument('--uuid', help='Defaults to the UUID in build/native-ram-block/seed.json')
    args = parser.parse_args()
    pins = json.loads((TREE/'build/native-ram-block/seed.json').read_text())
    seed = args.seed or Path(pins['seed_path'])
    volume_uuid = args.uuid or pins['expected_volume_uuid']
    metadata = helper.seed_metadata(seed, volume_uuid)
    outputs = []
    start = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='native-ram-test-') as directory:
        work = Path(directory)
        (work/'NativeDataSeed.h').write_text(helper.seed_header(metadata))
        for name in ['IOKit/IOLib.h','IOKit/IOKitKeys.h','IOKit/storage/IOBlockStorageDevice.h',
                     'IOKit/IODeviceTreeSupport.h','IOKit/IOBSD.h','libkern/c++/OSData.h',
                     'libkern/c++/OSDictionary.h','libkern/c++/OSIterator.h','libkern/c++/OSNumber.h',
                     'libkern/c++/OSString.h','libkern/crypto/sha2.h','machine/machine_routines.h',
                     'pexpert/pexpert.h','pexpert/arm64/boot.h']:
            path = work/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "'+str(HERE/'Mock.h')+'"\n')
        for source in ['sparse.cpp','driver.cpp','packed-hash.cpp']:
            binary = work/source.removesuffix('.cpp')
            command = ['xcrun','clang++','-std=c++20','-Wall','-Wextra','-Werror',
                       '-Wno-unused-parameter','-Wno-deprecated-declarations',
                       '-fsanitize=address,undefined','-g','-pthread','-I',str(work),
                       str(HERE/source),'-o',str(binary)]
            built = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if built.returncode:
                raise SystemExit(built.stdout)
            run = subprocess.run([binary]+([seed] if source in ('driver.cpp','packed-hash.cpp') else []), text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            if run.returncode:
                raise SystemExit(run.stdout)
            if source=='packed-hash.cpp':
                observed=run.stdout.split()
                assert observed==[metadata['logical_sha256'],str(metadata['logical_bytes']),str(metadata['initial_resident_bytes'])]
            outputs.append(run.stdout.strip())
            print(run.stdout.strip())
        # Independently reconstruct a small sparse disk to check the builder's
        # logical hash, gaps, explicit zeros, resident accounting and strict end.
        raw = bytes(8192)+b'Q'*4096+bytes(4096)
        packed = struct.pack('<8sIIQQQQ',b'DBRDISK1',1,4096,len(raw),1,8192,4096)+b'Q'*4096
        path = work/'small.dbr'
        path.write_bytes(packed)
        info = helper.seed_metadata(path,volume_uuid)
        assert info['logical_sha256']==hashlib.sha256(raw).hexdigest()
        assert info['encoded_sha256']==hashlib.sha256(packed).hexdigest()
        assert info['nonzero_pages']==1 and info['pointer_table_bytes']==32
        mutations=[]
        for offset,encoding,value in [(0,'8s',b'NOTDBR00'),(8,'I',2),(12,'I',8192),
                                      (16,'Q',0),(16,'Q',(1<<30)+4096),(24,'Q',65537),
                                      (32,'Q',1),(40,'Q',0),(40,'Q',1<<63)]:
            bad=bytearray(packed);struct.pack_into('<'+encoding,bad,offset,value);mutations.append(bytes(bad))
        mutations += [packed[:-1],packed+b'\0']
        for bad in mutations:
            path.write_bytes(bad)
            try: helper.seed_metadata(path,volume_uuid)
            except ValueError: pass
            else: raise AssertionError('Malformed seed accepted')
        outputs.append('PASS: host pin generation full logical SHA256, packed SHA256, geometry/cap validation')
        print(outputs[-1])
    files = [HERE/'sparse.cpp',HERE/'driver.cpp',HERE/'packed-hash.cpp',HERE/'Mock.h',HERE/'run_tests.py',
             HERE.parent/'NativeRamBlock.cpp',HERE.parent/'SparseDisk.h',HELPER]
    report = dict(passed=True, sanitizer='AddressSanitizer+UndefinedBehaviorSanitizer',
                  seed=metadata, results=outputs, elapsed_seconds=round(time.monotonic()-start,3),
                  sources={str(p.relative_to(TREE)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
                  hardware_qualified=False, actual_kernel_iokit_execution=False)
    output = TREE/'build/native-ram-block/host-tests.json'
    output.write_text(json.dumps(report,indent=2)+'\n')
    print(output)


if __name__=='__main__':
    main()
