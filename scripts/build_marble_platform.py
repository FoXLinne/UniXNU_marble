#!/usr/bin/env python3
"""Build the marble platform archives using the XNU compile record."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))

for component, directory in [('ArmDTPlatform', 'armdt-platform'), ('ArmCpu', 'armcpu')]:
    out = ROOT / 'build' / directory
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'drivers' / component / (component + '.cpp')
    obj = out / (component + '.o')
    command = []
    arguments = iter(record['arguments'])
    for argument in arguments:
        if argument == record['file']:
            command.append(str(source))
        elif argument in ('-o', '-MF', '-MT', '-MJ'):
            next(arguments)
            if argument == '-o':
                command.extend(['-o', str(obj)])
        else:
            command.append(argument)
    command.extend(['-I', str(ROOT / '.cache/xnu-work/osfmk')])
    subprocess.run(command, cwd=record['directory'], check=True)
    archive = out / ('lib' + component + '.a')
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True)
