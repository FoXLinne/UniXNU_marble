#!/usr/bin/env python3
"""Exercise UFS commands, partition selection and durable Data overlays."""
from pathlib import Path
import os
import subprocess

ROOT=Path(__file__).resolve().parents[1]
STACK=ROOT.parent/'UniXNU'
OUT=ROOT.parents[1]/'cache/persistent_boot/tests/storage'
OUT.mkdir(parents=True,exist_ok=True)
openssl=Path('/opt/homebrew/include')
cases=[
    ('RootLayout',STACK/'drivers/NativeUfs/tests/RootLayout.cpp',[]),
    ('MarbleLayout',STACK/'drivers/NativeUfs/tests/RootLayout.cpp',['-DNATIVE_UFS_ROOT_PARTITION="unixnu"']),
    ('RootCommands',STACK/'drivers/NativeUfs/tests/RootCommands.cpp',[]),
    ('SmmuTakeover',STACK/'drivers/NativeUfs/tests/SmmuTakeover.cpp',[]),
    ('DataPersistence',STACK/'drivers/NativeRamBlock/Test/persist.cpp',[]),
]
for name,source,flags in cases:
    binary=OUT/name
    command=['clang++','-std=c++17','-fsanitize=address,undefined','-g',str(source),'-o',str(binary),*flags]
    if name.endswith('Layout'):
        command+=['-I',str(openssl),'-L/opt/homebrew/lib','-lcrypto']
    subprocess.run(command,check=True,timeout=60)
    with (OUT/(name+'.log')).open('w') as log:
        subprocess.run([str(binary)],check=True,timeout=30,stdout=log,stderr=subprocess.STDOUT)
    print(name,'passed',flush=True)
