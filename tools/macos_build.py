#!/usr/bin/env python3
"""Build the native Apple Silicon AppKit host from the shared Apple guest."""
import subprocess
import shutil
import sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def run(*args):subprocess.run(list(map(str,args)),cwd=ROOT,check=True)
if __name__=='__main__':
 run(sys.executable,'tools/ios_build.py','--guest-only')
 run('cmake','-S','port/macos','-B','build/macos/app','-G','Xcode','-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0')
 run('cmake','--build','build/macos/app','--config','Release','--target','HaloCE','--','-quiet','CODE_SIGNING_ALLOWED=NO')
 app=ROOT/'build/macos/Halo: Combat Evolved.app'
 if app.exists():shutil.rmtree(app)
 run('ditto',ROOT/'build/macos/app/Release/HaloCE.app',app)
 print('App:',app)
