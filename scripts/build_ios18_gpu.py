#!/usr/bin/env python3
"""Build the Marble A725 kernel and Metal runtime for the 22H384 target."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib

from prepare_ios18_wallpaper import prepare_wallpaper
from marble_sources import stage_ios18, stage_ios18_build

DEVICE = Path(__file__).resolve().parents[1]
LLVM = Path('/opt/homebrew/opt/llvm/bin')
PROFILE = json.loads((DEVICE / 'profiles/gpu-22H384.json').read_text())
SOURCES = PROFILE['sources']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def phone_ui(root):
    gpu = root / 'gpu'
    manifest = json.loads((gpu / 'userspace.json').read_text())
    addon, owner = gpu / 'metal.gpuaddon', gpu / 'owner.trustcache'
    if sha(addon) != manifest['addon_sha256'] or sha(owner) != manifest['trust_cache_sha256']:
        raise RuntimeError('Phone UI input differs from its signed userspace manifest')
    with tempfile.TemporaryDirectory(prefix='phone-ui-', dir=gpu) as temporary:
        stage = Path(temporary) / 'stage'
        subprocess.run([sys.executable, DEVICE / 'scripts/build_phone_ui.py', '--addon', addon,
                        '--output', stage, '--target', 'ios18'], check=True)
        report = json.loads((stage / 'phone-ui.json').read_text())
        if report['turnip_sha256'] != manifest.get('turnip_bundle_sha256', report['turnip_sha256']):
            raise RuntimeError('Phone UI bundle changed the signed Turnip library')
        shutil.copyfile(stage / 'phone-ui.gpuaddon', addon)
    identity = tomllib.loads((DEVICE / 'profiles/ios-18.7.11-22H384.toml').read_text())['ui_identity']
    wallpaper = prepare_wallpaper(addon, Path('/Volumes/CrystalUpdate22H384.N841OS'),
                                  identity, gpu / 'wallpaper-assets')
    trust = manifest['cdhashes']
    trust['cdhash'], trust['phone_ui_cdhash'] = report['cdhashes'][1], report['cdhashes'][0]
    entries = sorted(bytes.fromhex(trust[key]) for key in
                     ('cdhash', 'turnip_cdhash', 'daemon_cdhash', 'phone_ui_cdhash'))
    identity = hashlib.sha256(b''.join(entries)).digest()[:16]
    owner.write_bytes(struct.pack('<I16sI', 2, identity, len(entries)) + b''.join(
        entry + bytes([2, 0, 0, 0]) for entry in entries))
    manifest.update(addon_sha256=sha(addon), trust_cache_sha256=sha(owner),
                    turnip_bundle_sha256=report['turnip_sha256'],
                    phone_ui_target=report['target'], phone_ui_source_sha256=sha(DEVICE / 'package/MarblePhoneUI.m'),
                    wallpaper=wallpaper)
    (gpu / 'userspace.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(addon)


def source_tree(root):
    gpu = root / 'gpu'
    gpu.mkdir(exist_ok=True)
    tree = gpu / 'UniXNU'
    stage_ios18_build(tree)
    for name in SOURCES:
        if name != 'UniXNU':
            stage_ios18(name, gpu / name)
    for name in ('AdrenoPM4', 'AdrenoFirmware'):
        stage_ios18(name, gpu / name)
    stage_ios18('GopFramebuffer', root / 'GopFramebuffer')
    host_patch = DEVICE / 'patches/UniXNU/ios18-gpu-host.patch'
    applied = subprocess.run(['git', '-C', tree, 'apply', '--reverse', '--check', host_patch],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if applied.returncode:
        subprocess.run(['git', '-C', tree, 'apply', host_patch], check=True)
    links = {
        gpu / 'GopFramebuffer': root / 'GopFramebuffer',
        tree / 'drivers/Adreno740': gpu / 'Adreno740',
        tree / 'drivers/AdrenoMetal': gpu / 'AdrenoMetal',
        tree / 'drivers/GopFramebuffer': root / 'GopFramebuffer',
        tree / 'kits/ArmSmmu4K': gpu / 'ArmSmmu4K',
        tree / 'kits/AdrenoPM4': gpu / 'AdrenoPM4',
        tree / 'kits/AdrenoFirmware': gpu / 'AdrenoFirmware',
    }
    for path, target in links.items():
        if path.is_symlink() and path.resolve() != target.resolve():
            path.unlink()
        if path.is_dir() and not path.is_symlink() and not any(path.iterdir()):
            path.rmdir()
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to(target.resolve(), target_is_directory=True)
    cache = tree / '.cache'
    if cache.is_symlink():
        cache.unlink()
    cache.mkdir(exist_ok=True)
    shared = DEVICE.parent / 'UniXNU/.cache'
    for path in shared.iterdir():
        target = cache / path.name
        if path.name != 'gpu-reference' and not target.exists():
            target.symlink_to(path.resolve(), target_is_directory=path.is_dir())
    reference = cache / 'gpu-reference'
    reference.mkdir(exist_ok=True)
    stage_ios18('metal2vulkan', reference / 'metal2vulkan')
    inputs = tree / 'scripts/data/gpu-metal-vulkan-inputs.json'
    pins = json.loads(inputs.read_text())
    pin = PROFILE['translator']
    pins['translator'] = {key: pin[key] for key in ('repository', 'commit')}
    inputs.write_text(json.dumps(pins, indent=2) + '\n')
    (gpu / 'sources.json').write_text(json.dumps(SOURCES, indent=2) + '\n')
    return tree


def compile_source(tree, source, obj, includes, log):
    record_path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / (
        'bsd/DEVELOPMENT/kern_exec.o.json' if source.suffix == '.c'
        else 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
    record = json.loads(record_path.read_text().strip().rstrip(','))
    command, args = [], iter(record['arguments'])
    for value in args:
        if value == record['file']:
            command.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            next(args)
            if value == '-o':
                command += ['-o', str(obj)]
        elif value != '-flto' and not value.startswith('-O'):
            command.append(value)
    command += ['-O2', '-g', '-fno-sanitize=signed-integer-overflow', '-DMARBLE_IOS18_GPU=1',
                '-Wno-error', '-Wno-c99-designator', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-missing-include-dirs', '-Wno-unused-parameter', '-Wno-overloaded-virtual']
    for path in [tree / '.cache/xnu-work/osfmk', DEVICE / 'package', *includes]:
        command += ['-I', str(path)]
    subprocess.run(command, cwd=record['directory'], stdout=log, stderr=subprocess.STDOUT, check=True)
    magic, cpu, subtype, kind = struct.unpack_from('<4I', obj.read_bytes())
    if (magic, cpu, subtype & 0xffffff, kind) != (0xfeedfacf, 0x100000c, 2, 1):
        raise RuntimeError(f'GPU kernel object requires arm64e: {obj}')


def accelerator_abi(root, obj, out):
    sys.path.insert(0, str(root / 'XnuPrelink'))
    from kc_image import Image
    cache_path = root / 'ipsw/22H384/kernelcache.decompressed'
    original = Image(cache_path.read_bytes())
    if sha(cache_path) != PROFILE['kernelcache_sha256']:
        raise RuntimeError('GPU ABI requires the pinned 22H384 collection')
    fixups = original.fixups()
    vptr, meta = int(PROFILE['vptr'], 16), int(PROFILE['metaclass'], 16)
    # The base constructor calls IOService C2, installs the authenticated base vptr, and returns.
    constructor, destructor = int(PROFILE['constructor'], 16), int(PROFILE['destructor'], 16)
    if struct.unpack('<4I', original.read(constructor, 16)) != tuple(
            int(word, 16) for word in PROFILE['constructor_prefix']):
        raise RuntimeError('22H384 accelerator constructor contract changed')
    raw = obj.read_bytes()
    commands, segments, position = [], [], 32
    for _ in range(struct.unpack_from('<I', raw, 16)[0]):
        kind, length = struct.unpack_from('<II', raw, position)
        commands.append((kind, position))
        if kind == 0x19:
            segments.append(position)
        position += length
    sym = next(position for kind, position in commands if kind == 2)
    symoff, count, stroff, _ = struct.unpack_from('<4I', raw, sym + 8)
    symbols = []
    for index in range(count):
        string, kind, _, _, value = struct.unpack_from('<IBBHQ', raw, symoff + index * 16)
        symbols.append((raw[stroff + string:raw.index(b'\0', stroff + string)].decode(), kind, value))
    table = next(value for name, _, value in symbols if name == '__ZTV14AGXAccelerator')
    entries = {}
    for position in segments:
        for index in range(struct.unpack_from('<I', raw, position + 64)[0]):
            section = position + 72 + index * 80
            va, size, fileoff = struct.unpack_from('<QQI', raw, section + 32)
            reloc, count = struct.unpack_from('<II', raw, section + 56)
            for n in range(count):
                offset, bits = struct.unpack_from('<II', raw, reloc + n * 8)
                if bits >> 27 & 1 and bits >> 28 == 11 and bits >> 25 & 3 == 3:
                    word = struct.unpack_from('<Q', raw, fileoff + offset)[0]
                    entries[va + offset] = (symbols[bits & 0xffffff][0], word >> 32 & 0xffff)
    for slot in range(168):
        name, diversity = entries[table + 16 + slot * 8]
        target, auth, key, expected, address = fixups[vptr + slot * 8]
        if (auth, key, address, diversity) != (1, 0, 1, expected):
            raise RuntimeError(f'22H384 accelerator PAC slot {slot} differs: {name}')
    bindings = {'__ZN15IOAcceleratorESC2EPK11OSMetaClass': constructor,
                '__ZN15IOAcceleratorESD2Ev': destructor}
    text, addresses = '.text\n.p2align 2\n', ''
    for name, address in bindings.items():
        text += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
        addresses += f'.globl {name}.original\n.set {name}.original, {address:#x}\n'
    addresses += f'.globl __ZN15IOAcceleratorES10gMetaClassE\n.set __ZN15IOAcceleratorES10gMetaClassE, {meta:#x}\n'
    for name, contents in [('gpu-bindings', text), ('gpu-addresses', addresses)]:
        path = out / (name + '.S')
        path.write_text(contents)
        subprocess.run(['xcrun', 'clang', '--target=arm64e-apple-ios18.0', '-c', path,
                        '-o', path.with_suffix('.o')], check=True)
    profile = dict(build='22H384', kernelcache_sha256=sha(cache_path), instance_bytes=136,
                   virtual_entries=168, vptr=hex(vptr), metaclass=hex(meta),
                   constructor=hex(constructor), destructor=hex(destructor), pac_slots_checked=168)
    (out / 'gpu-abi.json').write_text(json.dumps(profile, indent=2) + '\n')
    return [out / 'gpu-bindings.o', out / 'gpu-addresses.o']


def kernel(root):
    source = source_tree(root)
    tree = root / 'tree'
    out = tree / 'build/marble-gpu'
    out.mkdir(parents=True, exist_ok=True)
    driver = source / 'drivers/Adreno740'
    includes = [out, driver, source / 'drivers/AdrenoMetal', source / 'drivers/GopFramebuffer',
                source / 'kits/ArmSmmu4K', source / 'kits/AdrenoPM4', source / 'kits/AdrenoFirmware',
                DEVICE / 'package']
    for name in ('OriginalAcceleratorABI.h',):
        shutil.copyfile(source / 'drivers/AdrenoMetal' / name, out / name)
    accelerator = out / 'NativeAdrenoAccelerator.cpp'
    accelerator.write_text((driver / accelerator.name).read_text().replace(
        '#include "../AdrenoMetal/OriginalAcceleratorABI.h"', '#include "OriginalAcceleratorABI.h"'))
    sources = [DEVICE / 'package' / name for name in
               ('MarbleGpu.cpp', 'MarbleGmu.cpp', 'MarbleAdreno.cpp', 'MarbleZap.cpp')]
    sources += [driver / ('NativeAdreno' + name + '.cpp') for name in
                ('Memory', 'Cpu', 'SmmuMemory', 'SmmuContext', 'Vm', 'GpuContext', 'Ring', 'CpInit',
                 'Pwrup', 'Preempt', 'Submit', 'UserClient', 'Publish', 'Timer', 'KernelTime')]
    sources += sorted((source / 'kits/ArmSmmu4K').glob('ArmSmmu4K*.c'))
    sources += [source / 'kits/AdrenoPM4/AdrenoPM4.c']
    sources += [source / 'kits/AdrenoFirmware' / name for name in
                ('AdrenoFirmware.c', 'AdrenoHfi.c', 'AdrenoHfiVotes.c')]
    sources += [source / 'drivers/GopFramebuffer/NativeGOPPresenter.cpp', accelerator]
    objects = []
    with (out / 'compile.log').open('w') as log:
        for path in sources:
            obj = out / (path.stem + '.o')
            print('GPU:', path.name, flush=True)
            compile_source(tree, path, obj, includes, log)
            if path.name == accelerator.name:
                objects += accelerator_abi(root, obj, out)
            if path.name in ('NativeAdrenoAccelerator.cpp', 'NativeGOPPresenter.cpp'):
                late = obj.with_suffix('.late.o')
                subprocess.run([LLVM / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
                                '--globalize-symbol=__GLOBAL__sub_I_' + path.name, obj, late], check=True)
                obj = late
            objects.append(obj)
    firmware = DEVICE.parent / 'UniXNU/.cache/firmware/marble'
    assembly = '.section __DATA_CONST,__const\n'
    for name, file, expected in [
        ('gmu', 'gmu_gen70000.bin', 'b2d3f0e2a98fd6259ab9ffef16df63265542e6cc9a970d53d476e823d432042a'),
        ('sqe', 'a730_sqe.fw', 'e67d1829f57fc8326d806234b932dc3b78e28a9940df34c70752f17d5f38b7aa'),
        ('zap', 'a730_zap.mbn', 'f03e732d07046c01250998d7f1ee2a2fae3e3ae28872320cdd38334e60e9a10e')]:
        path = firmware / file
        if sha(path) != expected:
            raise RuntimeError(f'A725 firmware differs: {file}')
        label = f'_marble_{name}_firmware'
        assembly += (f'.p2align 12\n.globl {label}\n{label}:\n.incbin "{path}"\n'
                     f'{label}_end:\n.p2align 3\n.globl {label}_size\n{label}_size:\n.quad {label}_end-{label}\n')
    (out / 'firmware.S').write_text(assembly)
    subprocess.run(['xcrun', 'clang', '--target=arm64e-apple-ios18.0', '-c', out / 'firmware.S',
                    '-o', out / 'firmware.o'], check=True)
    objects.append(out / 'firmware.o')
    archive = out / 'libMarbleGpu.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
    (out / 'build.json').write_text(json.dumps(dict(sources=SOURCES, archive=str(archive),
        sha256=sha(archive), objects=[p.name for p in objects], abi='arm64e / 22H384'), indent=2) + '\n')
    print(archive)


def userspace(root):
    tree = source_tree(root)
    mesa = root / 'gpu/Mesa'
    stage_ios18('Mesa', mesa)
    source_link = tree / '.cache/mesa-work'
    if source_link.is_symlink():
        source_link.unlink()
    if not source_link.exists():
        source_link.symlink_to(mesa.resolve(), target_is_directory=True)
    manifest = tree / 'scripts/data/adreno-compiler-inputs.json'
    pins = json.loads((DEVICE / 'ios18/source/scripts/data/adreno-compiler-inputs.json').read_text())
    manifest.write_text(json.dumps(pins, indent=2) + '\n')
    turnip_out = tree / 'build/mesa-tu-ios18'
    info = turnip_out / 'meson-info/meson-info.json'
    if info.is_file() and Path(json.loads(info.read_text())['directories']['source']).resolve() != mesa.resolve():
        previous = turnip_out.with_name('mesa-tu-ios18-before-source-fix')
        turnip_out.rename(previous)
    # arm64e Metal callers use the upstream Rust PAC boundary, built against the 18.0 ABI.
    for name in ('build-turnip-darwin.py', 'build-adreno-metal-plugin.py', 'build-adreno-metal-bridge.py'):
        original = (DEVICE / 'ios18/source/scripts' / name).read_text()
        translated = (original.replace('ios27', 'ios18').replace('ios27.0', 'ios18.0')
                                           .replace("'27.0'", "'18.0'").replace('iOS 27', 'iOS 18')
                                           .replace('plugin27', 'plugin18'))
        if name == 'build-adreno-metal-plugin.py':
            translated = translated.replace('bundle.mkdir(parents=True)',
                'bundle.mkdir(parents=True)\nshutil.copyfile(ROOT / "build/QuartzCore.metallib", bundle / "QuartzCore.metallib")')
        (tree / 'scripts' / name).write_text(translated)
    build = tree / 'build'
    build.mkdir(exist_ok=True)
    # Keep the Rust dependency cache; Cargo binds each archive to its source and compilation target.
    rust_cache = build / 'm2v-target'
    existing = DEVICE.parent / 'UniXNU/build/m2v-target'
    if not rust_cache.exists() and existing.exists():
        rust_cache.symlink_to(existing.resolve(), target_is_directory=True)
    volumes = [Path('/Volumes/CrystalUpdate22H384.N841OS'),
               Path('/Volumes/CrystalUpdate22H384.N841SystemCryptex')]
    libraries = set()
    for volume in volumes:
        if not volume.is_dir():
            raise RuntimeError(f'Mount the original 22H384 volume: {volume}')
        for path in volume.rglob('*.metallib'):
            if path.is_file():
                libraries.add('/' + str(path.relative_to(volume)))
    quartzcore = volumes[1] / 'System/Library/Frameworks/QuartzCore.framework/default.metallib'
    shutil.copyfile(quartzcore, build / 'QuartzCore.metallib')
    libraries.discard('/System/Library/Frameworks/QuartzCore.framework/default.metallib')
    libraries.add('/System/Library/Extensions/NativeAdrenoMetal.bundle/QuartzCore.metallib')
    prime = build / 'prime-list.txt'
    prime.write_text(''.join(name + '\n' for name in sorted(libraries)))
    env = dict(os.environ, PATH=os.pathsep.join([str(Path(sys.executable).parent),
               str(Path.home() / '.cargo/bin'), '/opt/homebrew/bin', '/opt/homebrew/opt/llvm/bin',
               os.environ['PATH']]))
    for name, arguments in [('turnip', ['build-turnip-darwin.py', '--target', 'ios18']),
                            ('plugin', ['build-adreno-metal-plugin.py', '--target', 'ios18', '--no-replay'])]:
        with (root / 'gpu' / (name + '-build.log')).open('w') as log:
            subprocess.run([sys.executable, tree / 'scripts' / arguments[0], *arguments[1:]],
                           cwd=tree, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    addon = root / 'gpu/metal.gpuaddon'
    addon.unlink(missing_ok=True)
    env['ADRENO_PLUGIN_DIR'] = str(build / 'adreno-metal-plugin18')
    subprocess.run([sys.executable, tree / 'scripts/make-gpu-addon.py', '--publish', '--no-daemon',
                    '--prime-list', prime, addon], env=env, check=True)
    trust = json.loads((build / 'adreno-metal-plugin18/trust.json').read_text())
    entries = sorted(bytes.fromhex(trust[key]) for key in ('cdhash', 'turnip_cdhash', 'daemon_cdhash'))
    identity = hashlib.sha256(b''.join(entries)).digest()[:16]
    module = struct.pack('<I16sI', 2, identity, len(entries)) + b''.join(
        entry + bytes([2, 0, 0, 0]) for entry in entries)
    owner = root / 'gpu/owner.trustcache'
    owner.write_bytes(module)
    (root / 'gpu/userspace.json').write_text(json.dumps(dict(
        sources=SOURCES, target='arm64e-apple-ios18.0', addon=str(addon), addon_sha256=sha(addon),
        trust_cache=str(owner), trust_cache_sha256=sha(owner), cdhashes=trust,
        primelist_entries=len(libraries), translator=PROFILE['translator']['commit'],
        translator_patch_sha256=sha(DEVICE / PROFILE['translator']['patch']),
        quartzcore_metallib_sha256=sha(quartzcore),
        turnip_sha256=sha(build / 'mesa-tu-ios18/src/freedreno/vulkan/libvulkan_freedreno.dylib')),
        indent=2) + '\n')
    phone_ui(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('kernel', 'userspace', 'ui', 'all'))
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--assets', type=Path)
    parser.add_argument('--images', type=Path)
    parser.add_argument('--data-seed', type=Path)
    parser.add_argument('--skip-setup', action='store_true')
    args = parser.parse_args()
    root = args.root.resolve()
    if args.command != 'all':
        {'kernel': kernel, 'userspace': userspace, 'ui': phone_ui}[args.command](root)
        return
    if not args.images or not args.data_seed:
        parser.error('all requires --images and --data-seed from the retained iOS 18 installation')
    userspace(root)
    for command in ('kernel', 'prelink'):
        subprocess.run([sys.executable, DEVICE / 'scripts/build_ios18_arm64e.py', command,
                        '--output', root], check=True)
    subprocess.run([sys.executable, DEVICE / 'scripts/pack_ios18_ufs.py', '--root', root,
                    '--assets', (args.assets or root / 'gpu/boot').resolve(),
                    '--images', args.images.resolve(), '--data-seed', args.data_seed.resolve(),
                    '--temporary', *(['--skip-setup'] if args.skip_setup else [])], check=True)


if __name__ == '__main__':
    main()
