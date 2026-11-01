#!/usr/bin/env python3
"""Build the standalone BPMP APFS installer; no device access."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import struct

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'out/research-native/sol-hekate-source'
OUTPUT=ROOT/'out/research-native/sol-installer-platform'
PIN='45cd81ed08e30a26ba380958c7e20b55c342834b'
# Reduced subset of the reviewed official Makefile; no Nyx/modules/tools recursion.
UNITS='''bootloader/start.S bdk/exception_handlers.S bootloader/gfx/gfx.c
bdk/mem/heap.c bdk/soc/bpmp.c bdk/soc/ccplex.c bdk/soc/clock.c bdk/display/di.c
bdk/soc/i2c.c bdk/soc/irq.c bdk/soc/timer.c bdk/mem/mc.c bdk/mem/sdram.c bdk/mem/smmu.c bdk/mem/minerva.c
bdk/soc/gpio.c bdk/soc/pinmux.c bdk/soc/pmc.c bdk/sec/se.c bdk/sec/tsec.c
bdk/soc/uart.c bdk/soc/fuse.c bdk/soc/kfuse.c bdk/storage/sdmmc.c
bdk/storage/sdmmc_driver.c bdk/storage/sd.c bdk/storage/emmc.c bdk/power/bq24193.c
bdk/power/max17050.c bdk/power/max7762x.c bdk/rtc/max77620-rtc.c bdk/soc/hw_init.c
bdk/utils/btn.c bdk/utils/util.c bootloader/libs/fatfs/diskio.c
bdk/libs/fatfs/ff.c bdk/libs/fatfs/ffunicode.c bdk/libs/fatfs/ffsystem.c'''.split()

def build(output=OUTPUT):
    output=Path(output).absolute()
    if not output.resolve().is_relative_to(ROOT/'out') or any(p.is_symlink() for p in (output,*output.parents)):
        raise ValueError('output must be a new directory below workspace out')
    output=output.resolve()
    commit=subprocess.check_output(['git','-C',str(SOURCE),'rev-parse','HEAD'],text=True).strip()
    if commit!=PIN:raise ValueError('official Hekate source pin changed')
    if output.exists():raise ValueError('output already exists')
    gcc=shutil.which('arm-none-eabi-gcc');objcopy=shutil.which('arm-none-eabi-objcopy')
    if not gcc or not objcopy:raise RuntimeError('existing ARM bare-metal compiler and objcopy required')
    output.mkdir()
    flags=['-march=armv4t','-mtune=arm7tdmi','-mthumb','-mthumb-interwork','-O2','-g',
           '-nostdlib','-ffunction-sections','-fdata-sections','-fomit-frame-pointer','-fno-inline','-std=gnu11',
           '-DBDK_MALLOC_NO_DEFRAG','-DIPL_LOAD_ADDR=0x40008000',
           '-DGFX_INC="'+str(SOURCE/'bootloader/gfx/gfx.h')+'"',
           '-DFFCFG_INC="'+str(ROOT/'native/installer/ffconf.h')+'"',
           '-I'+str(SOURCE/'bdk'),'-I'+str(SOURCE/'bootloader'),'-Wall','-Wextra',
           '-Wno-unused-parameter','-Wno-array-bounds','-Wno-stringop-overread','-Wno-stringop-overflow']
    sources=[ROOT/'native/installer/main.c',ROOT/'native/installer/install.c',ROOT/'native/installer/storage.c',ROOT/'native/storage/raw_apfs.c',*(SOURCE/path for path in UNITS)]
    inputs=sources+[ROOT/path for path in ('native/installer/install.h','native/installer/storage.h','native/installer/ffconf.h','out/research-native/sol-hekate-source/bootloader/libs/fatfs/ffconf.h','native/storage/raw_apfs.h')]
    identities={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    commands=[];objects=[]
    for i,source in enumerate(sources):
        obj=output/f'part-{i}.o';objects.append(obj)
        commands.append([gcc,*flags,*(['-include','stdint.h'] if source.suffix=='.c' else []),'-c',str(source),'-o',str(obj)])
    elf=output/'payload.elf'
    commands.append([gcc,'-march=armv4t','-mthumb-interwork','-nostartfiles','-Wl,--nmagic,--gc-sections',
                     '-Wl,--wrap=sdmmc_storage_write,--wrap=sdmmc_storage_read',
                     '-Wl,--defsym=IPL_LOAD_ADDR=0x40008000','-Wl,-Map='+str(output/'payload.map'),
                     '-T',str(SOURCE/'bootloader/link.ld'),*map(str,objects),'-lgcc','-o',str(elf)])
    for i,command in enumerate(commands):
        result=subprocess.run(command,capture_output=True,text=True)
        (output/f'command-{i}.log').write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(f'command {i} failed: '+result.stdout+result.stderr)
    data=elf.read_bytes()
    if data[:7]!=b'\x7fELF\x01\x01\x01' or struct.unpack_from('<H',data,18)[0]!=40 or struct.unpack_from('<I',data,24)[0]!=0x40008000:
        raise ValueError('wrong ELF class, architecture or BPMP entry')
    symbols={fields[2]:int(fields[0],16) for line in subprocess.check_output(['arm-none-eabi-nm','-n',str(elf)],text=True).splitlines()
             if len(fields:=line.split())==3 and fields[0]!='U'}
    if symbols['__bss_end']>0x4003ff00 or symbols['__ipl_end']%16:
        raise ValueError('payload overlaps the initial stack or has an unaligned relocation extent')
    subprocess.run([objcopy,'-S','-O','binary','--pad-to='+hex(symbols['__ipl_end']),str(elf),str(output/'payload.bin')],check=True)
    size=(output/'payload.bin').stat().st_size
    if size>0x30000:raise ValueError('payload exceeds Hekate small-payload IRAM limit')
    if any(hashlib.sha256((ROOT/path).read_bytes()).hexdigest()!=identity for path,identity in identities.items()):
        raise ValueError('installer input changed during build')
    report={'profile':'hekate-bpmp-apfs-installer','upstream':str(SOURCE),'commit':commit,
            'compiler':subprocess.check_output([gcc,'--version'],text=True).splitlines()[0],
            'payload_bytes':size,'payload_sha256':hashlib.sha256((output/'payload.bin').read_bytes()).hexdigest(),
            'elf_layout':{key:hex(symbols[key]) for key in ('_start','__ipl_end','__bss_start','__bss_end')},
            'sources':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
            'inputs':identities,
            'commands':commands,'device_access':False,'storage_writes_implemented':True,'linked_storage_writes_refused':True,'destructive_dma_requires_on_device_confirmation':True,
            'proof_boundary':'Locally cross-compiled only; no hardware execution or installation.'}
    (output/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('profile','commit','payload_bytes','payload_sha256')},indent=2))
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=OUTPUT)
    build(parser.parse_args().output)
