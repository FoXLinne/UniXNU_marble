#!/usr/bin/env python3
"""Fetch iOS 18 ARM64e inputs, build XNU, and bind the original kernel collection."""
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
from types import SimpleNamespace

DEVICE = Path(__file__).resolve().parents[1]
PROFILE = DEVICE / 'profiles/ios-18.7.11-22H384.toml'


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def checkout(path, url, revision, tag=None):
    if not (path / '.git').exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        if tag:
            subprocess.run(['git', 'clone', '--depth', '1', '--branch', tag, url, path], check=True)
        else:
            subprocess.run(['git', 'init', '--quiet', path], check=True)
            subprocess.run(['git', '-C', path, 'remote', 'add', 'origin', url], check=True)
            subprocess.run(['git', '-C', path, 'fetch', '--depth', '1', 'origin', revision], check=True)
            subprocess.run(['git', '-C', path, 'checkout', '--quiet', '--detach', revision], check=True)
    if git(path, 'rev-parse', 'HEAD') != revision:
        raise RuntimeError(f'{path}: source revision differs from the OS profile')


def apply_patch(work, patch):
    applied = subprocess.run(['git', '-C', work, 'apply', '--reverse', '--check', patch],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if applied.returncode:
        subprocess.run(['git', '-C', work, 'apply', '--check', patch], check=True)
        subprocess.run(['git', '-C', work, 'apply', patch], check=True)


def prepare(out, upstream, stack, profile):
    if git(upstream, 'rev-parse', 'HEAD') != profile['upstream']['commit']:
        raise RuntimeError('UniXNU build scripts differ from the OS profile revision')
    os_spec = profile['os']
    reference = out / 'xnu'
    checkout(reference, 'https://github.com/apple-oss-distributions/xnu.git',
             os_spec['xnu_revision'], 'xnu-' + os_spec['xnu_version'])
    tree = out / 'tree'
    (tree / 'refer').mkdir(parents=True, exist_ok=True)
    (tree / '.cache').mkdir(exist_ok=True)
    for source in (stack / 'refer').iterdir():
        target = tree / 'refer' / source.name
        if not target.exists():
            target.symlink_to(reference if source.name == 'xnu' else source.resolve(), target_is_directory=True)
    for name in ('drivers', 'kits', 'package'):
        target = tree / name
        if not target.exists():
            target.symlink_to(stack / name, target_is_directory=True)
    shutil.copytree(upstream / 'scripts', tree / 'scripts', dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('__pycache__'))
    apply_patch(tree, DEVICE / profile['upstream']['scripts_patch'])
    shutil.copyfile(upstream / 'components.toml', tree / 'components.toml')
    work = tree / '.cache/xnu-work'
    if not work.exists():
        subprocess.run(['git', 'clone', '--quiet', '--shared', reference, work], check=True)
    if git(work, 'rev-parse', 'HEAD') != os_spec['xnu_revision']:
        raise RuntimeError('Prepared XNU source differs from the OS profile revision')
    patch = DEVICE / profile['kernel']['patch']
    apply_patch(work, patch)
    patch_list = out / 'xnu-patches.list'
    patch_list.write_text(str(patch) + '\n')
    env = dict(os.environ, UNIXNU_XNU_TAG='xnu-' + os_spec['xnu_version'],
               UNIXNU_XNU_COMMIT=os_spec['xnu_revision'], UNIXNU_XNU_DARWIN=os_spec['darwin'],
               UNIXNU_XNU_VERSION=os_spec['xnu_version'], UNIXNU_XNU_ARCH=os_spec['arch'],
               UNIXNU_IOS_VERSION=os_spec['ios_version'], UNIXNU_DRIVERKIT_TAG=os_spec['driverkit_tag'],
               UNIXNU_PATCH_LIST=str(patch_list), XNU_BOOTSTRAP='1', XNU_KERNEL_CHAINED_FIXUPS='1',
               JOBS='8', PATH=os.pathsep.join([str(Path(sys.executable).parent),
                       '/opt/homebrew/opt/llvm/bin', '/opt/homebrew/opt/lld/bin', os.environ['PATH']]))
    return tree, env


def release(tree, out, env, profile):
    sdk = tree / '.cache/fakeroot'
    env = dict(env, KERNEL_SDKROOT=str(sdk),
               XNU_AVAILABILITY_PL=str(sdk / 'usr/local/libexec/availability.pl'))
    os_spec = profile['os']
    version = os_spec['ios_version']
    archives = [tree / 'build/corecrypto-2023/libcorecrypto_kernel_noasm-namespaced.a']
    forced = [tree / 'build/armdt-platform/libArmDTPlatform.a', tree / 'build/armcpu/libArmCpu.a',
              tree / 'build/marble-boot/libMarbleBoot.a']
    args = ['make', '-C', str(tree / '.cache/xnu-work'), 'build', 'SDKROOT=iphoneos',
            'ARCH_CONFIGS=ARM64', 'MACHINE_CONFIGS=BCM2837', 'KERNEL_CONFIGS=RELEASE',
            'ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64e', 'LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY',
            'ARM64_LINK_ADDR=0xfffffff007004000', 'ARM64_PLKSEG_ADDR=0xfffffff004004000',
            f'DEPLOYMENT_TARGET_FLAGS=-miphoneos-version-min={version} -DXNU_TARGET_OS_IOS',
            f'DEPLOYMENT_LINKER_FLAGS=-Wl,-ios_version_min,{version}',
            'RC_DARWIN_KERNEL_VERSION=' + os_spec['darwin'],
            'RC_ProjectSourceVersion=' + os_spec['xnu_version'],
            'BUILD_LTO=1', 'BOUND_CHECKS=0', 'BUILD_WERROR=0', 'MAKEJOBS=-j8',
            'XNU_KERNEL_CHAINED_FIXUPS=1',
            f'LDFLAGS_KERNEL_LTO=-miphoneos-version-min={version} -Wl,-mllvm,-inline-threshold=100',
            'CFLAGS_EXTRA=-Wno-reserved-identifier -Wno-pre-c11-compat '
            '-Wno-missing-designated-field-initializers -Wno-suggest-override '
            '-Wno-unnecessary-virtual-specifier -Wno-suggest-destructor-override '
            '-Wno-ms-bitfield-padding -DXNU_NATIVE_PAN=0 -DXNU_BOOT_PROGRESS=0 '
            '-DXNU_SOURCE_CORECRYPTO14=1 -DXNU_ARM_DT_PLATFORM=1 -DXNU_ARMCPU=1',
            'LD_KERNEL_ARCHIVES=' + ' '.join([f'-L{sdk}/usr/local/lib/kernel', '-lfirehose_kernel',
                                             *map(str, archives), *(f'-Wl,-force_load,{p}' for p in forced)]),
            'XNU_EXTERNAL_KERNEL_ARCHIVES=' + ' '.join(map(str, archives + forced)),
            f'OBJROOT={tree}/build/xnu/ios-release-obj',
            f'SYMROOT={tree}/build/xnu/ios-release-sym', f'DSTROOT={tree}/build/xnu/ios-release-dst']
    (out / 'release-command.json').write_text(json.dumps(args, indent=2) + '\n')
    with (out / 'release-build.log').open('w') as log:
        subprocess.run(args, cwd=tree, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    native = tree / 'build/xnu/ios-release-obj/RELEASE_ARM64_BCM2837/mach.release.bcm2837.unstripped.noctf'
    subprocess.run([sys.executable, tree / 'scripts/seal-source-crypto-kernel.py', native],
                   cwd=tree, env=env, check=True)
    output = out / 'kernel.native.macho'
    shutil.copyfile(native, output)
    return output


def fetch(out, upstream, profile):
    sys.path.insert(0, str(upstream / 'scripts'))
    from unixnu.ipsw import Ipsw, kernelcache
    ctx = SimpleNamespace(board=profile, out_root=out)
    source = Ipsw(ctx, spec=profile['ipsw']['url'])
    for component in profile['ipsw']['fetch']:
        source.extract(component)
    cache = kernelcache(ctx, source)
    with cache.open('rb') as stream:
        header = stream.read(32)
    _, cpu, subtype, kind = struct.unpack_from('<4I', header)
    if cpu != 0x100000c or subtype & 0xffffff != 2 or kind != 12:
        raise RuntimeError('Target kernelcache must be an arm64e MH_FILESET')
    return cache


def qualify_pmgr(out, profile):
    cache = out / 'ipsw' / profile['ipsw']['build'] / 'kernelcache.decompressed'
    raw = cache.read_bytes()
    if hashlib.sha256(raw).hexdigest() != profile['kernel']['prelink']['kernelcache_sha256']:
        raise RuntimeError('PMGR qualification requires the pinned original collection')
    def read(address, count):
        at = 32
        for _ in range(struct.unpack_from('<I', raw, 16)[0]):
            command, size = struct.unpack_from('<II', raw, at)
            if command == 0x19:
                virtual, _, offset, backed = struct.unpack_from('<4Q', raw, at + 24)
                if virtual <= address and address + count <= virtual + backed:
                    return raw[offset + address - virtual:offset + address - virtual + count]
            at += size
        raise RuntimeError('PMGR contract address is outside the collection')
    contracts = {
        0xfffffff00916b680: [0xd503245f, 0xf9404400, 0xb40001a0, 0xf9400010, 0xaa0003f1,
            0xf2f9b431, 0xdac11a30, 0xd2814311, 0x8b110210, 0xf9400208, 0xaa1003e1,
            0xaa0803f0, 0xaa0103f1, 0xf2e7fcb1, 0xd71f0a11, 0x12800000, 0xd65f03c0],
        0xfffffff0091695e4: [0xd503245f, 0xf9404400, 0xb40001a0, 0xf9400010, 0xaa0003f1,
            0xf2f9b431, 0xdac11a30, 0xd2814411, 0x8b110210, 0xf9400208, 0xaa1003e2,
            0xaa0803f0, 0xaa0203f1, 0xf2e3a5d1, 0xd71f0a11, 0xd65f03c0],
    }
    for address, words in contracts.items():
        if read(address, len(words) * 4) != struct.pack('<' + 'I' * len(words), *words):
            raise RuntimeError(f'Original absent-provider PMGR contract changed at {address:#x}')
    (out / 'pmgr-contract.json').write_text(json.dumps({
        'kernelcache': str(cache), 'profile': profile['os']['name'], 'provider_offset': '0x88',
        'read': 'original null-provider path returns UINT32_MAX',
        'write': 'original null-provider path returns without a hardware operation',
        'qualified_functions': [hex(a) for a in contracts],
    }, indent=2) + '\n')


def build_board(tree, env):
    record_path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(record_path.read_text().strip().rstrip(','))
    out = tree / 'build/marble-boot'
    out.mkdir(parents=True, exist_ok=True)
    objects = []
    with (out / 'compile.log').open('w') as log:
        for name in ('MarbleBootWatchdog', 'MarblePmgr'):
            obj = out / (name + '.o')
            command = []
            arguments = iter(record['arguments'])
            for argument in arguments:
                if argument == record['file']:
                    command.append(str(DEVICE / 'package' / (name + '.cpp')))
                elif argument in ('-o', '-MF', '-MT', '-MJ'):
                    next(arguments)
                    if argument == '-o':
                        command += ['-o', str(obj)]
                else:
                    command.append(argument)
            command += ['-Werror', '-Wno-c99-designator', '-Wno-missing-include-dirs']
            subprocess.run(command, cwd=record['directory'], env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            magic, cpu, subtype, kind = struct.unpack_from('<4I', obj.read_bytes())
            if (magic, cpu, subtype & 0xffffff, kind) != (0xfeedfacf, 0x100000c, 2, 1):
                raise RuntimeError('Marble boot archive must contain arm64e objects')
            objects.append(obj)
    archive = out / 'libMarbleBoot.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], env=env, check=True)


def read_adt(data, offset=0):
    count, children = struct.unpack_from('<II', data, offset)
    offset += 8
    properties, nodes = {}, []
    for _ in range(count):
        name, length = struct.unpack_from('<32sI', data, offset)
        offset += 36
        length &= 0x7fffffff
        properties[name.split(b'\0', 1)[0].decode()] = data[offset:offset + length]
        offset += (length + 3) & ~3
    for _ in range(children):
        child, offset = read_adt(data, offset)
        nodes.append(child)
    return (properties, nodes), offset


def write_adt(tree):
    properties, children = tree
    data = bytearray(struct.pack('<II', len(properties), len(children)))
    for name, value in properties.items():
        data += struct.pack('<32sI', name.encode(), len(value))
        data += value + bytes(-len(value) % 4)
    return bytes(data) + b''.join(write_adt(child) for child in children)


def validate_prelink(out):
    report = json.loads((out / 'kernel.fileset.json').read_text())
    if (not report.get('mapping_complete') or not report.get('candidate_written') or
            report.get('allow_missing') or any(report.get('stubbed', {}).values())):
        missing = sum(sum(component.values()) for component in report.get('missing', {}).values())
        raise RuntimeError(f'Prelink has {missing} unresolved references')
    kernel = out / 'kernel.fileset.macho'
    with kernel.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if digest != report['candidate_sha256']:
        raise RuntimeError('Kernel differs from the complete prelink report')
    native_digest = hashlib.sha256((out / 'kernel.native.macho').read_bytes()).hexdigest()
    if report['inputs']['native_sha256'] != native_digest:
        raise RuntimeError('Prelink differs from the current compiled kernel; rerun prelink')
    return report


def boot_image(out, upstream, stack, profile, unique_chip_id):
    from build_pac_uefi import build_applications, pack_boot_image
    report = validate_prelink(out)
    kernel = out / 'kernel.fileset.macho'
    if report['inputs']['kernelcache_sha256'] != profile['kernel']['prelink']['kernelcache_sha256']:
        raise RuntimeError('Kernel collection differs from the iOS 18 profile')
    if not unique_chip_id or not 0 < int(unique_chip_id, 16) < 1 << 64:
        raise RuntimeError('bootimg requires --unique-chip-id HEX or UNIXNU_UNIQUE_CHIP_ID')
    platform = DEVICE.parent / 'marble_hackios'
    boot = out / 'boot'
    boot.mkdir(exist_ok=True)
    sys.path.insert(0, str(upstream / 'scripts'))
    from unixnu.ipsw import im4p_payload
    cache = out / 'ipsw' / profile['ipsw']['build']
    paths = {}
    for filename, expected in profile['ipsw']['sha256'].items():
        if filename.startswith('kernelcache.'):
            continue
        path = cache / filename
        with path.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != expected:
                raise RuntimeError(f'Firmware component differs from the profile: {filename}')
        paths[filename] = path
    original = im4p_payload(next(path for name, path in paths.items() if name.startswith('DeviceTree.')).read_bytes())
    if original.startswith((b'bvx2', b'bvxn', b'bvx-')):
        packed, decoded = boot / 'apple-device-tree.lzfse', boot / 'apple-device-tree.adt'
        packed.write_bytes(original)
        subprocess.run(['/usr/bin/compression_tool', '-decode', '-a', 'lzfse', '-i', packed, '-o', decoded], check=True)
        original = decoded.read_bytes()
    apple, end = read_adt(original)
    if end != len(original) or apple[0]['model'].rstrip(b'\0').decode() != profile['ipsw']['product']:
        raise RuntimeError('Apple device tree differs from the selected product')
    binaries = build_applications(boot, platform, stack)
    stage = boot / 'stage/EFI/UniXNU'
    payloads = stage / 'Payloads'
    payloads.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(binaries / 'UniXNU.efi', stage / 'UniXNU.efi')
    shutil.copyfile(binaries / 'DarwinBoot.efi', payloads / 'DarwinBoot.efi')
    shutil.copyfile(kernel, payloads / 'kernel.macho')
    trust = boot / 'ios18.trustcache'
    caches = [path for name, path in paths.items() if name.endswith('.trustcache')]
    subprocess.run([sys.executable, upstream / 'scripts/bundle-ipsw-trust.py', trust, *caches], check=True)
    shutil.copyfile(trust, payloads / 'kernel.macho.trustcache')
    board = tomllib.loads((DEVICE / 'board.toml').read_text())
    adt = payloads / 'kernel.macho.adt'
    subprocess.run([sys.executable, DEVICE / 'scripts/make-marble-adt.py', '--output', adt,
                    '--dram-size', board['payload']['dram_size'],
                    '--owned-memory-size', board['payload']['adt']['owned_memory_size'],
                    '--cpu-regs', '0', '--unique-chip-id', unique_chip_id,
                    '--chip-id', f"{profile['ipsw']['chip_id']:x}", '--iphone-identity', '--trust-cache'], check=True)
    tree, end = read_adt(adt.read_bytes())
    tree[0]['model'], tree[0]['target-type'] = apple[0]['model'], apple[0]['target-type']
    original_product = next(node[0] for node in apple[1] if node[0].get('name') == b'product\0')
    product = next(node[0] for node in tree[1] if node[0].get('name') == b'product\0')
    for key in ('product-name', 'product-description', 'artwork-device-idiom', 'artwork-device-subtype',
                'artwork-scale-factor', 'artwork-display-gamut', 'compatible-device-fallback', 'thin-bezel'):
        if key in original_product:
            product[key] = original_product[key]
    product['compatible-device-fallback'] = apple[0]['model']
    chosen = next(node[0] for node in tree[1] if node[0].get('name') == b'chosen\0')
    chosen['display-scale'] = product['artwork-scale-factor']
    adt.write_bytes(write_adt(tree))
    arguments = ' '.join(profile['payload']['kernel_args'])
    text = (f'version = 1\n[boot]\ntimeout = 0\ndefault = "marble"\n[kits]\nautoload = false\n'
            '[[driver]]\npath = "Payloads/DarwinBoot.efi"\n[[entry]]\nid = "marble"\n'
            f'title = "UniXNU"\ntype = "xnu"\npath = "Payloads/kernel.macho"\nargs = "{arguments}"\n')
    (stage / 'unixnu.toml').write_text(text)
    volume = boot / 'volume.img'
    total = sum(path.stat().st_size for path in stage.rglob('*') if path.is_file())
    volume_bytes = ((total + 4 * 1024**2 - 1) // (2 * 1024**2)) * (2 * 1024**2)
    with volume.open('wb') as stream:
        stream.truncate(volume_bytes)
    subprocess.run(['mformat', '-i', volume, '-v', 'MARBLEESP', '::'], check=True)
    subprocess.run(['mcopy', '-i', volume, '-s', '-Q', stage.parent, '::/'], check=True)
    blobs = [('loader.efi', (binaries / 'MarbleBootstrap.efi').read_bytes()), ('volume.img', volume.read_bytes())]
    body = b''.join(struct.pack('<16sII', name.encode(), len(data), 0) + data + bytes(-len(data) % 16)
                    for name, data in blobs) + bytes(24)
    image = pack_boot_image(boot, platform, struct.pack('<II', 0x50554e58, len(body)) + body, 'marble_ios18_arm64e_boot.img')
    (boot / 'boot.json').write_text(json.dumps({'profile': profile['os']['name'], 'kernel': str(kernel),
        'prelink_complete': True, 'enabled_cpus': [0], 'kernel_args': profile['payload']['kernel_args'],
        'model': profile['ipsw']['product'], 'volume_bytes': volume_bytes, 'image': str(image)}, indent=2) + '\n')
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['fetch', 'kernel', 'prelink', 'bootimg'])
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--unique-chip-id', default=os.environ.get('UNIXNU_UNIQUE_CHIP_ID'))
    parser.add_argument('--upstream', type=Path, default=DEVICE.parent / 'upstream/UniXNU')
    parser.add_argument('--stack', type=Path, default=DEVICE.parent / 'UniXNU')
    args = parser.parse_args()
    out, upstream, stack = args.output.resolve(), args.upstream.resolve(), args.stack.resolve()
    out.mkdir(parents=True, exist_ok=True)
    profile = tomllib.loads(PROFILE.read_text())
    if args.command == 'fetch':
        print(fetch(out, upstream, profile))
        return
    tree, env = prepare(out, upstream, stack, profile)
    if args.command == 'bootimg':
        print(boot_image(out, upstream, stack, profile, args.unique_chip_id))
        return
    if args.command == 'kernel':
        record = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
        if not record.is_file():
            with (out / 'kernel-bootstrap.log').open('w') as log:
                subprocess.run(['bash', str(tree / 'scripts/build-xnu-4k-baseline.sh')],
                               cwd=tree, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        for name, arguments in [('corecrypto', ['build-corecrypto-14.py']),
                                ('corecrypto-namespace', ['build-corecrypto-14.py', '--namespace']),
                                ('platform', ['build-arm-driver.py', 'ArmDTPlatform']),
                                ('cpu', ['build-arm-driver.py', 'ArmCpu'])]:
            with (out / (name + '-build.log')).open('w') as log:
                subprocess.run([sys.executable, tree / 'scripts' / arguments[0], *arguments[1:]],
                               cwd=tree, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        env = dict(env, XNU_BOOTSTRAP='0', XNU_SOURCE_CORECRYPTO14='1',
                   XNU_ARM_DT_PLATFORM='1', XNU_ARMCPU='1',
                   XNU_EXTRA_LD_ARCHIVES=f'-Wl,-force_load,{tree}/build/marble-boot/libMarbleBoot.a')
        qualify_pmgr(out, profile)
        build_board(tree, env)
        with (out / 'kernel-build.log').open('w') as log:
            subprocess.run(['bash', str(tree / 'scripts/build-xnu-4k-baseline.sh')],
                           cwd=tree, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        print(release(tree, out, env, profile))
        return
    cache = fetch(out, upstream, profile)
    tool = out / 'XnuPrelink'
    checkout(tool, profile['upstream']['prelink_url'], profile['upstream']['prelink_commit'])
    apply_patch(tool, DEVICE / profile['upstream']['prelink_patch'])
    native = out / 'kernel.native.macho'
    output = out / 'kernel.fileset.macho'
    with tempfile.TemporaryDirectory(prefix='prelink-', dir=out) as temporary:
        candidate = Path(temporary) / output.name
        mapping = output.with_suffix('.map.json')
        if mapping.is_file():
            shutil.copyfile(mapping, candidate.with_suffix('.map.json'))
        command = [sys.executable, str(tool / 'emit_prelink.py'), '--kernel', str(native),
                   '--prelinked', str(cache), '--slide=-0x1000000', '--output', str(candidate)]
        for root in profile['kernel']['roots']:
            command += ['--root', root]
        if profile['kernel']['prelink'].get('early_security_init'):
            command += ['--early-security-init', profile['kernel']['prelink']['early_security_init']]
        with (out / 'prelink.log').open('w') as log:
            result = subprocess.run(command, cwd=tree, env=env, stdout=log, stderr=subprocess.STDOUT)
        if candidate.with_suffix('.map.json').is_file():
            candidate.with_suffix('.map.json').replace(mapping)
        if result.returncode:
            if candidate.with_suffix('.json').is_file():
                candidate.with_suffix('.json').replace(out / 'prelink-failure.json')
            raise SystemExit(f'prelink: {out / "prelink.log"}')
        report = json.loads(candidate.with_suffix('.json').read_text())
        report.update(output=str(output), map=str(mapping))
        candidate.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
        candidate.replace(output)
        candidate.with_suffix('.json').replace(output.with_suffix('.json'))
    print(output)


if __name__ == '__main__':
    main()
