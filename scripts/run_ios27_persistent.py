#!/usr/bin/env python3
"""Capture one marble boot in a unique evidence directory."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--candidate', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
args = p.parse_args()
root = args.output.resolve()
session = root / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
session.mkdir(parents=True)
command = [sys.executable, str(Path(__file__).with_name('run_ios27_phone.py')),
           '--candidate', str(args.candidate.resolve()), '--output', str(session)]
with (session / 'observer.log').open('w') as out:
    process = subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT)
    (root / 'active.json').write_text(json.dumps({'pid': process.pid, 'session': str(session),
        'candidate': str(args.candidate.resolve())}, indent=2) + '\n')
    code = process.wait()
with (root / 'sessions.jsonl').open('a') as out:
    out.write(json.dumps({'session': str(session), 'returncode': code}) + '\n')
sys.exit(code)
