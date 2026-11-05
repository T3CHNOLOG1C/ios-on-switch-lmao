#!/usr/bin/env python3
"""Exercise the exact RAM reader with synthetic firmware objects; no devices."""
from pathlib import Path
import ctypes
import hashlib
import json
import struct
import subprocess
import tempfile
import unittest
import zlib

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
BASE=0xf1000000
LOW=BASE-0x402000
GD=LOW-0x1000
END=BASE+0x68000
UC0=LOW+0x1000
UC=LOW+0x1100
BD0=LOW+0x2000
BD=LOW+0x2200
DESC0=LOW+0x3000
DESC=LOW+0x3200
PARENT=LOW+0x4000
PLAT=LOW+0x5000
MMC=PLAT+0x20
PRIVATE=LOW+0x6000
UPRIVATE=LOW+0x7000
CAPACITY=1023871549440//512
COPY=ctypes.CFUNCTYPE(ctypes.c_int,ctypes.c_void_p,ctypes.c_uint64,ctypes.POINTER(ctypes.c_uint8),ctypes.c_uint32)

class Reader(ctypes.Structure):
    _fields_=[('context',ctypes.c_void_p),('copy',COPY)]

class TestExport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='switch-fw-export-')
        path=Path(cls.temp.name)/'export.dylib'
        subprocess.run(['clang','-std=c11','-O2','-shared','-fPIC','-Wall','-Wextra','-Werror',
                        str(HERE/'export.c'),'-o',str(path)],check=True)
        cls.lib=ctypes.CDLL(str(path));cls.lib.iosw_fw_capture.argtypes=[ctypes.POINTER(Reader),ctypes.c_uint64,ctypes.POINTER(ctypes.c_uint8)]
        cls.lib.iosw_fw_capture.restype=ctypes.c_int

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def setUp(self):
        self.mem=bytearray(END-GD);self.accesses=[];self.reject_at=None;self.change_at_second=None;self.gd_reads=0
        self.mem[BASE-GD:BASE-GD+390884]=(ROOT/'bazel/pinned/firmware/bl33.bin').read_bytes()
        self.p64(GD+8,0x201);self.p64(GD+0x58,BASE);self.p64(GD+0x80,BASE-0x88110000);self.p64(GD+0x90,LOW+0x8000)
        self.p64(BASE+0x66cb0,LOW);self.p64(BASE+0x66ca8,BASE)
        self.list(GD+0xa0,[UC0+0x20,UC+0x20])
        self.p64(UC0+8,BASE+0x52470);self.p64(UC+8,BASE+0x523f0)
        self.list(UC+0x10,[BD0+0x60,BD+0x60])
        for dev,desc,num in [(BD0,DESC0,0),(BD,DESC,1)]:
            self.p64(dev,BASE+0x51c10);self.p64(dev+0x20,desc);self.p64(dev+0x38,PARENT)
            self.p64(dev+0x48,UC);self.p32(dev+0x90,1)
            self.p32(desc,6);self.p32(desc+4,num);self.p64(desc+0x80,dev)
        self.p64(PARENT,BASE+0x52048);self.p64(PARENT+0x10,PLAT);self.p64(PARENT+0x40,PRIVATE)
        self.p64(PARENT+0x50,UPRIVATE);self.p32(PARENT+0x90,1)
        self.p64(PRIVATE,0x700b0000);self.p64(UPRIVATE,MMC)
        self.p64(MMC,PLAT);self.p32(MMC+8,0x80030000);self.p64(MMC+0x10,PARENT)
        self.p32(MMC+0x18,1);self.p32(MMC+0x1c,1);self.p32(MMC+0x20,4);self.p32(MMC+0x24,48000000)
        self.p32(MMC+0x2c,0xc0ff8000);self.p32(MMC+0x40,0x40000000)
        self.mem[MMC+0x50-GD:MMC+0x60-GD]=bytes(range(1,17))
        self.p16(MMC+0x60,0xabcd);self.p32(MMC+0x6c,512);self.p32(MMC+0x70,512);self.p64(MMC+0xe0,PARENT)
        self.capacity(CAPACITY)
        self.callback=COPY(self.copy);self.reader=Reader(None,self.callback)

    def p16(self,p,v):struct.pack_into('<H',self.mem,p-GD,v)
    def p32(self,p,v):struct.pack_into('<I',self.mem,p-GD,v)
    def p64(self,p,v):struct.pack_into('<Q',self.mem,p-GD,v)
    def capacity(self,sectors):
        csize=(sectors>>10)-1
        self.p32(MMC+0x44,(9<<16)|(csize>>16));self.p32(MMC+0x48,(csize&0xffff)<<16)
        self.p64(MMC+0x88,sectors*512);self.p64(MMC+0x90,sectors*512)
        self.p64(DESC+0x10,sectors);self.p64(DESC+0x18,512);self.p32(DESC+0x20,9)
    def list(self,head,nodes):
        allnodes=[head,*nodes]
        for i,p in enumerate(allnodes):self.p64(p,allnodes[(i+1)%len(allnodes)]);self.p64(p+8,allnodes[i-1])
    def copy(self,ctx,p,out,n):
        self.accesses.append((p,n))
        if p==GD:
            self.gd_reads+=1
            if self.gd_reads==2 and self.change_at_second:self.change_at_second()
        if self.reject_at is not None and p<=self.reject_at<p+n:return 1
        if not GD<=p<=END-n:
            raise AssertionError('out of fixture read '+hex(p))
        ctypes.memmove(out,bytes(self.mem[p-GD:p-GD+n]),n);return 0
    def run_capture(self,expected=0,gd=GD):
        out=(ctypes.c_uint8*256)(*([0xa5]*256))
        result=self.lib.iosw_fw_capture(ctypes.byref(self.reader),gd,out)
        self.assertEqual(result,expected)
        result_bytes=bytes(out)
        if expected:self.assertEqual(result_bytes,bytes([0xa5])*256)
        return result_bytes
    def test_exact_success_record(self):
        out=self.run_capture();expected=bytearray(256)
        struct.pack_into('<8I2Q4I',expected,0,0x4d435349,1,256,1,0x700b0000,1,512,1,CAPACITY,CAPACITY*512,0xc0ff8000,0xabcd,0x80030000,0)
        expected[0x40:0x60]=self.mem[MMC+0x40-GD:MMC+0x60-GD]
        struct.pack_into('<4Q',expected,0x60,MMC,DESC,GD,BASE-0x88110000)
        expected[0x80:0xa0]=hashlib.sha256((ROOT/'bazel/pinned/firmware/bl33.bin').read_bytes()).digest()
        struct.pack_into('<I',expected,0xf0,zlib.crc32(expected[:0xf0]))
        self.assertEqual(out,expected);self.assertEqual(self.gd_reads,2)
        self.assertTrue(all(0x80000000<=p<0x100000000 and p+n<=0x100000000 for p,n in self.accesses))
    def test_capacity_edges(self):
        for cap in (1024,0x100000000):
            with self.subTest(cap=cap):self.capacity(cap);self.run_capture()
    def test_invalid_gd_never_dereferenced(self):
        for gd in (0,0x700b0000,0x7fffffff,GD+1,0x100000000,0xfffffff8):
            with self.subTest(gd=gd):self.accesses=[];self.run_capture(2,gd);self.assertFalse(self.accesses)
    def test_every_instruction_fingerprint(self):
        import re
        words=re.findall(r'\{0x([0-9a-f]{8})u,0x([0-9a-f]{8})u\}',(HERE/'signatures.h').read_text())
        for off,word in words:
            with self.subTest(offset=off):
                p=BASE+int(off,16);self.p32(p,int(word,16)^1);self.run_capture(3);self.p32(p,int(word,16))
    def test_bad_global_fields(self):
        for p,v in [(GD+8,1),(GD+0x90,0),(GD+0x58,BASE+8),(GD+0x80,0),(GD+0x58,0x70000000),(GD+0x58,0xffff0000)]:
            with self.subTest(p=p,v=v):
                old=self.mem[p-GD:p-GD+8];self.p64(p,v);self.run_capture(2);self.mem[p-GD:p-GD+8]=old
    def test_heap_mismatch(self):
        for p in (BASE+0x66cb0,BASE+0x66ca8):
            old=self.mem[p-GD:p-GD+8];self.p64(p,LOW+8);self.run_capture(4);self.mem[p-GD:p-GD+8]=old
    def test_class_faults(self):
        for p,v in [(GD+0xa0,0x700b0000),(UC+8,BASE+0x52470),(UC+0x28,0),(UC+0x20,UC+0x20),(GD+0xa8,UC0+0x20)]:
            with self.subTest(p=p,v=v):
                old=self.mem[p-GD:p-GD+8];self.p64(p,v);self.run_capture(5);self.mem[p-GD:p-GD+8]=old
    def test_device_faults(self):
        for p,v in [(UC+0x10,0x700b0000),(BD+0x20,0x700b0000),(BD+0x68,0),(BD+0x48,UC0),(BD,BASE+0x52048),(DESC+0x80,BD0),(UC+0x18,BD0+0x60)]:
            with self.subTest(p=p,v=v):
                old=self.mem[p-GD:p-GD+8];self.p64(p,v);self.run_capture(6);self.mem[p-GD:p-GD+8]=old
        self.p32(DESC0+4,1);self.run_capture(6)
    def test_card_links(self):
        for p,v in [(BD+0x38,0x700b0000),(PARENT,BASE+0x51c10),(PARENT+0x90,0),(PARENT+0x10,0x700b0000),(PARENT+0x40,0x700b0000),(PRIVATE,0x700b0600),(UPRIVATE,MMC+8),(MMC,0),(MMC+0x10,0),(MMC+0xe0,0)]:
            with self.subTest(p=p,v=v):
                old=self.mem[p-GD:p-GD+8];self.p64(p,v);self.run_capture(7);self.mem[p-GD:p-GD+8]=old
    def test_state_refusals(self):
        for p,v in [(MMC+0x18,0),(MMC+0x1c,0),(MMC+8,0x30000),(MMC+0x2c,0x80ff8000),(MMC+0x2c,0x40ff8000),(MMC+0x60,0),(MMC+0x40,0),(MMC+0x40,0x80000000),(MMC+0x20,8),(MMC+0x24,0),(MMC+0x6c,1024),(MMC+0x70,1024),(MMC+0xdc,1),(MMC+0xd8,1),(MMC+0xd8,256),(DESC+0x18,1024),(DESC+0x20,10)]:
            with self.subTest(p=p,v=v):
                old=self.mem[p-GD:p-GD+4];self.p32(p,v);self.run_capture(8);self.mem[p-GD:p-GD+4]=old
    def test_capacity_mismatches(self):
        for p,v in [(MMC+0x44,0),(MMC+0x88,0),(MMC+0x90,0),(DESC+0x10,CAPACITY-1)]:
            with self.subTest(p=p,v=v):
                old=self.mem[p-GD:p-GD+8];self.p64(p,v);self.run_capture(9);self.mem[p-GD:p-GD+8]=old
    def test_every_read_can_fail_without_output(self):
        self.run_capture();points=sorted(set(p for p,n in self.accesses))
        for p in points:
            with self.subTest(p=hex(p)):
                self.reject_at=p;out=(ctypes.c_uint8*256)(*([0xa5]*256))
                result=self.lib.iosw_fw_capture(ctypes.byref(self.reader),GD,out)
                self.assertNotEqual(result,0);self.assertEqual(bytes(out),bytes([0xa5])*256)
        self.reject_at=None
    def test_changed_valid_snapshot(self):
        self.change_at_second=lambda:self.p16(MMC+0x60,0xabce)
        self.run_capture(10)
    def test_changed_invalid_snapshot(self):
        self.change_at_second=lambda:self.p32(MMC+0x1c,0)
        self.run_capture(8)

if __name__=='__main__':unittest.main(verbosity=2)
