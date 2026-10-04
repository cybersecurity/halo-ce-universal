#!/usr/bin/env python3
"""Stage, sign and package the asset-free Mac app. Notarization is separate."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
def run(*args):subprocess.run(list(map(str,args)),check=True)
def validate(app):
 for path in app.rglob('*'):
  if path.is_symlink() and not path.resolve().is_relative_to(app.resolve()):raise ValueError(f'Bundle link escapes app: {path}')
  if path.suffix.lower() in {'.iso','.xiso','.map','.p12','.p8','.xbe'} or path.name in {'disc.iso','game-store.plist','integrity.plist','save','maps','config.toml','ios-runtime.log'}:raise ValueError(f'Private/game data in app: {path}')
if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--identity',required=True);parser.add_argument('--output',type=Path,default=ROOT/'build/macos/HaloCE-AppleSilicon.dmg');args=parser.parse_args()
 app=ROOT/'build/macos/app/Release/HaloCE.app';validate(app)
 args.output=args.output.resolve();args.output.parent.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='halo-dmg-') as temporary:
  stage=Path(temporary);copy=stage/'Halo: Combat Evolved.app';run('ditto',app,copy)
  # The ELF guest is embedded in signed Mach-O text, so no JIT entitlement is required.
  for framework in sorted((copy/'Contents/Frameworks').glob('*.framework')):
   run('codesign','--force','--options','runtime','--timestamp','--sign',args.identity,framework)
  run('codesign','--force','--options','runtime','--timestamp','--entitlements',ROOT/'port/macos/Entitlements.plist','--sign',args.identity,copy)
  run('codesign','--verify','--deep','--strict',copy)
  (stage/'Applications').symlink_to('/Applications',target_is_directory=True)
  run('ditto',ROOT/'port/apple/DISTRIBUTION.md',stage/'Distribution notes.md')
  run('hdiutil','create','-ov','-format','UDZO','-volname','Halo Combat Evolved','-srcfolder',stage,args.output)
 run('codesign','--force','--timestamp','--sign',args.identity,args.output)
 digest=hashlib.sha256(args.output.read_bytes()).hexdigest();args.output.with_suffix('.dmg.sha256').write_text(f'{digest}  {args.output.name}\n')
 print(f'DMG: {args.output}\nSigned; not yet notarized.')
