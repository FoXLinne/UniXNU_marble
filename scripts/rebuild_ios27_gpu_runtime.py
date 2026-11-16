#!/usr/bin/env python3
"""Refresh the signed phone GPU payload while retaining pinned runtime inputs."""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

from assemble_ios27_runtime import unpack_addon, pack_addon
import build_ios27_runtime as runtime
import build_ios27_gpu_userspace as gpu

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', required=True, type=Path)
parser.add_argument('--base-runtime', required=True, type=Path)
parser.add_argument('--base-payload', required=True, type=Path)
parser.add_argument('--helper-payload', required=True, type=Path)
args = parser.parse_args()
root = args.root.resolve()
output = root / 'runtime-pooled-image'
output.mkdir(exist_ok=True)
for directory in ('wifi-runtime', 'haptics', 'appsync'):
    shutil.copytree(args.base_runtime / directory, output / directory, dirs_exist_ok=True)
report = json.loads((args.base_runtime / 'runtime.json').read_text())
profile = json.loads((runtime.PROFILE / 'target.json').read_text())
fixups = json.loads((runtime.PROFILE / 'fixups.json').read_text())
report['steps']['validate_os'] = runtime.validate_os_root(Path('/private/tmp/marble-24b119-os'), profile)
verified = runtime.validate_fixups(Path('/private/tmp/marble-24b119-cryptex'), fixups)
report['steps']['fixups'] = dict(status='verified', count=len(verified), verified=verified)
gpu.addon(root)
files = unpack_addon(root / 'gpu/metal.gpuaddon')
helper = 'addons/NativeAdrenoMetal.bundle/MarbleExecutablePath.dylib'
files[helper] = unpack_addon(args.helper_payload / 'runtime.gpuaddon')[helper]
input_addon = output / 'gpu-with-path-adapter.gpuaddon'
pack_addon(files, input_addon)
report['steps']['phone_ui'] = runtime.build_phone_ui(input_addon, output / 'phone-ui')
report['steps']['wallpaper'] = runtime.merge_wallpaper(output / 'phone-ui/phone-ui.gpuaddon',
    Path('/private/tmp/marble-24b119-os'), output / 'wallpaper-cache', profile['wallpaper'])
report['trust_manifest'] = runtime.trust_manifest(output)
report['input_hashes'] = {
    'fixups.json': runtime.sha256(runtime.PROFILE / 'fixups.json'),
    'target.json': runtime.sha256(runtime.PROFILE / 'target.json'),
    'MarbleUiPolicy27.c': runtime.sha256(runtime.REPO / 'package/ios27-runtime/MarbleUiPolicy27.c'),
    'MarbleCrashInfo27.c': runtime.sha256(runtime.REPO / 'package/ios27-runtime/MarbleCrashInfo27.c'),
}
(output / 'runtime.json').write_text(json.dumps(report, indent=2) + '\n')
firmware = output / 'wifi-firmware'
firmware.mkdir(exist_ok=True)
for name, (mode, data) in unpack_addon(args.base_payload / 'runtime.gpuaddon').items():
    prefix = 'firmware/ath11k/WCN6855/hw2.1/'
    if name.startswith(prefix):
        (firmware / name[len(prefix):]).write_bytes(data)
subprocess.run([sys.executable, str(Path(__file__).with_name('assemble_ios27_runtime.py')),
    '--root', str(root), '--runtime', str(output), '--output', str(root / 'runtime-payload-pooled-image'),
    '--wifi-firmware', str(firmware), '--debug-agent', str(root / 'debug-agent')], check=True)
print('GPU runtime assembled', root / 'runtime-payload-pooled-image/report.json', flush=True)
