#!/usr/bin/env python3
"""Replace Turnip in a retained 24B119 payload and renew its signed bundle and trust module."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import plistlib

from assemble_ios27_runtime import unpack_addon, pack_addon
from build_ios27_runtime import codesign, cdhash, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-payload', required=True, type=Path)
    parser.add_argument('--turnip', required=True, type=Path)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--plugin-bundle', type=Path)
    parser.add_argument('--debug-agent', type=Path)
    args = parser.parse_args()
    base, output = args.base_payload.resolve(), args.output.resolve()
    report = json.loads((base / 'report.json').read_text())
    assert report['build'] == '24B119'
    assert sha256(base / 'runtime.gpuaddon') == report['addon_sha256']
    assert sha256(base / 'owner.trustcache') == report['trust_cache_sha256']
    files = unpack_addon(base / 'runtime.gpuaddon')
    prefix = 'addons/NativeAdrenoMetal.bundle/'
    before = files[prefix + 'libvulkan_freedreno.dylib'][1]
    output.mkdir(parents=True, exist_ok=False)
    bundle = output / 'NativeAdrenoMetal.bundle'
    for name, (mode, data) in files.items():
        if name.startswith(prefix):
            target = bundle / name[len(prefix):]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            target.chmod(mode)
    if args.plugin_bundle:
        for name in ('NativeAdrenoMetal', 'Info.plist', 'gpu-build.json'):
            shutil.copy2(args.plugin_bundle / name, bundle / name)
        executable = bundle / 'NativeAdrenoMetal'
        dependencies = subprocess.check_output(['otool', '-L', str(executable)], text=True)
        foundation = '/System/Library/Frameworks/Foundation.framework/Foundation'
        if foundation in dependencies:
            subprocess.run(['install_name_tool', '-change', foundation,
                '@loader_path/MarblePhoneUI.dylib', str(executable)], check=True)
        else:
            assert '@loader_path/MarblePhoneUI.dylib' in dependencies
        codesign(executable, entitlements=None)
    turnip = bundle / 'libvulkan_freedreno.dylib'
    shutil.copyfile(args.turnip, turnip)
    # Missing optional Vulkan entry points stay local NULL entries. A flat
    # namespace otherwise searches every loaded image for thousands of names.
    binding = Path(__file__).resolve().parents[1] / 'ios27/source/scripts/macho-self-bind-weak-imports.py'
    subprocess.run([sys.executable, str(binding), str(turnip)], check=True)
    codesign(turnip, entitlements=None)
    metadata = json.loads((bundle / 'gpu-build.json').read_text())
    metadata['turnip'] = json.loads(args.profile.read_text())
    identity_path = args.profile.with_name('compiler-cache-identity.json')
    if identity_path.is_file():
        identity = json.loads(identity_path.read_text())
        assert identity['binary_sha256'] == sha256(args.turnip)
        metadata['compiler_cache_identity'] = identity
    (bundle / 'gpu-build.json').write_text(json.dumps(metadata, indent=2) + '\n')
    codesign(bundle)
    changed = []
    for path in sorted(bundle.rglob('*')):
        if path.is_file():
            name = prefix + path.relative_to(bundle).as_posix()
            value = (path.stat().st_mode & 0o777, path.read_bytes())
            if files.get(name) != value:
                changed.append(name)
            files[name] = value
    # Retain every existing owner identity and add both renewed code directories.
    trust = (base / 'owner.trustcache').read_bytes()
    version, _, count = struct.unpack_from('<I16sI', trust)
    assert version == 2 and len(trust) == 24 + count * 24
    entries = {trust[i:i + 24] for i in range(24, len(trust), 24)}
    for path in (turnip, bundle):
        entries.add(bytes.fromhex(cdhash(path)) + bytes([2, 0, 0, 0]))
    if args.debug_agent:
        agent = args.debug_agent.resolve()
        debug = json.loads((agent / 'report.json').read_text())
        binary = agent / 'bin/marble-debug'
        assert sha256(binary) == debug['binary_sha256']
        for path in (agent / 'daemons').glob('*.plist'):
            name = 'daemons/' + path.name
            assert plistlib.loads(files[name][1]) == plistlib.loads(path.read_bytes())
        files['bin/marble-debug'] = (0o755, binary.read_bytes())
        changed.append('bin/marble-debug')
        entries.add(bytes.fromhex(cdhash(binary)) + bytes([2, 0, 0, 0]))
        report['usb_debug_agent'] = debug
    body = b''.join(sorted(entries))
    owner = output / 'owner.trustcache'
    owner.write_bytes(struct.pack('<I16sI', 2, hashlib.sha256(body).digest()[:16], len(entries)) + body)
    addon = output / 'runtime.gpuaddon'
    pack_addon(files, addon)
    assert unpack_addon(addon) == files
    report.update(addon=str(addon), addon_sha256=sha256(addon),
                  owner_trust_cache=str(owner), trust_cache_sha256=sha256(owner),
                  cdhash_count=len(entries), device_result='pending')
    report['files'] = {name: dict(sha256=hashlib.sha256(data).hexdigest(), mode=mode)
                       for name, (mode, data) in sorted(files.items())}
    report['turnip_replacement'] = dict(base_payload=str(base), changed=changed,
        previous_sha256=hashlib.sha256(before).hexdigest(), sha256=sha256(turnip),
        profile=str(args.profile.resolve()), profile_sha256=sha256(args.profile))
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['turnip_replacement'], indent=2), flush=True)


if __name__ == '__main__':
    main()
