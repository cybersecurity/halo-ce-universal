#!/usr/bin/env python3
"""Inspect and extract Halo map files from a user's Xbox XISO without changing them."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def map_entries(image):
    """Read bounded XDVDFS directory trees and return map file extents."""
    length=image.seek(0,2)
    for base in (0,0xFD90000,0x2080000):
        image.seek(base+32*2048);header=image.read(2048)
        if header[:20]==b'MICROSOFT*XBOX*MEDIA':break
    else:raise ValueError('No Xbox filesystem header found')
    root,size=struct.unpack_from('<II',header,20)
    visited=set()
    def directory(sector,size,prefix):
        extent=base+sector*2048
        if extent+size>length or size>16*1024*1024 or sector in visited:
            raise ValueError('Invalid or cyclic directory extent')
        visited.add(sector);image.seek(extent);data=image.read(size);nodes=set()
        def node(offset):
            if offset in nodes or offset+14>len(data):raise ValueError('Invalid directory tree')
            nodes.add(offset)
            left,right,sector,size,attr,count=struct.unpack_from('<HHIIBB',data,offset)
            if offset+14+count>len(data):raise ValueError('Invalid filename length')
            name=data[offset+14:offset+14+count].decode('ascii')
            if not name or name in ('.','..') or any(c in name for c in '/\\\0'):raise ValueError('Invalid filename')
            if left:yield from node(left*4)
            path=prefix/name
            if attr&16:
                yield from directory(sector,size,path)
            elif path.parts[0].lower()=='maps' and path.suffix.lower()=='.map':
                start=base+sector*2048
                if start+size>length:raise ValueError('Map extends beyond image')
                yield path,start,size
            if right:yield from node(right*4)
        yield from node(0)
    return list(directory(root,size,Path()))


def main():
    """Report exact cache build IDs; optionally copy the maps unchanged."""
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image',type=Path)
    parser.add_argument('--output',type=Path,help='new output directory; existing files are never replaced')
    args=parser.parse_args();manifest=[]
    with args.image.open('rb') as image:
        for path,start,size in map_entries(image):
            image.seek(start);header=image.read(2048)
            if header[:4]!=b'daeh' or header[-4:]!=b'toof':raise ValueError(f'Invalid Halo cache header: {path}')
            build=header[64:96].split(b'\0',1)[0].decode('ascii')
            entry={'path':str(path),'bytes':size,'build':build,'version':struct.unpack_from('<I',header,4)[0]}
            print(f'{path}: {build}, {size:,} bytes',flush=True)
            if args.output:
                target=args.output/path;target.parent.mkdir(parents=True,exist_ok=True)
                digest=hashlib.sha256();image.seek(start);remaining=size
                with target.open('xb') as destination:
                    while remaining:
                        data=image.read(min(4*1024*1024,remaining))
                        if not data:raise ValueError('Truncated image')
                        destination.write(data);digest.update(data);remaining-=len(data)
                entry['sha256']=digest.hexdigest()
            manifest.append(entry)
    if args.output:
        (args.output/'asset-manifest.json').write_text(json.dumps({'source':args.image.name,'maps':manifest},indent=2)+'\n')
    if any(m['build'] not in ('01.01.14.2342','01.10.12.2276') for m in manifest):
        print('Unsupported map build: the iOS port accepts PAL and NTSC-US only. No headers were changed.')

if __name__=='__main__':main()
