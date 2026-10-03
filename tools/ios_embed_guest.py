#!/usr/bin/env python3
"""Embed the guest's immutable code as signed Mach-O text, with separate data."""
import argparse
import struct
from pathlib import Path


def embed(source, destination):
    """Validate the ELF load ranges and emit assembly plus layout metadata."""
    data=source.read_bytes()
    if data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',data,18)[0]!=183:
        raise ValueError('Expected little-endian AArch64 ELF64')
    offset=struct.unpack_from('<Q',data,32)[0]
    stride,count=struct.unpack_from('<HH',data,54)
    segments=[]
    for i in range(count):
        kind,flags,start,address,_,size,memory,_=struct.unpack_from('<IIQQQQQQ',data,offset+i*stride)
        if kind!=1:continue
        if start+size>len(data) or size>memory or address+memory>0x100000000:
            raise ValueError('Invalid guest segment bounds')
        segments.append((flags,address,size,memory,data[start:start+size]))
    if len(segments)!=2 or [s[0] for s in segments]!=[5,6]:
        raise ValueError('Guest must contain exactly one RX and one RW load segment')
    code,writable=segments
    if code[1]!=0x88000000 or writable[1]%0x4000 or code[1]+code[3]>writable[1]:
        raise ValueError('Unexpected guest layout or page alignment')
    destination.mkdir(parents=True,exist_ok=True)
    code_bytes=code[4].ljust(writable[1]-code[1],b'\0')
    (destination/'guest-code.bin').write_bytes(code_bytes)
    (destination/'guest-data.bin').write_bytes(writable[4])
    (destination/'guest_image.h').write_text(
        '/* Generated guest image layout. */\n#pragma once\n'
        f'#define IOS_GUEST_CODE_SIZE {len(code_bytes)}u\n'
        f'#define IOS_GUEST_DATA_ADDRESS 0x{writable[1]:x}u\n'
        f'#define IOS_GUEST_DATA_SIZE {writable[2]}u\n'
        f'#define IOS_GUEST_IMAGE_END 0x{writable[1]+writable[3]:x}u\n'
        'extern const unsigned char halo_guest_code[], halo_guest_data[];\n')
    (destination/'guest_image.S').write_text(
        '/* Guest code is covered by the enclosing app code signature. */\n'
        '.section __TEXT,__halocode,regular,pure_instructions\n.p2align 14\n'
        '.globl _halo_guest_code\n_halo_guest_code:\n'
        f'.incbin "{(destination/"guest-code.bin").resolve()}"\n'
        '.section __DATA_CONST,__halodata\n.p2align 3\n'
        '.globl _halo_guest_data\n_halo_guest_data:\n'
        f'.incbin "{(destination/"guest-data.bin").resolve()}"\n')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();embed(args.elf,args.output)
