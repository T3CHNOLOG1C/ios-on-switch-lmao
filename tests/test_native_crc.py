"""Execute the exact ARM CRC helper against independent zlib fixtures."""
import random
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zlib

from scripts.package_native_handoff import tool

ROOT=Path(__file__).resolve().parents[1]

class NativeCrcTests(unittest.TestCase):
    def test_crc32_matches_zlib_at_all_alignments_and_chunk_boundaries(self):
        qemu=shutil.which('qemu-system-aarch64')
        if not qemu:
            raise unittest.SkipTest('AArch64 CPU model required for instruction-level CRC check')
        try:
            clang,linker=tool('clang'),tool('ld.lld')
        except ValueError as exc:
            raise unittest.SkipTest(str(exc))
        data=random.Random(7801).randbytes(65544)
        cases=[(offset,size,zlib.crc32(data[offset:offset+size]))
               for offset in range(8)
               for size in (*range(17),31,32,33,255,256,257,4096,65535,65536)]
        with tempfile.TemporaryDirectory(prefix='iosw-crc-') as tmp:
            p=Path(tmp);(p/'input.bin').write_bytes(data)
            fixtures=',\n'.join('{%d,%d,0x%x}'%c for c in cases)
            (p/'test.c').write_text('''
#include <stdint.h>
extern const uint8_t input[];
extern uint32_t loader_crc32_update(uint32_t, const void *, uint64_t);
struct sample { uint32_t offset, size, expected; };
static const struct sample cases[]={'''+fixtures+'''};
int test_main(void) {
  for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
    const struct sample *s=&cases[i];
    if (~loader_crc32_update(UINT32_MAX,input+s->offset,s->size)!=s->expected) return 1;
    uint32_t crc=UINT32_MAX;
    for (unsigned offset=0;offset<s->size;) {
      unsigned size=s->size-offset;
      if (size>113)size=113;
      crc=loader_crc32_update(crc,input+s->offset+offset,size);offset+=size;
    }
    if (~crc!=s->expected) return 2;
  }
  return 0;
}
''')
            # The only semihost operation is SYS_EXIT_EXTENDED. No host file IO.
            (p/'entry.S').write_text('''
.section .text.entry,"ax"
.global _start
_start:
  ldr x0,=0xa0020000
  mov sp,x0
  bl test_main
  adr x1,exit_parameters
  str x0,[x1,#8]
  mov x0,#0x20
  hlt #0xf000
1: wfe
  b 1b
.data
.balign 8
exit_parameters: .quad 0x20026,0
.section .rodata,"a"
.global input
input:
.incbin "input.bin"
''')
            (p/'link.ld').write_text('ENTRY(_start)\nSECTIONS { . = 0xA0000000; .text : { *(.text.entry) *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }')
            objects=[]
            for i,source in enumerate([p/'entry.S',p/'test.c',ROOT/'native/loader/checksum.S']):
                obj=p/f'{i}.o';objects.append(str(obj))
                subprocess.run([clang,'--target=aarch64-none-elf','-march=armv8-a','-mstrict-align',
                                '-mgeneral-regs-only','-ffreestanding','-fno-builtin','-fno-stack-protector',
                                '-O2','-Wall','-Wextra','-Werror','-c',str(source),'-o',str(obj)],
                               cwd=p,check=True,capture_output=True)
            subprocess.run([linker,'-T',str(p/'link.ld'),'-o',str(p/'test.elf'),*objects],check=True,capture_output=True)
            result=subprocess.run([qemu,'-machine','virt','-cpu','cortex-a57','-accel','tcg','-smp','1',
                '-m','2G','-display','none','-serial','none','-monitor','none','-nodefaults','-no-reboot',
                '-semihosting-config','enable=on,target=native','-device',f'loader,file={p}/test.elf,cpu-num=0'],
                timeout=15,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(len(cases),208)

if __name__=='__main__':
    unittest.main()
