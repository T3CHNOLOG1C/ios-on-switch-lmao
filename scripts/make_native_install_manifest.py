#!/usr/bin/env python3
"""Describe an already prepared APFS image, split into ordered FAT files."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import struct

def geometry(first, total):
    if len(first)<4096 or first[32:36]!=b'NXSB':raise ValueError('first part is not a raw APFS container')
    block,count=struct.unpack_from('<IQ',first,36)
    if block<4096 or block>65536 or block&(block-1) or len(first)<block or count*block!=total or total//512>0xffffffff:
        raise ValueError('NXSB geometry must exactly match the concatenated image')
    words=struct.unpack('<'+'I'*((block-8)//4),first[8:block]);a=b=0;mod=0xffffffff
    for word in words:a+=word;b+=a
    a%=mod;b%=mod;x=mod-(a+b)%mod;y=mod-(a+x)%mod
    if struct.unpack_from('<Q',first)[0] != x|(y<<32):raise ValueError('NXSB checksum mismatch')
    return block

def fat_path(card):
    encoded=card.encode('ascii');path=PurePosixPath(card)
    if (not encoded or len(encoded)>=240 or card.startswith('/') or ':' in card or '\\' in card or
        any(ord(c)<32 or ord(c)>126 for c in card) or
        any(p in ('','.','..') for p in card.split('/')) or str(path)!=card):
        raise ValueError('part names must be canonical FAT-relative ASCII paths')
    return encoded

def encode(parts, total, block, digest):
    """Serialize already streamed parts without reading or hashing them again."""
    if (not 1<=len(parts)<=128 or len(digest)!=32 or not any(digest) or
        block<4096 or block>65536 or block&(block-1)):
        raise ValueError('invalid manifest parts, digest or block size')
    records=[];names=set();summed=0
    for card,size in parts:
        encoded=fat_path(card)
        if card.casefold() in names:
            raise ValueError('part names must be unique canonical FAT-relative ASCII paths')
        names.add(card.casefold())
        if not size or size%512 or size>0xfffffe00:raise ValueError('invalid FAT part size')
        summed+=size;records.append(encoded.ljust(240,b'\0')+struct.pack('<Q',size)+bytes(8))
    if summed!=total or parts[0][1]<block or total//512>0xffffffff or not total or total%block:
        raise ValueError('manifest image or part geometry differs')
    return struct.pack('<8sIIQII32s',b'IOSWIMG1',1,len(records),total,block,0,digest)+b''.join(records)

def build(parts, output):
    if not 1 <= len(parts) <= 128:
        raise ValueError('1..128 ordered parts required')
    digest=hashlib.sha256(); records=[]; total=0; first=None;names=set()
    for pair in parts:
        card,host=pair.split('=',1)
        fat_path(card)
        if card.casefold() in names:raise ValueError('part names must be unique')
        names.add(card.casefold())
        source=Path(host); size=source.stat().st_size
        if not size or size%512 or size>0xfffffe00:
            raise ValueError('each FAT part must be positive, 512-aligned and at most 4 GiB - 512 bytes')
        with source.open('rb') as stream:
            if first is None:
                first=stream.read(65536);stream.seek(0)
            while chunk:=stream.read(1024*1024):digest.update(chunk)
        if source.stat().st_size!=size:raise ValueError('part changed while hashing')
        total+=size; records.append((card,size))
    block=geometry(first,total)
    data=encode(records,total,block,digest.digest())
    output=Path(output)
    with output.open('xb') as stream:stream.write(data)
    return {'image_bytes':total,'parts':len(records),'sha256':digest.hexdigest(),'manifest':str(output)}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True,help='new .IWI file; copy to IOSWITCH/INSTALL on source FAT')
    parser.add_argument('parts',nargs='+',help='ordered CARD/RELATIVE/PATH=host-file')
    args=parser.parse_args();print(build(args.parts,args.output))
