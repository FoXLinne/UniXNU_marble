#!/usr/bin/env python3
"""Build the signed read-only diagnostic service for marble 24B119."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess
import secrets

from ios27_migration import DEVICE, digest, save

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
output = args.output.resolve()
for name in ('bin', 'daemons'):
    (output / name).mkdir(parents=True, exist_ok=True)
source = DEVICE / 'package/debug27/MarbleDebug.c'
unified = DEVICE / 'package/debug27/MarbleUnifiedLog.m'
registration = DEVICE / 'package/debug27/MarbleWebKitRegistration.m'
system_content = DEVICE / 'package/ios27-runtime/userspace/MarbleSystemContent27.m'
auth = output / 'auth.key'
if not auth.exists():
    auth.write_text(secrets.token_hex(32))
auth.chmod(0o600)
token = auth.read_text().strip()
assert re.fullmatch('[0-9a-f]{64}', token)
source_id = hashlib.sha256(source.read_bytes() + b'\0' + unified.read_bytes() + b'\0' + (unified.parent / 'ActivityStreamSPI.h').read_bytes() + b'\0' + registration.read_bytes() + b'\0' + system_content.read_bytes()).hexdigest()
header = output / 'auth.generated.h'
header.write_text('#define MARBLE_DEBUG_AUTH "' + token + '"\n')
header.chmod(0o600)
binary = output / 'bin/marble-debug'
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
subprocess.run(['xcrun', 'clang', '-target', 'arm64e-apple-ios27.2', '-isysroot', sdk, '-O2', '-g0',
                '-Wall', '-Wextra', '-Werror', '-DMARBLE_DEBUG_SOURCE="' + source_id[:16] + '"',
                '-include', str(header), '-x', 'objective-c', str(source), str(unified), str(registration), str(system_content), '-fobjc-arc',
                '-fblocks', '-framework', 'Foundation', '-framework', 'OSLog', '-o', str(binary)], check=True)
entitlements = output / 'entitlements.plist'
entitlements.write_bytes(plistlib.dumps({'platform-application': True,
                                         'com.apple.private.security.no-container': True,
                                         'com.apple.private.logging.diagnostic': True,
                                         'com.apple.private.logging.kernel-info': True,
                                         'com.apple.usermanagerd.persona.fetch': True,
                                         'com.apple.private.MobileContainerManager.allowed': True,
                                         'com.apple.private.coreservices.can-register-install-results': True,
                                         'com.apple.private.coreservices.can-perform-rebuild-registration': True,
                                         'com.apple.private.mobileinstall.allowedSPI': ['CopyFrameworkInfoForLaunchServices', 'EnumerateInstalledAppsForLaunchServices', 'CopyInstalledAppsForLaunchServices']}))
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier',
                'org.unixnu.marble.debug', '--entitlements', str(entitlements), str(binary)], check=True)
subprocess.run(['codesign', '--verify', '--strict', str(binary)], check=True)
details = subprocess.check_output(['codesign', '-d', '--verbose=4', str(binary)], stderr=subprocess.STDOUT, text=True)
cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', details, re.M)[1]
job = {'Label': 'org.unixnu.marble.debug', 'ProgramArguments': ['/usr/libexec/marble-debug'],
       'RunAtLoad': True, 'KeepAlive': True, 'ThrottleInterval': 10, 'UserName': 'root',
       'ProcessType': 'Background', 'StandardOutPath': '/var/log/marble-debug.log',
       'StandardErrorPath': '/var/log/marble-debug.log'}
(output / 'daemons/org.unixnu.marble.debug.plist').write_bytes(plistlib.dumps(job))
registration_job = {'Label': 'org.unixnu.marble.register-webkit', 'ProgramArguments': ['/usr/libexec/marble-debug', '--register-webkit'], 'RunAtLoad': True, 'KeepAlive': False, 'UserName': 'mobile', 'ProcessType': 'Background'}
(output / 'daemons/org.unixnu.marble.register-webkit.plist').write_bytes(plistlib.dumps(registration_job))
save(output / 'report.json', {'build': '24B119', 'target': 'arm64e-apple-ios27.2',
                             'source_sha256': source_id, 'binary_sha256': digest(binary),
                             'unified_source_sha256': digest(unified),
                             'webkit_registration_source_sha256': digest(registration),
                             'system_content_source_sha256': digest(system_content),
                             'cdhash': cdhash, 'transport': 'authenticated read-only TCP 23456; existing loopback routes retained',
                             'commands': ['GET /status', 'GET /kernel.log', 'GET /unified.log (authenticated)', 'GET /webkit-status.json (authenticated)', 'CLI --register-webkit (one boot initialization, fixed stock paths)'],
                             'network_auth_file': str(auth), 'device_result': 'pending'})
print(output / 'report.json')
