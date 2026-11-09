#!/usr/bin/env python3
"""Build and sign the iOS 18 Wi-Fi authentication service and launchd cache."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

DEVICE = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--component', required=True, type=Path)
p.add_argument('--os-root', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, a.component / 'tools/build-supplicant.py', '--target', 'ios'], check=True)
for directory in ('bin', 'etc/wifi', 'daemons', 'xpc'):
    (out / directory).mkdir(exist_ok=True, parents=True)
binary = out / 'bin/unixnu-wpa_supplicant'
shutil.copy2(a.component / 'out/wpa-ios/bin/wpa_supplicant', binary)
binary.chmod(0o755)
entitlements = out / 'entitlements.plist'
entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier',
                'org.unixnu.wifi.supplicant', '--entitlements', entitlements, binary], check=True)
subprocess.run(['codesign', '--verify', '--strict', binary], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', binary], check=True, capture_output=True, text=True).stderr
cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M).group(1)
(out / 'etc/wifi/wpa_supplicant.conf').write_text('ap_scan=1\n')
job = {'Label': 'org.unixnu.wifi.supplicant', 'ProgramArguments': [
    '/usr/libexec/unixnu-wpa_supplicant', '-Dsheng', '-ien0',
    '-c/private/var/ngpu/etc/wifi/wpa_supplicant.conf', '-q'],
    'RunAtLoad': True, 'KeepAlive': True, 'ThrottleInterval': 3, 'UserName': 'root',
    'Umask': 0o77, 'ProcessType': 'Interactive',
    'StandardOutPath': '/var/log/unixnu-wifi.log', 'StandardErrorPath': '/var/log/unixnu-wifi.log'}
(out / 'daemons/org.unixnu.wifi.supplicant.plist').write_bytes(plistlib.dumps(job))
script = DEVICE / 'ios18/source/scripts/build-boot-runtime.py'
spec = importlib.util.spec_from_file_location('wifi_boot_runtime', script)
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)
cache = out / 'xpc/launchd.plist'
report = runtime.merge_launchd_cache(a.os_root / 'System/Library/xpc/launchd.plist', out / 'daemons', cache)
report.update(runtime.sign_launchd_cache(cache))
directories = {name: name for name in ('bin', 'etc/wifi', 'daemons', 'xpc')}
files = {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest()
         for directory in directories.values() for path in sorted((out / directory).rglob('*')) if path.is_file()}
(out / 'report.json').write_text(json.dumps({'target': 'ios18', 'build': '22H384', 'directories': directories,
    'files': files, 'cdhashes': [cdhash, report['cdhash']], 'launchd_cache': report,
    'supplicant': json.loads((a.component / 'out/wpa-ios/report.json').read_text())}, indent=2) + '\n')
print(out)
