#!/usr/bin/env python3
"""Build the signed 22H384 Actuator HAL and its board tuning overlay."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile
import sys
import importlib.util

DEVICE = Path(__file__).resolve().parents[1]

def build(os_root, output, diagnostic_probe=False, wifi_runtime=None, virtual_actuator=False, speaker_transport='builtin', routing_trace=False):
    output.mkdir(parents=True, exist_ok=True)
    ios = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    mac = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    hal = output / 'hal'
    hal.mkdir(exist_ok=True)
    # The directory overlay supplies a complete plug-in listing to CoreAudio.
    # Apple's bundles retain their contents and signatures.
    original_hal = os_root / 'System/Library/Audio/Plug-Ins/HAL'
    for path in original_hal.iterdir():
        if path.is_dir(): shutil.copytree(path, hal / path.name, dirs_exist_ok=True, symlinks=True)
    bundle = hal / 'MarbleHaptics.driver'
    bundle.mkdir(exist_ok=True)
    binary = bundle / 'MarbleHaptics'
    with tempfile.TemporaryDirectory() as temporary:
        headers = Path(temporary) / 'CoreAudio'
        headers.mkdir()
        for name in ('AudioServerPlugIn.h', 'AudioHardware.h', 'AudioHardwareBase.h'):
            (headers / name).symlink_to(mac / 'System/Library/Frameworks/CoreAudio.framework/Headers' / name)
        subprocess.run(['xcrun', 'clang', '-target', 'arm64e-apple-ios18.0', '-isysroot', ios,
            '-I', temporary, '-I', str(DEVICE / 'package'), '-O2', '-g0', '-Wall', '-Wextra', '-Werror',
            '-bundle', '-fvisibility=hidden', *(['-DMARBLE_VIRTUAL_ACTUATOR=1'] if virtual_actuator else []),
            *(['-DMARBLE_AUDIO_ROUTE_TRACE=1'] if routing_trace else []),
            *(['-DMARBLE_HAPTIC_SERVER_DIAGNOSTICS=1', '-lobjc'] if diagnostic_probe else []),
            *(['-DMARBLE_SPEAKER_THUNDERBOLT=1'] if speaker_transport == 'thunderbolt' else []),
            '-framework', 'CoreFoundation',
            str(DEVICE / 'hal/MarbleHapticsPlugIn.c'), '-o', str(binary)], check=True)
    factory = 'EF40B271-9792-4B16-8844-D3E83423F28C'
    info = {'CFBundleDevelopmentRegion': 'en', 'CFBundleExecutable': 'MarbleHaptics',
        'CFBundleIdentifier': 'org.unixnu.marble.haptics', 'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleName': 'MarbleHaptics', 'CFBundlePackageType': 'BNDL',
        'CFBundleShortVersionString': '1.0', 'CFBundleVersion': '1',
        'CFBundleSupportedPlatforms': ['iPhoneOS'], 'MinimumOSVersion': '18.0',
        'CFPlugInFactories': {factory: 'MarbleHaptics_Create'},
        'CFPlugInTypes': {'443ABAB8-E7B3-491A-B985-BEB9187030DB': [factory]}}
    (bundle / 'Info.plist').write_bytes(plistlib.dumps(info))
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier',
        'org.unixnu.marble.haptics', bundle], check=True)
    subprocess.run(['codesign', '--verify', '--strict', bundle], check=True)
    detail = subprocess.run(['codesign', '--display', '--verbose=4', bundle], capture_output=True, text=True, check=True).stderr
    cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M).group(1)
    tunings = output / 'tunings'
    # The installed OS supplies N841 assets; this board advertises D84 to UIKit.
    # Publish the matching haptics preset and VAD chain under the board identity.
    source_tunings = os_root / 'Library/Audio/Tunings/N841'
    for product in ('N841', 'D84'):
        for source in source_tunings.rglob('*'):
            if not source.is_file(): continue
            relative = str(source.relative_to(source_tunings))
            if product != 'N841': relative = relative.replace('N841', product).replace('n841', product.lower())
            destination = tunings / product / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
    directories = {'haptics-hal': 'hal', 'haptics-tunings': 'tunings'}
    cdhashes = [cdhash]
    if diagnostic_probe:
        if wifi_runtime is None:
            raise ValueError('Diagnostic probe requires the current Wi-Fi runtime launchd cache')
        for name in ('bin', 'daemons', 'xpc'):
            (output / name).mkdir(exist_ok=True)
            directories[name] = name
        probe = output / 'bin/marble-haptics-probe'
        with tempfile.TemporaryDirectory() as temporary:
            headers = Path(temporary) / 'CoreAudio'
            headers.mkdir()
            for name in ('AudioHardware.h', 'AudioHardwareBase.h', 'AudioServerPlugIn.h', 'AudioHardwareDeprecated.h'):
                (headers / name).symlink_to(mac / 'System/Library/Frameworks/CoreAudio.framework/Headers' / name)
            subprocess.run(['xcrun', 'clang', '-target', 'arm64e-apple-ios18.0', '-isysroot', ios,
                '-I', temporary, '-I', str(DEVICE / 'package'), '-O2', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                '-framework', 'Foundation', '-framework', 'CoreHaptics', '-framework', 'AVFAudio',
                '-framework', 'AudioToolbox', '-framework', 'CoreAudio',
                DEVICE / 'hal/MarbleHapticsProbe.m', '-o', probe], check=True)
        subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier',
            'org.unixnu.marble.haptics.probe', probe], check=True)
        detail = subprocess.run(['codesign', '--display', '--verbose=4', probe], capture_output=True, text=True, check=True).stderr
        cdhashes.append(re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M).group(1))
        job = {'Label': 'org.unixnu.marble.haptics.probe', 'ProgramArguments': ['/usr/libexec/marble-haptics-probe'],
            'RunAtLoad': True, 'UserName': 'root', 'ProcessType': 'Background'}
        (output / 'daemons/org.unixnu.marble.haptics.probe.plist').write_bytes(plistlib.dumps(job))
        spec = importlib.util.spec_from_file_location('haptics_boot_runtime', DEVICE / 'ios18/source/scripts/build-boot-runtime.py')
        runtime = importlib.util.module_from_spec(spec)
        sys.path.insert(0, str(DEVICE / 'ios18/source/scripts'))
        spec.loader.exec_module(runtime)
        cache = output / 'xpc/launchd.plist'
        runtime.merge_launchd_cache(wifi_runtime / 'xpc/launchd.plist', output / 'daemons', cache)
        cdhashes.append(runtime.sign_launchd_cache(cache)['cdhash'])
    files = {str(path.relative_to(output)): hashlib.sha256(path.read_bytes()).hexdigest()
        for relative in directories.values() for path in sorted((output / relative).rglob('*')) if path.is_file()}
    report = {'target': 'ios18', 'build': '22H384', 'directories': directories, 'files': files,
        'cdhashes': cdhashes, 'diagnostic_probe': diagnostic_probe,
        'device_uid': 'VirtualAudioDevice_Actuator' if virtual_actuator else 'Actuator',
        'virtual_actuator': virtual_actuator, 'sample_rate': 48000, 'pcm': 'PCM16 mono HAL and kernel',
        'speaker_transport': speaker_transport,
        'routing_trace': routing_trace,
        'source_sha256': {str(path.relative_to(DEVICE)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in [DEVICE / 'hal/MarbleHapticsPlugIn.c', DEVICE / 'package/MarbleHapticsAbi.h',
                DEVICE / 'package/audio/MarbleSpeakerAbi.h',
                *([DEVICE / 'hal/MarbleHapticsProbe.m', DEVICE / 'hal/MarbleHapticServerDiagnostics.inc',
                   DEVICE / 'hal/MarbleAudioRouteSnapshots.inc', DEVICE / 'hal/MarbleAudioRoutingTrace.inc',
                   DEVICE / 'hal/MarbleAudioRoutingImports.inc'] if diagnostic_probe else [])]}}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(output)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--diagnostic-probe', action='store_true')
    parser.add_argument('--wifi-runtime', type=Path)
    parser.add_argument('--virtual-actuator', action='store_true')
    parser.add_argument('--speaker-transport', choices=('builtin', 'thunderbolt'), default='builtin')
    parser.add_argument('--routing-trace', action='store_true')
    args = parser.parse_args()
    build(args.os_root.resolve(), args.output.resolve(), args.diagnostic_probe,
          args.wifi_runtime.resolve() if args.wifi_runtime else None, args.virtual_actuator, args.speaker_transport, args.routing_trace)
