import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('xbox_assets',Path(__file__).parents[1]/'scripts/xbox_assets.py')
x=importlib.util.module_from_spec(spec);spec.loader.exec_module(x)

def entry(name,sector,size,attrs=0,left=0,right=0):
    name=name.encode();return struct.pack('<HHIIBB',left,right,sector,size,attrs,len(name))+name

def fixture(path,base=0,build=b'01.10.12.2276'):
    def table(names,first):
        data=bytearray(32*len(names))
        for index,name in enumerate(names):
            node=entry(name,first+index,2048 if name.endswith('.map') else 8,right=(index+1)*8 if index+1<len(names) else 0)
            data[index*32:index*32+len(node)]=node
        return data
    maps=table(sorted(n+'.map' for n in x.REQUIRED_MAPS),36)
    movies=table(sorted(n+'.bik' for n in x.REQUIRED_MOVIES),60)
    with path.open('wb') as f:
        f.truncate(base+80*x.SECTOR)
        descriptor=bytearray(x.SECTOR);descriptor[:20]=x.MAGIC;descriptor[-20:]=x.MAGIC
        struct.pack_into('<II',descriptor,20,33,64);f.seek(base+32*x.SECTOR);f.write(descriptor)
        root=bytearray(64);root[:18]=entry('maps',34,len(maps),0x10,right=8);root[32:50]=entry('bink',35,len(movies),0x10)
        f.seek(base+33*x.SECTOR);f.write(root)
        f.seek(base+34*x.SECTOR);f.write(maps)
        f.seek(base+35*x.SECTOR);f.write(movies)
        header=bytearray(2048);header[:4]=b'daeh';struct.pack_into('<I',header,4,5);header[64:64+len(build)]=build
        for sec in range(36,60):f.seek(base+sec*x.SECTOR);f.write(header)
        for sec in range(60,65):f.seek(base+sec*x.SECTOR);f.write(b'BIKi1234')

class ReaderTests(unittest.TestCase):
    def test_xiso_and_redump_extract(self):
        for base in (0,0x18300000):
            with self.subTest(base=base), tempfile.TemporaryDirectory() as d:
                p=Path(d)/'disc.iso';fixture(p,base)
                got=x.extract(p,Path(d)/'data')
                self.assertEqual(got['partition_offset'],base)
                self.assertEqual(len(got['assets']),29)
                self.assertEqual((Path(d)/'data/maps/ui.map').stat().st_size,2048)
    def test_pal_signature_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'disc.iso';fixture(p,build=b'01.01.14.2342');i=x.Image(p);_,files=x.inventory(i)
            self.assertEqual(x.selected_assets(i,files)[1],['01.01.14.2342'])
    def test_pc_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'disc.iso';p.write_bytes(b'\0'*(34*x.SECTOR))
            with p.open('r+b') as f:f.seek(16*x.SECTOR+1);f.write(b'CD001')
            with self.assertRaisesRegex(x.ImageError,'PC/Mac'):x.inventory(x.Image(p))
    def test_unsafe_name(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'disc.iso';fixture(p)
            with p.open('r+b') as f:f.seek(33*x.SECTOR+14);f.write(b'../x')
            with self.assertRaisesRegex(x.ImageError,'Unsafe'):x.inventory(x.Image(p))
    def test_cycle(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'disc.iso';fixture(p)
            with p.open('r+b') as f:f.seek(33*x.SECTOR+32);f.write(struct.pack('<H',8))
            with self.assertRaisesRegex(x.ImageError,'Cyclic'):x.inventory(x.Image(p))
    def test_truncation(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'disc.iso';fixture(p)
            with p.open('r+b') as f:f.truncate(35*x.SECTOR)
            with self.assertRaises(x.ImageError):x.inventory(x.Image(p))
    def test_wrong_cache_build(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'disc.iso';fixture(p,build=b'wrong-game');i=x.Image(p);_,files=x.inventory(i)
            with self.assertRaisesRegex(x.ImageError,'Unsupported Halo'):x.selected_assets(i,files)

if __name__=='__main__':unittest.main()
