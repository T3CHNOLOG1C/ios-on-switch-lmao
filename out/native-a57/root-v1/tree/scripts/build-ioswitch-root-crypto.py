#!/usr/bin/env python3
"""Build only portable verification, real XNU crypto adapter and pinned public key."""
import argparse, hashlib, json, struct, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[3]
KEY_SHA = 'b461cf5e80ed2cc20a61abc5c18b280567c4383cd92edf83867407e560ddc53b'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--key', type=Path, default=REPO/'vendor/unixnu/out/qemu/tree/build/verified-root-os/trusted-key.bin')
args = parser.parse_args()
key = args.key.resolve().read_bytes()
if len(key) != 395 or hashlib.sha256(key).hexdigest() != KEY_SHA:
    raise SystemExit('Require exact independently pinned owner public key')
out = ROOT/'build/verified-root-ioswitch'
out.mkdir(parents=True, exist_ok=True)
assembly = out/'trusted-key.S'
assembly.write_text('.section __DATA_CONST,__const\n.balign 8\n.globl _verified_root_key\n_verified_root_key:\n' +
    ''.join('.byte '+','.join(str(x) for x in key[i:i+16])+'\n' for i in range(0,len(key),16)))
recipe = ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json'
record = json.loads(recipe.read_text().strip().rstrip(','))
objects=[]; sources=[]; commands=[]
for source in [ROOT/'kits/VerifiedRoot/src/verified_root.c', ROOT/'kits/VerifiedRoot/adapters/xnu_crypto.c']:
    dest=out/(source.stem+'.o'); command=[]; arguments=record['arguments']; i=0
    while i<len(arguments):
        value=arguments[i]
        if value==record['file']:command.append(str(source))
        elif value=='-o':command.extend(['-o',str(dest)]); i+=1
        elif value in ('-MF','-MJ'):i+=1
        elif value not in ('-MD','-MP'):command.append(value)
        i+=1
    command.extend(['-I'+str(ROOT/'kits/VerifiedRoot/include'),'-Werror','-mgeneral-regs-only','-mstrict-align'])
    result=subprocess.run(command,cwd=record['directory'],capture_output=True,text=True)
    (out/(source.stem+'.log')).write_text(result.stdout+result.stderr)
    if result.returncode:raise SystemExit(result.stderr[-5000:])
    objects.append(dest); sources.append(source); commands.append(command)
key_obj=out/'trusted-key.o'
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
command=['xcrun','clang','-isysroot',sdk,'-target','arm64-apple-ios17.0','-Werror','-c',str(assembly),'-o',str(key_obj)]
subprocess.run(command,check=True);commands.append(command);objects.append(key_obj);sources.append(assembly)
for obj in objects:
    data=obj.read_bytes(); magic,cpu,subtype,kind,n,_=struct.unpack_from('<6I',data)
    if (magic,cpu,subtype,kind)!=(0xfeedfacf,0x100000c,0,1):raise SystemExit('Require ordinary ARM64 object')
    at=32;deployment=None
    for _ in range(n):
        cmd,size=struct.unpack_from('<II',data,at)
        if cmd==0x32:deployment=struct.unpack_from('<II',data,at+8)
        at+=size
    if deployment!=(2,0x110000):raise SystemExit('Require iOS17 ordinary ARM64 objects')
archive=out/'libVerifiedRootProbe.a'; archive.unlink(missing_ok=True)
subprocess.run(['xcrun','ar','rcs',str(archive),*[str(o) for o in objects]],check=True)
symbols=subprocess.check_output(['xcrun','nm','-g',str(archive)],text=True)
for forbidden in ['_verified_root_manifest','_verified_root_samples','_verified_root_policies','_native_verified_root_probe']:
    if forbidden in symbols:raise SystemExit('Unexpected embedded-table or legacy-probe symbol '+forbidden)
for wanted in ['_vr_open','_vr_verify_block','_vr_xnu_crypto','_verified_root_key']:
    if wanted not in symbols:raise SystemExit('Missing required verifier symbol '+wanted)
if archive.stat().st_size>256*1024:raise SystemExit('Key-only verifier archive unexpectedly large')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
report=dict(archive=str(archive),archive_sha256=sha(archive),archive_bytes=archive.stat().st_size,
    trusted_key_sha256=KEY_SHA,embedded_tables_absent=True,legacy_probe_absent=True,
    original_crypto_adapter=True,source_sha256={str(p.relative_to(ROOT)):sha(p) for p in sources},
    object_sha256={p.name:sha(p) for p in objects},public_key_bytes=395,
    signature_algorithm='RSA3072-PKCS1v1.5-SHA256',hardware_qualified=False)
(out/'build.json').write_text(json.dumps(report,indent=2)+'\n')
(out/'compile-commands.json').write_text(json.dumps(commands,indent=2)+'\n')
print(json.dumps(report,indent=2))
