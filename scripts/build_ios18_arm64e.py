#!/usr/bin/env python3
"""Fetch iOS 18 ARM64e inputs, build XNU, and bind the original kernel collection."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
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


def build_board(tree, env):
    record_path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(record_path.read_text().strip().rstrip(','))
    out = tree / 'build/marble-boot'
    out.mkdir(parents=True, exist_ok=True)
    obj = out / 'MarbleBootWatchdog.o'
    command = []
    arguments = iter(record['arguments'])
    for argument in arguments:
        if argument == record['file']:
            command.append(str(DEVICE / 'package/MarbleBootWatchdog.cpp'))
        elif argument in ('-o', '-MF', '-MT', '-MJ'):
            next(arguments)
            if argument == '-o':
                command += ['-o', str(obj)]
        else:
            command.append(argument)
    command += ['-Werror', '-Wno-c99-designator', '-Wno-missing-include-dirs']
    with (out / 'compile.log').open('w') as log:
        subprocess.run(command, cwd=record['directory'], env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    magic, cpu, subtype, kind = struct.unpack_from('<4I', obj.read_bytes())
    if (magic, cpu, subtype & 0xffffff, kind) != (0xfeedfacf, 0x100000c, 2, 1):
        raise RuntimeError('Marble boot archive must contain arm64e objects')
    archive = out / 'libMarbleBoot.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['fetch', 'kernel', 'prelink'])
    parser.add_argument('--output', required=True, type=Path)
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
    command = [sys.executable, str(tool / 'emit_prelink.py'), '--kernel', str(native),
               '--prelinked', str(cache), '--slide=-0x1000000', '--output', str(output)]
    for root in profile['kernel']['roots']:
        command += ['--root', root]
    with (out / 'prelink.log').open('w') as log:
        result = subprocess.run(command, cwd=tree, env=env, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'prelink: {out / "prelink.log"}')
    print(output)


if __name__ == '__main__':
    main()
