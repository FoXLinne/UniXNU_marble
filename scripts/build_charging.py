#!/usr/bin/env python3
"""Build marble ADSP transport and battery service."""
import argparse
import json
from pathlib import Path
import subprocess

board = Path(__file__).resolve().parents[1]
stack = board.parent / 'UniXNU'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--firmware', type=Path, default=stack / '.cache/firmware/marble/adsp2.mbn')
args = parser.parse_args()
out = stack / 'build/marble-charging'
out.mkdir(parents=True, exist_ok=True)
record = json.loads((stack / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
sources = ['QcomSmem.c', 'QcomSmp2p.c', 'QcomGlink.c', 'QcomQmp.c', 'QcomRpmh.c', 'QcomMdt.c', 'QcomQrtr.c', 'MarbleQrtr.c', 'MarbleBattmgr.c', 'MarbleAdsp.cpp', 'MarbleBattery.cpp', 'MarbleBatteryProbe.cpp', 'MarbleCharging.cpp', 'MarbleUiPolicy.c']
objects = []
for name in sources:
    source = board / 'package' / name
    obj = out / (source.stem + '.o')
    active = record
    if name == 'MarbleUiPolicy.c':
        active = json.loads((stack / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/security/DEVELOPMENT/mac_base.o.json').read_text().strip().rstrip(','))
    iterator = iter(active['arguments'])
    command = []
    for arg in iterator:
        if arg == active['file']:
            command.append(str(source))
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            next(iterator)
            if arg == '-o':
                command += ['-o', str(obj)]
        elif source.suffix == '.c' and arg in ('-fsized-deallocation', '-fapple-kext', '-fc++-static-destructors=none'):
            continue
        elif source.suffix == '.c' and arg == '-xc++':
            command.append('-xc')
        elif source.suffix == '.c' and arg == '--driver-mode=g++':
            command.append('--driver-mode=gcc')
        elif source.suffix == '.c' and arg.startswith('-std='):
            command.append('-std=gnu11')
        else:
            command.append(arg)
    command += ['-I', str(stack / '.cache/xnu-work/osfmk')]
    result = subprocess.run(command, cwd=active['directory'], text=True, capture_output=True)
    (out / (source.stem + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(result.stderr[-6000:])
    if name == 'MarbleBattery.cpp':
        late = out / 'MarbleBattery.late.o'
        subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_MarbleBattery.cpp', str(obj), str(late)], check=True)
        obj = late
    objects.append(str(obj))
firmware = args.firmware.resolve()
assembly = out / 'firmware.S'
assembly.write_text('.section __TEXT,__const\n.p2align 12\n.globl _marble_adsp_firmware\n_marble_adsp_firmware:\n.incbin "' + str(firmware) + '"\n.p2align 3\n.globl _marble_adsp_firmware_size\n_marble_adsp_firmware_size:\n.quad ' + str(firmware.stat().st_size) + '\n')
obj = out / 'firmware.o'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-c', str(assembly), '-o', str(obj)], check=True)
objects.append(str(obj))
archive = stack / 'build/native-root-block/libNativeRootBlock.a'
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *objects], check=True)
print(archive)
