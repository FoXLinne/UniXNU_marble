#!/usr/bin/env python3
"""Fetch iOS 18 ARM64e inputs, build XNU, and bind the original kernel collection."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
from types import SimpleNamespace

from marble_sources import stage_ios18_stack, stage_ios18, source_revision

DEVICE = Path(__file__).resolve().parents[1]
PROFILE = DEVICE / 'profiles/ios-18.7.11-22H384.toml'
ADSP_FIRMWARE = DEVICE.parent / 'UniXNU/.cache/firmware/marble/adsp2.mbn'
NATIVE_CFLAGS = ('-Wno-reserved-identifier -Wno-pre-c11-compat '
                 '-Wno-missing-designated-field-initializers -Wno-suggest-override '
                 '-Wno-unnecessary-virtual-specifier -Wno-suggest-destructor-override '
                 '-Wno-ms-bitfield-padding -DXNU_NATIVE_CS_HASH_CACHE=1')


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def checkout(path, url, revision, tag=None):
    name = url.rsplit('/', 1)[-1].removesuffix('.git')
    name = {'NativeRamBlock': 'RamBlock'}.get(name, name)
    if (DEVICE / 'ios18/sources' / name).is_dir():
        stage_ios18(name, path)
        return
    if not (path / '.git').exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        if tag:
            subprocess.run(['git', 'clone', '--depth', '1', '--branch', tag, url, path], check=True)
        else:
            subprocess.run(['git', 'init', '--quiet', path], check=True)
            subprocess.run(['git', '-C', path, 'remote', 'add', 'origin', url], check=True)
            subprocess.run(['git', '-C', path, 'fetch', '--depth', '1', 'origin', revision], check=True)
            subprocess.run(['git', '-C', path, 'checkout', '--quiet', '--detach', revision], check=True)


def apply_patch(work, patch):
    if Path(work).name in ('SoftwareAES', 'NativeRamBlock', 'XnuPrelink'):
        return
    applied = subprocess.run(['git', '-C', work, 'apply', '--reverse', '--check', patch],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if applied.returncode:
        subprocess.run(['git', '-C', work, 'apply', '--check', patch], check=True)
        subprocess.run(['git', '-C', work, 'apply', patch], check=True)


def prepare(out, upstream, stack, profile):
    stage_ios18_stack(stack)
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
        if target.is_symlink() and target.resolve() != (stack / name).resolve():
            target.unlink()
        if not target.exists():
            target.symlink_to(stack / name, target_is_directory=True)
    shutil.copytree(upstream / 'scripts', tree / 'scripts', dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('__pycache__'))
    apply_patch(tree, DEVICE / profile['upstream']['scripts_patch'])
    shutil.copyfile(upstream / 'components.toml', tree / 'components.toml')
    work = tree / '.cache/xnu-work'
    if not work.exists():
        subprocess.run(['git', 'clone', '--quiet', '--shared', reference, work], check=True)
    patch = DEVICE / profile['kernel']['patch']
    performance_patch = DEVICE / profile['kernel']['performance_patch']
    apply_patch(work, patch)
    apply_patch(work, DEVICE / profile['kernel']['gpu_patch'])
    ui_patch = DEVICE / profile['kernel']['ui_patch']
    apply_patch(work, ui_patch)
    filesystem_patch = DEVICE / profile['kernel']['filesystem_patch']
    apply_patch(work, filesystem_patch)
    rtc_patch = DEVICE / 'patches/xnu-11417/native-rtc.patch'
    apply_patch(work, rtc_patch)
    apply_patch(work, performance_patch)
    native_pac_el0_patch = DEVICE / 'patches/xnu-11417/native-pac-el0.patch'
    apply_patch(work, native_pac_el0_patch)
    dataprotect_patch = DEVICE / 'patches/xnu-11417/sheng-owner-dataprotect.patch'
    apply_patch(work, dataprotect_patch)
    console_patch = DEVICE / 'patches/xnu-11417/early-video-console.patch'
    apply_patch(work, console_patch)
    metal_cache_patch = DEVICE / 'patches/xnu-11417/metal-cache-access.patch'
    apply_patch(work, metal_cache_patch)
    cache_times_patch = DEVICE / 'patches/xnu-11417/metal-cache-times.patch'
    apply_patch(work, cache_times_patch)
    microtime_patch = DEVICE / 'patches/xnu-11417/fractional-mhz-microtime.patch'
    apply_patch(work, microtime_patch)
    msgbuf_patch = DEVICE / 'patches/xnu-11417/release-msgbuf-size.patch'
    apply_patch(work, msgbuf_patch)
    backlight_patch = DEVICE / 'ios18/source/patches/xnu/zzwb-native-sandbox-grant-set-properties.patch'
    apply_patch(work, backlight_patch)
    appsync_patch = DEVICE / 'patches/xnu-11417/appsync-user-trust.patch'
    apply_patch(work, appsync_patch)
    launch_constraints_patch = DEVICE / 'ios18/source/patches/xnu/zzyc-owner-skip-launch-constraints.patch'
    apply_patch(work, launch_constraints_patch)
    patch_list = out / 'xnu-patches.list'
    patch_list.write_text(str(patch) + '\n' + str(DEVICE / profile['kernel']['gpu_patch']) + '\n' + str(ui_patch) + '\n' + str(filesystem_patch) + '\n' + str(rtc_patch) + '\n' + str(performance_patch) + '\n' + str(native_pac_el0_patch) + '\n' + str(dataprotect_patch) + '\n' + str(console_patch) + '\n')
    with patch_list.open('a') as stream:
        stream.write(str(metal_cache_patch) + '\n' + str(cache_times_patch) + '\n' + str(microtime_patch) + '\n' + str(msgbuf_patch) + '\n' + str(backlight_patch) + '\n' + str(appsync_patch) + '\n' + str(launch_constraints_patch) + '\n')
    env = dict(os.environ, UNIXNU_XNU_TAG='xnu-' + os_spec['xnu_version'],
               UNIXNU_XNU_COMMIT=os_spec['xnu_revision'], UNIXNU_XNU_DARWIN=os_spec['darwin'],
               UNIXNU_XNU_VERSION=os_spec['xnu_version'], UNIXNU_XNU_ARCH=os_spec['arch'],
               UNIXNU_IOS_VERSION=os_spec['ios_version'], UNIXNU_DRIVERKIT_TAG=os_spec['driverkit_tag'],
               UNIXNU_PATCH_LIST=str(patch_list), XNU_BOOTSTRAP='1', XNU_KERNEL_CHAINED_FIXUPS='1',
               JOBS='2', PATH=os.pathsep.join([str(Path(sys.executable).parent),
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
              tree / 'build/marble-boot/libMarbleBoot.a', tree / 'build/marble-storage/libMarbleStorage.a',
              tree / 'build/marble-gpu/libMarbleGpu.a']
    args = ['make', '-C', str(tree / '.cache/xnu-work'), 'build', 'SDKROOT=iphoneos',
            'ARCH_CONFIGS=ARM64', 'MACHINE_CONFIGS=BCM2837', 'KERNEL_CONFIGS=RELEASE',
            'ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64e', 'LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY',
            'ARM64_LINK_ADDR=0xfffffff007004000', 'ARM64_PLKSEG_ADDR=0xfffffff004004000',
            f'DEPLOYMENT_TARGET_FLAGS=-miphoneos-version-min={version} -DXNU_TARGET_OS_IOS',
            f'DEPLOYMENT_LINKER_FLAGS=-Wl,-ios_version_min,{version}',
            'RC_DARWIN_KERNEL_VERSION=' + os_spec['darwin'],
            'RC_ProjectSourceVersion=' + os_spec['xnu_version'],
            'BUILD_LTO=1', 'BOUND_CHECKS=0', 'BUILD_WERROR=0', 'MAKEJOBS=-j2',
            'XNU_KERNEL_CHAINED_FIXUPS=1',
            f'LDFLAGS_KERNEL_LTO=-miphoneos-version-min={version} -Wl,-mllvm,-inline-threshold=100',
            'CFLAGS_EXTRA=' + NATIVE_CFLAGS + ' -DXNU_NATIVE_PAN=1 -DXNU_BOOT_PROGRESS=0 '
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
    subprocess.run(['xcrun', 'strip', '-S', '-o', output, native], check=True)
    build_capture_layout(tree, out)
    return output


def build_capture_layout(tree, out):
    directory = out / 'capture-layout'
    directory.mkdir(exist_ok=True)
    fields = {
        'proc': ('bsd/RELEASE/kern_exec.o.json', '#include <sys/proc_internal.h>\n', {
            'pid': 'offsetof(struct proc,p_pid)', 'state': 'offsetof(struct proc,p_stat)',
            'name': 'offsetof(struct proc,p_name)', 'next': 'offsetof(struct proc,p_list.le_next)'}),
        'thread': ('osfmk/RELEASE/startup.o.json', '#include <kern/task.h>\n#include <kern/thread.h>\n', {
            'task_threads_head': 'offsetof(struct task,threads)',
            'task_cache_slide': 'offsetof(struct task,task_shared_region_slide)',
            'task_image_info': 'offsetof(struct task,all_image_info_addr)',
            **{name: f'offsetof(struct thread,{name})' for name in
               ('task_threads', 'state', 'continuation', 'kernel_stack', 'wait_event')},
            **{name: f'offsetof(struct thread,machine)+offsetof(struct machine_thread,{name})'
               for name in ('upcb', 'kstackptr', 'jop_pid')},
            'bytes': 'sizeof(struct thread)'}),
        'user': ('osfmk/RELEASE/startup.o.json',
                 '#include <kern/task.h>\n#include <vm/vm_map_xnu.h>\n#include <arm/pmap/pmap.h>\n', {
            'task_map': 'offsetof(struct task,map)',
            'map_pmap': 'offsetof(struct _vm_map,pmap)',
            'pmap_ttep': 'offsetof(struct pmap,ttep)'})}
    result = {}
    for name, (record, includes, expressions) in fields.items():
        source, assembly = directory / (name + '.c'), directory / (name + '.ll')
        source.write_text('#include <stddef.h>\n' + includes +
                          'const unsigned long layout[] = {' + ','.join(expressions.values()) + '};\n')
        record = json.loads((tree / 'build/xnu/ios-release-obj/RELEASE_ARM64_BCM2837' / record)
                            .read_text().strip().rstrip(','))
        command, arguments = [], iter(record['arguments'])
        for value in arguments:
            if value == record['file']:
                command.append(str(source))
            elif value in ('-o', '-MF', '-MT', '-MJ'):
                next(arguments)
                if value == '-o':
                    command += ['-o', str(assembly)]
            elif value != '-c':
                command.append(value)
        with (directory / (name + '.log')).open('w') as log:
            subprocess.run(command + ['-S', '-emit-llvm', '-g0'], cwd=record['directory'],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        line = next(line for line in assembly.read_text().splitlines() if '@layout =' in line)
        values = [int(value) for value in re.findall(r'i64 (\d+)', line)]
        result[name] = dict(zip(expressions, values, strict=True))
    (out / 'kernel.layout.json').write_text(json.dumps(result, indent=2) + '\n')


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
    stage_ios18('GopFramebuffer', tree.parent / 'GopFramebuffer')
    wifi_module = stage_ios18('Ath12kWifi', tree.parent / 'wifi/Ath12kWifi')
    wifi_output = tree.parent / 'wifi/objects'
    with (tree.parent / 'wifi-build.log').open('w') as log:
        subprocess.run([sys.executable, DEVICE / 'scripts/build_marble_wifi.py', wifi_module, wifi_output],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    wifi_objects = json.loads((wifi_output / 'build-report.json').read_text())['objects']
    from build_ios18_display import generate, bindings as display_bindings
    from build_ios18_hid import prepare as prepare_hid, bindings as hid_bindings
    from build_ios18_usb import generate as generate_usb, bindings as usb_bindings
    records = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    out = tree / 'build/marble-boot'
    out.mkdir(parents=True, exist_ok=True)
    generate(out)
    generate_usb(out)
    hid_flags = prepare_hid(tree)
    adsp = out / 'MarbleAdsp.cpp'
    original = (DEVICE / 'package/MarbleAdsp.cpp').read_text()
    declaration = 'extern const uint8_t marble_adsp_firmware[];'
    if original.count(declaration) != 1:
        raise RuntimeError('ADSP firmware declaration changed')
    adsp.write_text(original.replace(declaration, 'extern const uint8_t *marble_adsp_firmware;'))
    resident = out / 'NativeResident.cpp'
    original_resident = (DEVICE / 'package/NativeResident.cpp').read_text()
    resident_source = original_resident.replace('  native_root_block_export_data();\n', '')
    delay_start = resident_source.index('  uint64_t start, now, frequency;')
    delay_end = resident_source.index('  // The resident firmware resumes', delay_start)
    resident_source = resident_source[:delay_start] + resident_source[delay_end:]
    resident.write_text(resident_source)
    panel_source=(DEVICE / 'package/MarblePkg/RootDisplay.c').read_text()
    panel_source=panel_source[panel_source.index('STATIC VOID PanelPublishState('):
                              panel_source.index('/* Prefix ABI')]
    panel_source=panel_source.replace('(VOID *)(UINTN)0xae94000','nativeDsi')
    panel_source=panel_source.replace('(VOID *)(UINTN)0xae00000','nativeMdp')
    panel_source=panel_source.replace('(CONST VOID *)(UINTN)0xae94004',
                                      '(CONST VOID *)(nativeDsi+1)')
    panel_source=panel_source.replace('PANEL_MODE_COMMIT *Commit=Context;',
                                      'PANEL_MODE_COMMIT *Commit=(PANEL_MODE_COMMIT *)Context;')
    panel_source=panel_source.replace('gBS->Stall(', 'NativePanelDelay(')
    (out / 'MarblePanelCommands.inc').write_text(panel_source)
    objects = []
    with (out / 'compile.log').open('w') as log:
        sources = [DEVICE / 'package' / filename for filename in
                   ('MarbleBootWatchdog.cpp', 'MarblePmgr.cpp', 'MarbleRtc.cpp', 'MarbleCpu.cpp', 'MarbleSystemOS.c', 'MarbleMetal.c', 'MarbleUiPolicy.c', 'MarbleSetupPolicy.c', 'MarbleAppPublication.c', 'RootWatchdogPolicy.c', 'MarbleDisplay.cpp', 'MarbleScanout.cpp', 'MarblePanel.cpp', 'MarbleBacklight.cpp',
                    'QcomSmem.c', 'QcomSmp2p.c', 'QcomGlink.c', 'QcomQmp.c', 'QcomRpmh.c',
                    'QcomMdt.c', 'QcomQrtr.c', 'MarbleQrtr.c', 'MarbleBattmgr.c', 'MarbleAdsp.cpp',
                    'MarbleBattery.cpp', 'MarbleBatteryProbe.cpp', 'MarbleCharging.cpp', 'MarbleWifi.cpp', 'MarbleWifiRuntime.cpp', 'MarbleWifiFirmware.c', 'MarbleRuntime18.cpp', 'RuntimeUsb.cpp', 'MarbleUsb.cpp', 'GeniI2c.c', 'NativeResident.cpp', 'MarbleNativePanel.cpp',
                    'MarbleHid18.cpp', 'NativeHomeInput.cpp', 'MarbleTouch.cpp', 'MarbleButtons.cpp', 'MarbleHaptics.cpp', 'KeyStoreEvents.cpp')]
        sources.append(tree.parent / 'SoftwareAES/NativeAES.cpp')
        sources.append(tree.parent / 'AppleKeyStoreState/NativeKeyStoreEvents.cpp')
        sources.append(tree.parent / 'GopFramebuffer/NativeLegacyDisplay.cpp')
        sources.append(DEVICE / 'ios18/sources/QcomGeniSpi/GeniSpi.c')
        for source in sources:
            filename = source.name
            if filename == 'MarbleAdsp.cpp':
                source = adsp
            elif filename == 'NativeResident.cpp':
                source = resident
            record_path = records / ('bsd/DEVELOPMENT/kern_exec.o.json' if source.suffix == '.c'
                                     else 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
            if filename in ('MarbleMetal.c', 'MarbleUiPolicy.c'):
                release_record = tree / 'build/xnu/ios-release-obj/RELEASE_ARM64_BCM2837/bsd/RELEASE/kern_exec.o.json'
                if release_record.is_file():
                    record_path = release_record
            record = json.loads(record_path.read_text().strip().rstrip(','))
            obj = out / (source.stem + '.o')
            command = []
            arguments = iter(record['arguments'])
            for argument in arguments:
                if argument == record['file']:
                    command.append(str(source))
                elif argument in ('-o', '-MF', '-MT', '-MJ'):
                    next(arguments)
                    if argument == '-o':
                        command += ['-o', str(obj)]
                else:
                    command.append(argument)
            strict = filename in ('MarbleBootWatchdog.cpp', 'MarblePmgr.cpp', 'MarbleSystemOS.c',
                                  'MarbleDisplay.cpp', 'MarblePower18.cpp', 'NativeAES.cpp', 'NativeLegacyDisplay.cpp')
            command += ['-Werror' if strict else '-Wno-error', '-Wno-c99-designator', '-Wno-missing-include-dirs']
            command += ['-fno-sanitize=signed-integer-overflow', '-DMARBLE_IOS18_GPU=1']
            if filename in ('MarbleMetal.c', 'MarbleUiPolicy.c'):
                command += ['-UDEVELOPMENT', '-DDEVELOPMENT=0', '-UDEBUG', '-DDEBUG=0', '-fno-lto']
            command += ['-I', str(out), '-Wno-unused-parameter', '-Wno-deprecated-enum-enum-conversion']
            command += ['-I', str(tree / '.cache/xnu-work/osfmk')]
            command += ['-I', str(DEVICE / 'package')]
            command += ['-I', str(wifi_module / 'include')]
            command += ['-I', str(DEVICE / 'ios18/source/package/UniXNUPkg/Include')]
            command += ['-I', str(DEVICE / 'ios18/sources/QcomGeniSpi')]
            if filename == 'RuntimeUsb.cpp':
                command += ['-DMARBLE_IOS18=1']
            if filename == 'NativeHomeInput.cpp':
                command += hid_flags
            if source.suffix == '.c':
                command += ['-Wno-comma', '-Wno-implicit-fallthrough']
            if filename in ('MarbleDisplay.cpp', 'NativeLegacyDisplay.cpp'):
                command += ['-Wno-missing-prototypes', '-Wno-cast-align', '-Wno-atomic-implicit-seq-cst']
            subprocess.run(command, cwd=record['directory'], env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            magic, cpu, subtype, kind = struct.unpack_from('<4I', obj.read_bytes())
            if (magic, cpu, subtype & 0xffffff, kind) != (0xfeedfacf, 0x100000c, 2, 1):
                raise RuntimeError('Marble boot archive must contain arm64e objects')
            if filename in ('MarbleBootWatchdog.cpp', 'NativeAES.cpp', 'NativeLegacyDisplay.cpp', 'MarbleBattery.cpp', 'MarbleBacklight.cpp', 'NativeHomeInput.cpp', 'NativeKeyStoreEvents.cpp', 'MarbleUsb.cpp'):
                late = obj.with_suffix('.late.o')
                subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-objcopy',
                                '--remove-section=__DATA,__mod_init_func',
                                '--globalize-symbol=__GLOBAL__sub_I_' + filename, obj, late], check=True)
                obj = late
            objects.append(obj)
    objects.extend(Path(unit['path']) for unit in wifi_objects)
    assembly, obj = out / 'adsp-firmware.S', out / 'adsp-firmware.o'
    assembly.write_text('.section __DATA_CONST,__const\n.p2align 3\n'
                        '.globl _marble_adsp_firmware_size\n_marble_adsp_firmware_size:\n.quad ' +
                        str(ADSP_FIRMWARE.stat().st_size) + '\n')
    subprocess.run(['xcrun', 'clang', '--target=arm64e-apple-ios18.0', '-c', assembly, '-o', obj], check=True)
    objects.append(obj)
    bindings = {
        '__ZN10IOWatchdogC2EPK11OSMetaClass': 0xfffffff0087f1ce4,
        '__ZN10IOWatchdogD2Ev': 0xfffffff0087f1d18,
        '__ZN10IOWatchdog4stopEP9IOService': 0xfffffff0087f26e0,
        '__ZN10IOWatchdog13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient': 0xfffffff0087f2a04,
        '_marble_original_watchdog_start': 0xfffffff0087f1dc0,
        '_marble_original_watchdog_properties': 0xfffffff0087f2364,
    }
    bindings.update(aes_bindings(tree.parent, out / 'NativeAES.o'))
    bindings.update(display_bindings(out / 'NativeLegacyDisplay.o', out))
    bindings.update(hid_bindings(tree.parent, out / 'NativeHomeInput.o'))
    usb_functions, usb_data, usb_aliases = usb_bindings(out / 'MarbleUsb.o')
    bindings.update(usb_functions)
    native_aliases = {
        '__ZN11IOHIDDevice12didTerminateEP9IOServicejPb': '__ZN9IOService12didTerminateEPS_jPb',
    }
    native_aliases.update(usb_aliases)
    for name in native_aliases:
        bindings.pop(name, None)
    text, addresses = '.text\n.p2align 2\n', ''
    for name, target in native_aliases.items():
        text += f'.globl {name}\n{name}:\n bti c\n b {target}\n'
    for name, address in bindings.items():
        addresses += f'.globl {name}.original\n.set {name}.original, {address:#x}\n'
        text += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
    addresses += '.globl __ZN10IOWatchdog10gMetaClassE\n.set __ZN10IOWatchdog10gMetaClassE, 0xfffffff00a37e358\n'
    addresses += '.globl __ZTV10IOWatchdog\n.set __ZTV10IOWatchdog, 0xfffffff007ac9e90\n'
    addresses += '.globl __ZN16IOAESAccelerator10gMetaClassE\n.set __ZN16IOAESAccelerator10gMetaClassE, 0xfffffff00a50e5f8\n'
    addresses += '.globl __ZTV16IOAESAccelerator\n.set __ZTV16IOAESAccelerator, 0xfffffff007d18c88\n'
    addresses += '.globl __ZN25IOMobileFramebufferLegacy10gMetaClassE\n.set __ZN25IOMobileFramebufferLegacy10gMetaClassE, 0xfffffff00a518d80\n'
    addresses += '.globl __ZTV25IOMobileFramebufferLegacy\n.set __ZTV25IOMobileFramebufferLegacy, 0xfffffff007d4ac30\n'
    addresses += '.globl __ZN11IOHIDDevice10gMetaClassE\n.set __ZN11IOHIDDevice10gMetaClassE, 0xfffffff00a510e68\n'
    addresses += '.globl __ZTV11IOHIDDevice\n.set __ZTV11IOHIDDevice, 0xfffffff007d37368\n'
    for name, address in usb_data.items():
        addresses += f'.globl {name}\n.set {name}, {address:#x}\n'
    for name, source in (('watchdog-bindings', text), ('watchdog-addresses', addresses)):
        assembly, obj = out / (name + '.S'), out / (name + '.o')
        assembly.write_text(source)
        subprocess.run(['xcrun', 'clang', '--target=arm64e-apple-ios18.0', '-c', assembly, '-o', obj], check=True)
        objects.append(obj)
    archive = out / 'libMarbleBoot.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], env=env, check=True)


def aes_bindings(out, obj):
    sys.path.insert(0, str(out / 'XnuPrelink'))
    from kc_image import Image
    cache = Image((out / 'ipsw/22H384/kernelcache.decompressed').read_bytes())
    fixups = cache.fixups()
    raw = obj.read_bytes()
    sections, symbols, position = [], [], 32
    for _ in range(struct.unpack_from('<I', raw, 16)[0]):
        command, length = struct.unpack_from('<II', raw, position)
        if command == 0x19:
            for index in range(struct.unpack_from('<I', raw, position + 64)[0]):
                section = position + 72 + index * 80
                va, size, fileoff = struct.unpack_from('<QQI', raw, section + 32)
                reloc, count = struct.unpack_from('<II', raw, section + 56)
                sections.append((va, size, fileoff, reloc, count))
        elif command == 2:
            symoff, count, stroff, _ = struct.unpack_from('<4I', raw, position + 8)
            for index in range(count):
                string, _, _, _, value = struct.unpack_from('<IBBHQ', raw, symoff + index * 16)
                symbols.append((raw[stroff + string:raw.index(b'\0', stroff + string)].decode(), value))
        position += length
    vtable = next(value for name, value in symbols if name == '__ZTV9NativeAES')
    entries = {}
    for va, size, fileoff, reloc, count in sections:
        if not va <= vtable < va + size:
            continue
        for index in range(count):
            offset, bits = struct.unpack_from('<II', raw, reloc + index * 8)
            if bits >> 27 & 1 and bits >> 28 == 11 and bits >> 25 & 3 == 3:
                word = struct.unpack_from('<Q', raw, fileoff + offset)[0]
                entries[va + offset] = (symbols[bits & 0xffffff][0], word)
    bindings = {'__ZN16IOAESAcceleratorC2EPK11OSMetaClass': 0xfffffff00994c570,
                '__ZN16IOAESAcceleratorD2Ev': 0xfffffff00994c5d8}
    for slot in range(181):
        name, word = entries[vtable + 16 + slot * 8]
        target, auth, key, diversity, address = fixups[0xfffffff007d18c98 + slot * 8]
        if (auth, key, address) != (1, 0, 1) or word >> 32 & 0xffff != diversity:
            raise RuntimeError(f'22H384 AES virtual slot {slot} authentication differs')
        if '16IOAESAccelerator' in name:
            bindings[name] = target
    if vtable + 16 + 181 * 8 in entries:
        raise RuntimeError('22H384 AES virtual table size differs')
    return bindings


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
    if report.get('base_kernelcache_sha256', report['inputs']['kernelcache_sha256']) != profile['kernel']['prelink']['kernelcache_sha256']:
        raise RuntimeError('Kernel collection differs from the iOS 18 profile')
    if not unique_chip_id or not 0 < int(unique_chip_id, 16) < 1 << 64:
        raise RuntimeError('bootimg requires --unique-chip-id HEX or UNIXNU_UNIQUE_CHIP_ID')
    platform = DEVICE / 'firmware'
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
    gpu = json.loads((out / 'gpu/userspace.json').read_text())
    owner = out / 'gpu/owner.trustcache'
    addon = out / 'gpu/metal.gpuaddon'
    if hashlib.sha256(owner.read_bytes()).hexdigest() != gpu['trust_cache_sha256'] or \
            hashlib.sha256(addon.read_bytes()).hexdigest() != gpu['addon_sha256']:
        raise RuntimeError('GPU payload differs from its signed userspace build')
    subprocess.run([sys.executable, upstream / 'scripts/bundle-ipsw-trust.py', trust, *caches, owner], check=True)
    shutil.copyfile(trust, payloads / 'kernel.macho.trustcache')
    shutil.copyfile(addon, payloads / 'kernel.macho.gpuaddon')
    board = tomllib.loads((DEVICE / 'board.toml').read_text())
    adt = payloads / 'kernel.macho.adt'
    subprocess.run([sys.executable, DEVICE / 'scripts/make-marble-adt.py', '--output', adt,
                    '--dram-size', board['payload']['dram_size'],
                    '--owned-memory-size', profile['payload']['owned_memory_size'],
                    '--root-match-volume-uuid', profile['payload']['root_volume_uuid'],
                    '--cpu-regs', '0', '--unique-chip-id', unique_chip_id,
                    '--chip-id', f"{profile['ipsw']['chip_id']:x}", '--iphone-identity', '--trust-cache', '--gpu-addon'], check=True)
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
    parser.add_argument('--source', '--upstream', dest='upstream', type=Path,
                        default=DEVICE / 'ios18/source')
    parser.add_argument('--stack', type=Path, default=None)
    args = parser.parse_args()
    out, upstream = args.output.resolve(), args.upstream.resolve()
    stack = (args.stack or out / 'stack').resolve()
    out.mkdir(parents=True, exist_ok=True)
    for variable, directory in (('TMPDIR', 'tmp'), ('CLANG_MODULE_CACHE_PATH', 'clang-module-cache'),
                                ('XDG_CACHE_HOME', 'host-cache')):
        path = out / directory
        path.mkdir(exist_ok=True)
        os.environ[variable] = str(path)
    tempfile.tempdir = os.environ['TMPDIR']
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
                   XNU_ARM_DT_PLATFORM='1', XNU_ARMCPU='1', XNU_NATIVE_PAN='1', XNU_NATIVE_CS_HASH_CACHE='1',
                   XNU_EXTRA_CFLAGS=NATIVE_CFLAGS,
                   XNU_EXTRA_LD_ARCHIVES=f'-Wl,-force_load,{tree}/build/marble-boot/libMarbleBoot.a '
                                        f'-Wl,-force_load,{tree}/build/marble-storage/libMarbleStorage.a '
                                        f'-Wl,-force_load,{tree}/build/marble-gpu/libMarbleGpu.a')
        checkout(out / 'SoftwareAES', 'https://github.com/UniXNU/SoftwareAES.git',
                 source_revision('SoftwareAES'))
        checkout(out / 'AppleKeyStoreState', 'https://github.com/UniXNU/AppleKeyStoreState.git',
                 source_revision('AppleKeyStoreState'))
        apply_patch(out / 'SoftwareAES', DEVICE / 'patches/SoftwareAES/arm64e-base.patch')
        checkout(out / 'GopFramebuffer', 'https://github.com/UniXNU/GopFramebuffer.git',
                 source_revision('GopFramebuffer'))
        checkout(out / 'IOHIDFamily', 'https://github.com/apple-oss-distributions/IOHIDFamily.git',
                 'e31e90a24c27ad6cbe981650d12a1a4124065b03', 'IOHIDFamily-2238.120.5')
        from build_ios18_gpu import kernel as build_gpu
        build_gpu(out)
        build_board(tree, env)
        storage = out / 'IOStorageFamily-323'
        checkout(storage, 'https://github.com/apple-oss-distributions/IOStorageFamily.git',
                 '1b96f8f04b9dff4dc0277ac43625bf694e62c5aa', 'IOStorageFamily-323')
        ram = out / 'NativeRamBlock'
        checkout(ram, 'https://github.com/UniXNU/NativeRamBlock.git',
                 source_revision('RamBlock'))
        apply_patch(ram, DEVICE / 'patches/NativeRamBlock/data-snapshot.patch')
        from build_ios18_storage import build as build_storage
        build_storage(tree, storage, ram)
        with (out / 'kernel-build.log').open('w') as log:
            subprocess.run(['bash', str(tree / 'scripts/build-xnu-4k-baseline.sh')],
                           cwd=tree, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        print(release(tree, out, env, profile))
        return
    cache = out / 'ipsw' / profile['ipsw']['build'] / 'kernelcache.decompressed'
    if not cache.is_file():
        cache = fetch(out, upstream, profile)
    tool = out / 'XnuPrelink'
    checkout(tool, profile['upstream']['prelink_url'], profile['upstream']['prelink_commit'])
    apply_patch(tool, DEVICE / profile['upstream']['prelink_patch'])
    native = out / 'kernel.native.macho'
    output = out / 'kernel.fileset.macho'
    if 'com.apple.driver.AppleUSBXDCI' in profile['kernel']['roots']:
        from build_ios18_usb import prelink
        initializers = {}
        if profile['kernel']['prelink'].get('early_security_init'):
            name, value = profile['kernel']['prelink']['early_security_init'].split('=')
            initializers[name] = int(value, 0)
        print(prelink(out, cache, native, profile['kernel']['roots'], initializers))
        return
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
