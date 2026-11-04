#!/usr/bin/env python3
"""Check ADSP transport and marble battery protocol on the host."""
from pathlib import Path
import subprocess

board = Path(__file__).resolve().parents[1]
source = board / 'package'
out = board.parent / 'UniXNU/build/marble-charging/tests'
out.mkdir(parents=True, exist_ok=True)
for name, modules in (
    ('transport', ['QcomSmem', 'QcomSmp2p', 'QcomGlink', 'QcomQmp', 'QcomRpmh', 'QcomMdt']),
    ('qrtr', ['QcomQrtr']),
    ('battery', ['MarbleBattmgr']),
):
    executable = out / name
    subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                    str(source / 'tests' / ('charging_' + name + '.c')),
                    *[str(source / (module + '.c')) for module in modules], '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
