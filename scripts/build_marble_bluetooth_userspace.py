#!/usr/bin/env python3
"""Build the 22H384 BlueTool adapter and update the candidate's launchd cache."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import subprocess
import zlib

DEVICE = Path(__file__).resolve().parents[1]
CACHE_PATH = 'xpc/launchd.plist'
DAEMON_SHA256 = 'd6b84dc749bc1d7f0ddccfa698db5a420bd3b325b47008852754d2d3d2797e38'
DAEMON_PATCHES = (
    # Use the final factory block when the product table does not list this board.
    (0x3ABA2C, 0x340002A0, 0xD503201F),
    # The last block invokes the native constructor with the generic controller type.
    (0x3ABA40, 0x5280FA41, 0x52800001),
)


def branch(source, target, link=False):
    delta = target - source
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError('Invalid AArch64 branch')
    return (0x94000000 if link else 0x14000000) | ((delta >> 2) & 0x03ffffff)


def adrp(source, target, register):
    pages = (target >> 12) - (source >> 12)
    if not -(1 << 20) <= pages < (1 << 20):
        raise ValueError('Invalid AArch64 page displacement')
    pages &= 0x1fffff
    return 0x90000000 | ((pages & 3) << 29) | ((pages >> 2) << 5) | register


def optional_command_patches(image):
    descriptor = 0xa3c578 + 24 * 0x7c
    if struct.unpack_from('<HH', image, descriptor + 2) != (0x0c5b, 0x1208):
        raise ValueError('Unexpected Write Default Erroneous Data Reporting command descriptor')
    # Move the optional step's entry check into its own verbose error log block.
    # Its original failure path still forwards the actual error to chain completion.
    entry, cave = 0x1d89d8, 0x1d8a14
    supported_commands = 0xa5f498
    chain_context, chain_next = 0xa492d8, 0x2999bc
    capability = supported_commands + 18
    original = (0x528005c1, 0x97f98fae, 0x34000140, 0xd00035a0, 0x912dd000,
                0x97f9944f, 0x528005c0, 0x97f99137, 0xaa0003f4)
    replacement = (
        adrp(cave, capability, 8),
        0x39400108 | ((capability & 0xfff) << 10),  # ldrb w8, [x8, #offset]
        0x36180068,  # tbz w8, #3, unsupported
        0xd100c3ff,  # original frame setup
        branch(cave + 16, entry + 4),
        adrp(cave + 20, chain_context, 8),
        0xf9400100 | (((chain_context & 0xfff) // 8) << 10),
        0xd50323ff,  # authenticate the saved LR before tail-calling the native chain
        branch(cave + 32, chain_next),
    )
    # The H4 table has no optional pre-stop callback. Keep its regular close
    # operation and any valid pre-stop callbacks for other transports.
    pre_stop = 0x1b2a10
    guard = cave + 36
    empty_delta = (pre_stop - 4) - (guard + 4)
    if empty_delta % 4 or not -(1 << 20) <= empty_delta < (1 << 20):
        raise ValueError('Invalid H4 pre-stop branch')
    original += (0x52800201, 0x9417d3a4, 0x35000120)
    replacement += (0xf9403100, 0xb4000000 | ((empty_delta >> 2) & 0x7ffff) << 5,
                    branch(guard + 8, pre_stop + 4))
    return ((entry, 0xD100C3FF, branch(entry, cave)),
            (pre_stop, 0xf9403100, branch(pre_stop, guard)),
            (cave - 4, 0x52800040, branch(cave - 4, 0x1d8a44)),
            *((cave + index * 4, before, after)
              for index, (before, after) in enumerate(zip(original, replacement, strict=True))))


def extended_scan_patches(image, experimental_advertising=False):
    descriptor_table = 0xa3c578
    command_descriptors = (
        (0xcc, 0x2036, 0x2404), (0xcd, 0x2037, 0x2408),
        (0xce, 0x2038, 0x2410), (0xcf, 0x2039, 0x2420),
        (0xd0, 0x203a, 0x2440), (0xd1, 0x203b, 0x2480),
        (0xd7, 0x2041, 0x2520), (0xd8, 0x2042, 0x2540),
        (0xd9, 0x2043, 0x2580),
    )
    for index, opcode, bit in command_descriptors:
        if struct.unpack_from('<HH', image, descriptor_table + 24 * index + 2) != (opcode, bit):
            raise ValueError('Unexpected LE extended command descriptor')
    # Share the controller's command bits between the native scan configuration
    # and the lower scan engine. Repurpose a verbose capability log block.
    cave = 0x50c8c4
    capability = 0xa5f498 + 37
    original = (0xb9400be8, 0x7100011f, 0x900016e8, 0xfd44c100, 0x9a9c1348, 0xbd0093e0)
    replacement = (
        adrp(cave, capability, 8),
        0x39400108 | ((capability & 0xfff) << 10),
        0x121b0508,  # and w8, w8, #0x60
        0x7101811f,  # cmp w8, #0x60
        0x1a9f17e0,  # cset w0, eq
        0xd65f03c0,
    )
    # Publish the engine flag after the native Read Local Supported Commands
    # completion updates its bitmap. The configuration setter can run earlier.
    publish = cave + 24
    complete = 0x1d8c58
    scan_flag = 0xa54a06
    original += (0xf80943e8, 0x90ffd7a0, 0x91000000, 0xb00022a3, 0x912b3863,
                 0x910243e4, 0xaa1603e1, 0x52800002, 0x52800185)
    # The controller uses one LE command family for scanning, initiating and
    # advertising. Publish the native selector flags from actual command bits.
    # The capability block runs before the controller initialization callback.
    family = 0x50c870
    replacement += (adrp(publish, capability, 8), replacement[1], *replacement[2:5],
                    adrp(publish + 20, scan_flag, 8), 0x39000100 | ((scan_flag & 0xfff) << 10),
                    branch(publish + 28, family), 0xD503201F)
    family_original = (
        0xB9401FE8, 0x7100011F, 0x900016E8, 0xFD44C100,
        0x9A9C1348, 0xBD0093E0, 0xF80943E8, 0x90FFD7A0,
        0x91000000, 0xB00022A3, 0x912A3C63, 0x910243E4,
        0xAA1603E1, 0x52800002, 0x52800185, 0x940AFEB8,
    )
    # Keep the initiating selector independent of the experimental broadcaster.
    # Normal LE peripherals need extended initiation when extended scan is active.
    if not experimental_advertising:
        # Both HCI initialization and the later platform configuration must
        # compute the same selector. The platform setter otherwise replaces it
        # with the generic Apple controller's board-specific default.
        initiate = family + 36
        family_replacement = (
            adrp(family, capability, 8),
            0x3952F509,  # ldrb w9, [x8, #0x4bd] (commands byte 37)
            0x121B0929,  # and w9, w9, #0xe0 (scan parameters, enable, initiate)
            0x7103813F,  # cmp w9, #0xe0
            0x1A9F17E9,  # cset w9, eq
            adrp(family + 20, 0xa4fa4a, 10),
            0x39292949,  # strb w9, [x10, #0xa4a] (LE initiating selector)
            adrp(family + 28, 0x88e000, 1),
            branch(family + 32, complete + 4),
            adrp(initiate, capability, 8),
            0x3952F508,  # ldrb w8, [x8, #0x4bd]
            0x121B0908,  # and w8, w8, #0xe0
            0x7103811F,  # cmp w8, #0xe0
            0x1A9F17E0,  # cset w0, eq
            branch(initiate + 20, 0x25c9cc),  # native initiating setter; preserve caller LR
            0xD503201F,
        )
        advertising_patches = ((0x50c954, branch(0x50c954, 0x25c9cc, link=True), branch(0x50c954, initiate, link=True)),)
    else:
        family_replacement, advertising_patches = experimental_advertising_patches(family, capability, complete)
    return ((complete, 0xd00035a1, branch(complete, publish)),
            *advertising_patches,
            (family - 4, 0x34000240, branch(family - 4, 0x50c8b4)),
            *((family + index * 4, before, after)
              for index, (before, after) in enumerate(zip(family_original, family_replacement, strict=True))),
            (cave - 4, 0x34000220, branch(cave - 4, 0x50c904)),
            (0x50c964, 0x12000260, branch(0x50c964, cave, link=True)),
            (0x40aa10, 0x37000360, branch(0x40aa10, 0x40aa7c)),
            (0x40aaa8, 0xd73f0911, branch(0x40aaa8, cave, link=True)),
            *((cave + index * 4, before, after)
              for index, (before, after) in enumerate(zip(original, replacement, strict=True))))


def experimental_advertising_patches(family, capability, complete):
    # The board supplies no product-specific advertising-set count. The native
    # standard advertising path needs one instance and assigns it handle zero.
    # A controller that advertises the complete extended command set supports
    # at least that instance. Keep any count selected by the product profile.
    advertising = 0x50c6e8
    advertising_original = (0x900016E8, 0xFD44C900, 0xBD0093E0, 0xB90097F6,
                            0x90FFD7A0, 0x91000000, 0xB00022A3)
    advertising_replacement = (
        0x392E0540,  # strb w0, [x10, #0xb81] (native extended advertising selector)
        adrp(advertising + 4, 0xa4a9b0, 10),
        0x3966C149,  # ldrb w9, [x10, #0x9b0]
        0x35000049,  # cbnz w9, keep the product profile's count
        0x3926C140,  # strb w0, [x10, #0x9b0] (one advertising set)
        branch(advertising + 20, family + 56),
        0xD503201F,
    )
    family_replacement = (
        adrp(family, capability, 8),
        0x3952F509,  # ldrb w9, [x8, #0x4bd]  (commands byte 37)
        0x121B0929,  # and w9, w9, #0xe0      (scan params, enable and initiate)
        0x7103813F,  # cmp w9, #0xe0
        0x1A9F17E9,  # cset w9, eq
        adrp(family + 20, 0xa4fa4a, 10),
        0x39292949,  # strb w9, [x10, #0xa4a] (native LE initiating selector)
        0x340000E0,  # cbz w0, restore original instruction (extended scan off)
        0x3952F109,  # ldrb w9, [x8, #0x4bc]  (commands byte 36)
        0x121E1529,  # and w9, w9, #0xfc       (extended advertising commands)
        0x7103F13F,  # cmp w9, #0xfc
        0x54000061,  # b.ne restore original instruction
        adrp(family + 48, 0xa4eb81, 10),
        branch(family + 52, advertising),
        adrp(family + 56, 0x88e000, 1),
        branch(family + 60, complete + 4),
    )
    return family_replacement, (
        (advertising - 4, 0x340001C0, branch(advertising - 4, 0x50c71c)),
        *((advertising + index * 4, before, after)
          for index, (before, after) in enumerate(zip(advertising_original, advertising_replacement, strict=True))),
    )


def launchd_cache(addon):
    data = addon.read_bytes()
    if data[:8] != b'NGPUADD2' or struct.unpack_from('<Q', data, 16)[0] != len(data):
        raise ValueError('Invalid baseline addon')
    at = 24
    for _ in range(struct.unpack_from('<I', data, 8)[0]):
        length, _, size, packed = struct.unpack_from('<IIQQ', data, at)
        at += 24
        name = data[at:at + length - 1].decode()
        at += (length + 7) & ~7
        if name == CACHE_PATH:
            result = zlib.decompress(data[at:at + packed])
            if len(result) != size:
                raise ValueError('Invalid baseline launchd cache')
            return result
        at += (packed + 7) & ~7
    raise ValueError('Baseline lacks its signed Wi-Fi launchd cache')


def patch_daemon(source, target, entitlement_path, experimental_advertising=False):
    image = bytearray(source.read_bytes())
    if hashlib.sha256(image).hexdigest() != DAEMON_SHA256:
        raise ValueError('Unrecognized 22H384 bluetoothd')
    patches = (*DAEMON_PATCHES, *optional_command_patches(image), *extended_scan_patches(image, experimental_advertising))
    for offset, original, replacement in patches:
        if struct.unpack_from('<I', image, offset)[0] != original:
            raise ValueError(f'Unexpected instruction at {offset:#x}')
        struct.pack_into('<I', image, offset, replacement)
    target.write_bytes(image)
    target.chmod(0o755)
    command = ['codesign', '-d', '--xml', '--entitlements', '-']
    original = subprocess.check_output([*command, source], stderr=subprocess.DEVNULL)
    entitlement_path.write_bytes(original)
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier',
                    'com.apple.bluetoothd', '--entitlements', entitlement_path, target], check=True)
    signed = subprocess.check_output([*command, target], stderr=subprocess.DEVNULL)
    if plistlib.loads(signed) != plistlib.loads(original):
        raise ValueError('bluetoothd entitlements differ')
    return patches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-addon', required=True, type=Path)
    parser.add_argument('--os-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--experimental-advertising', action='store_true',
                        help='Reproduce the experimental controller advertising candidate')
    args = parser.parse_args()
    out = args.output.resolve()
    version = plistlib.loads((args.os_root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if version['ProductBuildVersion'] != '22H384':
        raise ValueError('BlueTool requires the original 22H384 system')
    source = args.os_root / 'usr/sbin/BlueTool'
    (out / 'bin').mkdir(parents=True, exist_ok=True)
    (out / 'xpc').mkdir(exist_ok=True)
    binary = out / 'bin/unixnu-BlueTool'
    compiler_input = DEVICE / 'ios18/sources/QcaBluetooth/userland/BlueTool.c'
    subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64e-apple-ios18.0',
                    '-fblocks', '-Os', '-Wall', '-Wextra', '-Werror', compiler_input, '-o', binary], check=True)
    entitlements = out / 'BlueTool.entitlements.plist'
    original_entitlements = plistlib.loads(subprocess.check_output(
        ['codesign', '-d', '--xml', '--entitlements', '-', source], stderr=subprocess.DEVNULL))
    allowed_reads = 'com.apple.security.exception.sysctl.read-only'
    service_entitlements = dict(original_entitlements)
    service_entitlements[allowed_reads] = sorted(set(original_entitlements.get(allowed_reads, ())) |
        {'kern.qbt_boot', 'kern.qbt_power_on', 'kern.qbt_firmware'})
    entitlements.write_bytes(plistlib.dumps(service_entitlements))
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier',
                    'com.apple.BlueTool', '--entitlements', entitlements, binary], check=True)
    signed_entitlements = subprocess.check_output(['codesign', '-d', '--xml', '--entitlements', '-', binary],
                                                  stderr=subprocess.DEVNULL)
    if plistlib.loads(signed_entitlements) != service_entitlements:
        raise ValueError('BlueTool entitlements differ')
    daemon = out / 'bin/bluetoothd'
    daemon_patches = patch_daemon(args.os_root / 'usr/sbin/bluetoothd', daemon,
                                 out / 'bluetoothd.entitlements.plist', args.experimental_advertising)
    original = launchd_cache(args.baseline_addon)
    cache = plistlib.loads(original)
    jobs = cache['LaunchDaemons']
    matches = [key for key, job in jobs.items() if job.get('Label') == 'com.apple.BlueTool']
    if len(matches) != 1:
        raise ValueError('Expected one BlueTool launchd job')
    key = matches[0]
    job = jobs[key]
    if job.get('ProgramArguments', [None])[0] != '/usr/sbin/BlueTool' or job.get('MachServices') != {'com.apple.BlueTool': True}:
        raise ValueError('Unexpected BlueTool entry point or Mach service')
    job.update(Program='/usr/libexec/unixnu-BlueTool', ProgramArguments=['/usr/libexec/unixnu-BlueTool'],
               UserName='root', EnableTransactions=False, EnablePressuredExit=False, ThrottleInterval=3)
    daemon_jobs = [job for job in jobs.values() if job.get('Label') == 'com.apple.bluetoothd']
    if len(daemon_jobs) != 1:
        raise ValueError('Expected one bluetoothd launchd job')
    daemon_job = daemon_jobs[0]
    argv = daemon_job.get('ProgramArguments', [])
    if not argv or argv[0] != '/usr/sbin/bluetoothd':
        raise ValueError('Unexpected bluetoothd entry point')
    daemon_job.update(Program='/usr/libexec/bluetoothd', ProgramArguments=['/usr/libexec/bluetoothd', *argv[1:]])
    destination = out / 'xpc/launchd.plist'
    destination.write_bytes(plistlib.dumps(cache, fmt=plistlib.FMT_XML, sort_keys=False))
    subprocess.run(['plutil', '-convert', 'binary1', destination], check=True)
    if plistlib.loads(destination.read_bytes()) != cache:
        raise ValueError('Launchd cache serialization changed its contents')
    spec = importlib.util.spec_from_file_location('boot_runtime', DEVICE / 'ios18/source/scripts/build-boot-runtime.py')
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)
    cache_signature = runtime.sign_launchd_cache(destination)
    paths = ('bin/unixnu-BlueTool', 'bin/bluetoothd', 'xpc/launchd.plist', 'xpc/launchd.plist.sig')
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = dict(build='22H384', target='ios18', baseline_addon=digest(args.baseline_addon),
                  files={name: digest(out / name) for name in paths},
                  cdhashes=[runtime.verified_cdhash(binary), runtime.verified_cdhash(daemon), cache_signature['cdhash']],
                  directories={'bin': 'bin', 'xpc': 'xpc'}, jobs=len(jobs), updated_job=key,
                  original_launchd_sha256=hashlib.sha256(original).hexdigest(),
                  original_bluetool_sha256=digest(source), original_entitlements_preserved=True,
                  additional_sysctl_reads=service_entitlements[allowed_reads],
                  daemon_input_sha256=DAEMON_SHA256, daemon_patches=daemon_patches)
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out)


if __name__ == '__main__':
    main()
