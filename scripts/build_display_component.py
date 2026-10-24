#!/usr/bin/env python3
"""Compile owned IOKit components using the current kernel recipe."""
import argparse
import json
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('component',choices=['NativeLegacyDisplay','NativeGOPPresenter','NativeKeyStoreEvents'])
component=parser.parse_args().component
record=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
source=ROOT/'drivers'/('NativeKeyStoreEvents' if component=='NativeKeyStoreEvents' else 'NativeMobileDisplay')/(component+'.cpp')
out=ROOT/'build'/{'NativeLegacyDisplay':'native-legacy-display','NativeGOPPresenter':'native-gop-presenter','NativeKeyStoreEvents':'native-keystore-events'}[component]
out.mkdir(parents=True,exist_ok=True)
destination=out/(component+'.o')
arguments = record['arguments']
command = []
index = 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command.extend(['-o', str(destination)])
        index += 1
    elif value in ('-MF', '-MJ', '-MT'):
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier', '-I', str(ROOT / '.cache/xnu-work/osfmk')])
command+=['-I',str(out)]
result=subprocess.run(command,cwd=record['directory'],capture_output=True,text=True)
(out/'compile.log').write_text(result.stdout+result.stderr)
if result.returncode:raise SystemExit((result.stdout+result.stderr)[-3500:])
