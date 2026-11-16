#!/usr/bin/env python3
"""Build arm64e Turnip and Metal from the merged, pinned marble 24B119 source set."""
import argparse
import ast
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from marble_sources import copy_sources
from ios27_migration import DEVICE, WORKSPACE, DEFAULT_OUTPUT, digest, execute, environment, record_stage, save, link


def integration(component):
    report = json.loads((DEVICE / 'records/ios27-24B119/source-integration.json').read_text())
    row = next(c for c in report['components'] if c['name'] == component)
    source = DEVICE / 'ios27/sources' / component
    # A worker may be running compile checks after resolving its recorded
    # conflicts. Source contents and the recorded pins allow the next build
    # stage to proceed while that worker finishes its checks and report.
    for gap in row.get('semantic_gaps', []):
        path = source / gap['path']
        if gap['kind'] not in ('text', 'added_both') or not path.is_file():
            raise ValueError(component + ': resolve source integration gap ' + str(gap))
        if re.search(rb'(?m)^<{7}(?: |$)', path.read_bytes()):
            raise ValueError(f'unresolved source merge: {path}')
    for path in source.rglob('*'):
        if path.is_file() and path.suffix in ('.c', '.h', '.cpp', '.mm', '.rs', '.toml'):
            if re.search(rb'(?m)^<{7}(?: |$)', path.read_bytes()):
                raise ValueError(f'unresolved source merge: {path}')
    return row


def materialize(name, path, scopes=()):
    """Keep a pristine upstream HEAD and record the actual merged delta as one patch."""
    row = integration(name)
    source, repo = DEVICE / 'ios27/sources' / name, WORKSPACE / row['repo']
    revision = row['new_rev']
    if not (path / '.git').is_dir():
        path.mkdir(parents=True, exist_ok=True)
        execute(['git', 'init', '--quiet', str(path)])
        objects = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', '--path-format=absolute',
                                           '--git-path', 'objects'], text=True).strip()
        (path / '.git/objects/info/alternates').write_text(objects + '\n')
        execute(['git', '-C', str(path), 'remote', 'add', 'origin', row['url']])
        execute(['git', '-C', str(path), 'config', 'remote.origin.promisor', 'true'])
        if scopes:
            execute(['git', '-C', str(path), 'sparse-checkout', 'init', '--cone'])
            execute(['git', '-C', str(path), 'sparse-checkout', 'set', *scopes])
        execute(['git', '-C', str(path), 'checkout', '--quiet', '--detach', revision])
    actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != revision:
        checkpoint = path.with_name(path.name + '.source-' + actual[:12])
        if checkpoint.exists():
            raise FileExistsError('GPU source checkpoint path occupied: ' + str(checkpoint))
        path.rename(checkpoint)
        record_stage('gpu_source_refresh', 'materializing', component=name,
                     previous_source=str(checkpoint), revision=revision)
        return materialize(name, path, scopes)
    copy_sources(source, path, ('target', '__pycache__'))
    listed = subprocess.check_output(['git', '-C', str(path), 'ls-tree', '-r', '-z', revision])
    for entry in listed.split(b'\0'):
        if not entry:
            continue
        metadata, relative = entry.split(b'\t', 1)
        mode = metadata.split()[0]
        file = path / os.fsdecode(relative)
        if mode in (b'100644', b'100755') and file.is_file() and not file.is_symlink():
            file.chmod(0o755 if mode == b'100755' else 0o644)
    # File modes belong to the recorded git source. copy_sources retains the
    # self-contained snapshot's modes, including executable generators.
    execute(['git', '-C', str(path), 'add', '-A'])
    patch = subprocess.check_output(['git', '-C', str(path), 'diff', '--cached', '--binary', revision])
    return revision, patch


def replace_assignment(path, name, expression):
    text = path.read_text()
    tree = ast.parse(text)
    matches = [node for node in tree.body if isinstance(node, ast.Assign)
               and any(isinstance(target, ast.Name) and target.id == name for target in node.targets)]
    if len(matches) != 1:
        raise ValueError(f'{path}: expected one assignment for {name}')
    node = matches[0]
    lines = text.splitlines(keepends=True)
    lines[node.lineno - 1:node.end_lineno] = [name + ' = ' + expression + '\n']
    path.write_text(''.join(lines))


def plugin_target_source(root):
    """Select CoreAnimation options by exported names for the target OS."""
    source = DEVICE / 'ios27/sources/AdrenoMetal'
    target = root / 'component-overrides/AdrenoMetal24B119'
    copy_sources(source, target)
    path = target / 'NativeAdrenoMetalPlugin.mm'
    text = path.read_text()
    begin = text.index('#if !TARGET_OS_OSX\n        // The stored option values and the framebuffer branch')
    end = text.index('#else\n        // macOS: the framebuffer format', begin)
    replacement = '''#if !TARGET_OS_OSX
        // 24B119: select exported option names. The setter updates values
        // already read by CoreAnimation as well as the environment defaults.
        auto valueName = reinterpret_cast<const char *(*)(unsigned)>(dlsym(RTLD_DEFAULT, "CAGetDebugValueEnvVariableName"));
        auto setValue = reinterpret_cast<void (*)(unsigned, float)>(dlsym(RTLD_DEFAULT, "CASetDebugValueFloat"));
        auto optionName = reinterpret_cast<const char *(*)(unsigned)>(dlsym(RTLD_DEFAULT, "CAGetDebugOptionEnvVariableName"));
        auto setOption = reinterpret_cast<void (*)(unsigned, bool)>(dlsym(RTLD_DEFAULT, "CASetDebugOption"));
        unsigned values = 0, options = 0;
        for (unsigned i = 0; valueName && setValue && i < 96; ++i) {
            const char *name = valueName(i);
            if (!name) break;
            if (!strcmp(name, "CA_DISPLAY_EDR_HEADROOM")) { setValue(i, 1.0f); ++values; }
            if (!strcmp(name, "CA_FP16_MIN_HEADROOM")) { setValue(i, 1e30f); ++values; }
        }
        for (unsigned i = 0; optionName && setOption && i < 300; ++i) {
            const char *name = optionName(i);
            if (!name) break;
            if (!strcmp(name, "CA_ALLOW_EDR_MIRRORING") || !strcmp(name, "CA_FORCE_FP16_FRAMEBUFFER")) {
                setOption(i, false); ++options;
            }
        }
        gpu->mark(0x6E00 | (values == 2 ? 1 : 0) | (options == 2 ? 2 : 0));
        os_log(OS_LOG_DEFAULT, "NativeAdrenoMetal: 24B119 display options values=%u flags=%u", values, options);
'''
    path.write_text(text[:begin] + replacement + text[end:])
    link(target, root / 'tree/drivers/AdrenoMetal')
    save(root / 'gpu/coreanimation-target.json', {
        'build': '24B119', 'source_sha256': digest(source / path.name), 'generated_sha256': digest(path),
        'mechanism': 'exported CAGetDebug*EnvVariableName and CASetDebug* calls',
        'exports_checked': ['CAGetDebugValueEnvVariableName', 'CASetDebugValueFloat',
                            'CAGetDebugOptionEnvVariableName', 'CASetDebugOption'],
        'setter_abi': 'CASetDebugValueFloat takes option in w0 and float in s0; verified against 24B119 instructions',
        'float_option_limit': 96, 'boolean_option_limit': 300, 'device_result': 'pending'})


def prepare(root, translator=False):
    tree = root / 'tree'
    for name in ('build-turnip-darwin.py', 'shader_cache_identity.py', 'build-adreno-metal-plugin.py'):
        shutil.copy2(DEVICE / 'ios27/source/scripts' / name, tree / 'scripts' / name)
    origins = json.loads((DEVICE / 'source_origins.json').read_text())['sources']
    scopes = next(r['source_paths'] for r in origins if r['name'] == 'ios18-Mesa')
    mesa, patch = materialize('Mesa', tree / '.cache/mesa-work', scopes)
    target = tree / 'patches/mesa/ios27-marble-merged.patch'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(patch)
    pins = tree / 'scripts/data/adreno-compiler-inputs.json'
    data = json.loads(pins.read_text())
    data['mesa']['commit'] = mesa
    data['patches'] = [str(target.relative_to(tree))]
    pins.write_text(json.dumps(data, indent=2) + '\n')
    # Reuse only host tools; source trees and target build outputs are isolated.
    old = WORKSPACE / 'outputs/UniXNU_marble/ios18_arm64e/gpu/UniXNU/.cache'
    for name in ('mesa-venv', 'mesa-tools'):
        if (old / name).is_dir():
            link(old / name, tree / '.cache' / name)
    result = {'Mesa': {'revision': mesa, 'delta_sha256': digest(target)}}
    if translator:
        revision, patch = materialize('metal2vulkan', tree / '.cache/gpu-reference/metal2vulkan')
        target = tree / 'patches/metal2vulkan/ios27-marble-merged.patch'
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(patch)
        pins = tree / 'scripts/data/gpu-metal-vulkan-inputs.json'
        data = json.loads(pins.read_text())
        data['translator']['commit'] = revision
        pins.write_text(json.dumps(data, indent=2) + '\n')
        script = tree / 'scripts/build-adreno-metal-bridge.py'
        replace_assignment(script, 'PATCHES', "[ROOT / 'patches/metal2vulkan/ios27-marble-merged.patch']")
        result['metal2vulkan'] = {'revision': revision, 'delta_sha256': digest(target)}
        plugin_target_source(root)
    save(root / 'gpu/source-build.json', result)
    return tree


def addon(root):
    output = root / 'tree/build/adreno-metal-plugin27'
    trust = json.loads((output / 'trust.json').read_text())
    # These maps are consumed by the existing runtime addon mount contract.
    staging = root / 'gpu/stage'
    for name in ('addons', 'bin', 'daemons'):
        (staging / name).mkdir(parents=True, exist_ok=True)
    copy_sources(output / 'NativeAdrenoMetal.bundle', staging / 'addons/NativeAdrenoMetal.bundle')
    shutil.copy2(output / 'adrenoprobed', staging / 'bin/adrenoprobed')
    shutil.copyfile(output / 'org.blu-sharky.adrenoprobed.plist', staging / 'daemons/org.blu-sharky.adrenoprobed.plist')
    hashes = sorted(bytes.fromhex(trust[k]) for k in ('cdhash', 'turnip_cdhash', 'daemon_cdhash'))
    import struct
    owner = root / 'gpu/owner.trustcache'
    owner.write_bytes(struct.pack('<I16sI', 2, hashlib.sha256(b''.join(hashes)).digest()[:16], len(hashes)) +
                      b''.join(value + bytes([2, 0, 0, 0]) for value in hashes))
    target = root / 'gpu/metal.gpuaddon'
    tree = root / 'tree'
    # make-gpu-addon.py's CLI is the authoritative format implementation.
    with tempfile.TemporaryDirectory(prefix='metal-pack-', dir=root / 'gpu') as scratch:
        candidate = Path(scratch) / 'metal.gpuaddon'
        execute([sys.executable, str(tree / 'scripts/make-gpu-addon.py'), '--publish', '--no-daemon', str(candidate)],
                cwd=tree, env=dict(environment(root), ADRENO_PLUGIN_DIR=str(output)), log=root / 'logs/gpu-addon.log')
        candidate.replace(target)
    save(root / 'gpu/userspace.json', {'target': 'ios27', 'build': '24B119', 'addon_sha256': digest(target),
                                    'trust_cache_sha256': digest(owner), 'cdhashes': trust,
                                    'sources': json.loads((root / 'gpu/source-build.json').read_text()),
                                    'device_result': 'pending'})
    record_stage('gpu_userspace', 'built', report=str(root / 'gpu/userspace.json'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prepare', 'turnip', 'plugin', 'addon', 'all'))
    parser.add_argument('--root', type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    root = args.root.resolve()
    (root / 'gpu').mkdir(parents=True, exist_ok=True)
    if args.command in ('prepare', 'turnip', 'plugin', 'all'):
        tree = prepare(root, translator=args.command in ('prepare', 'plugin', 'all'))
        env = environment(root)
        if args.command in ('turnip', 'all'):
            execute([sys.executable, str(tree / 'scripts/build-turnip-darwin.py'), '--target', 'ios27'],
                    cwd=tree, env=env, log=root / 'logs/turnip27.log')
            record_stage('turnip', 'built', log=str(root / 'logs/turnip27.log'))
        if args.command in ('plugin', 'all'):
            integration('AdrenoMetal')
            expected_source = json.loads((root / 'gpu/coreanimation-target.json').read_text())['generated_sha256']
            execute([sys.executable, str(tree / 'scripts/build-adreno-metal-plugin.py'), '--target', 'ios27', '--no-replay'],
                    cwd=tree, env=env, log=root / 'logs/metal-plugin27.log')
            actual_source = tree / 'drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm'
            if digest(actual_source) != expected_source:
                raise ValueError('24B119 plugin source changed during its build')
            binary = tree / 'build/adreno-metal-plugin27/NativeAdrenoMetal.bundle/NativeAdrenoMetal'
            if b'24B119 display options values=%u flags=%u' not in binary.read_bytes():
                raise ValueError('24B119 plugin requires the named CoreAnimation option adapter')
            record_stage('metal_plugin', 'built', source_sha256=expected_source,
                         binary=str(binary), binary_sha256=digest(binary),
                         log=str(root / 'logs/metal-plugin27.log'))
    if args.command in ('addon', 'all'):
        addon(root)


if __name__ == '__main__':
    main()
