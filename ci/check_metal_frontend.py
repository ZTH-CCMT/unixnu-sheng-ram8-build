#!/usr/bin/env python3
"""Compile the actual Metal frontend before any IPSW/Rust/kernel build."""
import argparse, pathlib, subprocess, sys
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--top',type=pathlib.Path,required=True)
a=ap.parse_args(); top=a.top.resolve()
source=top/'UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm'
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
cmd=['xcrun','--sdk','iphoneos','clang++','-target','arm64e-apple-ios27.0','-isysroot',sdk,
     '-std=c++17','-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
     '-fsyntax-only','-ferror-limit=0','-I'+str(top/'UniXNU/drivers/Adreno740'),str(source)]
print('Checking actual Objective-C++ frontend with iPhoneOS SDK:',sdk,flush=True)
log=top/'metal-frontend-check.log'
with log.open('w',encoding='utf-8') as out:
    process=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors='replace')
    for line in process.stdout:
        print(line,end='',flush=True);out.write(line)
    result=process.wait()
print('Metal frontend syntax result:',result,flush=True)
sys.exit(result)
