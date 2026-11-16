#!/usr/bin/env python3
"""Build the 24B119 read-only SystemOS volume adapter for LaunchServices."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
source = repo / 'package/ios27-runtime/userspace/MarbleSystemContent27.m'
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
binary = output / 'MarbleSystemContent.dylib'
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
subprocess.run(['xcrun', 'clang', '-target', 'arm64e-apple-ios27.2', '-isysroot', sdk,
                '-O2', '-g0', '-Wall', '-Wextra', '-Werror', '-fobjc-arc', '-fblocks',
                '-dynamiclib', str(source), '-framework', 'Foundation', '-install_name',
                '/System/Library/Extensions/NativeAdrenoMetal.bundle/MarbleSystemContent.dylib',
                '-o', str(binary)], check=True)
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier',
                'org.unixnu.marble.system-content', str(binary)], check=True)
subprocess.run(['codesign', '--verify', '--strict', str(binary)], check=True)
details = subprocess.check_output(['codesign', '-d', '--verbose=4', str(binary)],
                                  stderr=subprocess.STDOUT, text=True)
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
(output / 'report.json').write_text(json.dumps({
    'source': str(source), 'source_sha256': digest(source),
    'binary': str(binary), 'binary_sha256': digest(binary),
    'cdhash': re.search(r'^CDHash=([0-9a-f]{40})$', details, re.M)[1],
    'device_result': 'pending',
    'scope': 'lsd native secured-device cache; authenticated read-only SystemOS views; class methods restored',
}, indent=2) + '\n')
print(output / 'report.json')
