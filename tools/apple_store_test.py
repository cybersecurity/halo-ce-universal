#!/usr/bin/env python3
"""Exercise the private image transaction with synthetic Xbox images."""
import hashlib
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
from ios_xiso_test import fixture, ROOT
PROBE=ROOT/'build/apple-store-probe'
subprocess.run(['xcrun','clang','-fobjc-arc','-Wno-deprecated-declarations','-Wno-incompatible-pointer-types','-Iport/apple','-Iport/runtime','port/apple/game_store_probe.m','port/apple/game_store.m','port/runtime/xiso.c','-framework','Foundation','-o',str(PROBE)],cwd=ROOT,check=True)
class StoreTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name);self.store=self.root/'private';self.image=self.root/'game.xiso';fixture(self.image)
 def tearDown(self):self.temp.cleanup()
 def run_probe(self,mode,success=True):
  args=[str(PROBE),mode,str(self.store)]+([] if mode=='ready' else [str(self.image)])
  p=subprocess.run(args,capture_output=True,text=True,timeout=20);self.assertEqual(p.returncode,0 if success else 1,p.stderr)
 def generation(self):return self.store/plistlib.loads((self.store/'game-store.plist').read_bytes())['generation']
 def test_retained_verified_copy_and_restart(self):
  original=self.image.read_bytes();self.run_probe('import');generation=self.generation();self.assertEqual((generation/'disc.iso').read_bytes(),original)
  manifest=plistlib.loads((generation/'integrity.plist').read_bytes());self.assertEqual(manifest['sha256'],hashlib.sha256(original).hexdigest())
  self.image.unlink();self.run_probe('ready')
 def test_cancel_preserves_existing_generation(self):
  self.run_probe('import');before=self.generation();self.run_probe('cancel',False);self.assertEqual(self.generation(),before);self.assertEqual(len(list(self.store.glob('game-*'))),2) # generation plus pointer
  self.run_probe('ready')
 def test_corrupt_image_does_not_replace_working_game(self):
  self.run_probe('import');before=self.generation();self.image.write_bytes(b'not an xbox image');self.run_probe('import',False);self.assertEqual(self.generation(),before);self.run_probe('ready')
 def test_truncated_private_copy_is_rejected(self):
  self.run_probe('import');(self.generation()/'disc.iso').write_bytes(b'broken');self.run_probe('ready',False)
 def test_recovers_only_owned_unpublished_imports(self):
  self.run_probe('import');before=self.generation()
  import uuid
  abandoned=self.store/('game-'+str(uuid.uuid4()));abandoned.mkdir();(abandoned/'import.pending').write_text('Halo private image transaction v1')
  unowned=self.store/('game-'+str(uuid.uuid4()));unowned.mkdir()
  self.run_probe('ready');self.assertFalse(abandoned.exists());self.assertTrue(unowned.exists());self.assertEqual(before,self.generation())
 def test_pointer_cannot_escape_storage(self):
  self.store.mkdir();(self.store/'game-store.plist').write_bytes(plistlib.dumps({'generation':'../../outside'}));self.run_probe('ready',False)
if __name__=='__main__':unittest.main()
