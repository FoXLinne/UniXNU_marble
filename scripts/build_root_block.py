#!/usr/bin/env python3
"""Qualify original ordinary-ARM64 block-storage ABI using public Darwin 23 source."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
import native_prelink as np
source = ROOT / 'refer/IOStorageFamily'
revision = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
if revision != 'e45fbe30a7879e0b16e26dddb78538f1b00e0bff':
    raise SystemExit('Unexpected public Darwin 23 IOStorageFamily source')
out = ROOT / 'build/storage-abi'
headers = out / 'include/IOKit/storage'
headers.mkdir(parents=True, exist_ok=True)
for path in source.glob('*.h'):
    shutil.copyfile(path, headers / path.name)
# Only this platform-selection header is missing from the source-kernel SDK.
# The reference object is an annotation instrument and is never linked/executed.
sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
conditions = sdk / 'usr/include/TargetConditionals.h'
shutil.copyfile(conditions, out / 'include/TargetConditionals.h')
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
obj_path = out / 'IOBlockStorageDevice-reference.o'
arguments = record['arguments']
command, index = [], 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source / 'IOBlockStorageDevice.cpp'))
    elif value == '-o':
        command += ['-o', str(obj_path)]
        index += 1
    elif value in ('-MF', '-MJ'):
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W') and value not in ('-MD', '-MP'):
        command.append(value)
    index += 1
command += ['-I', str(out / 'include'), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
            '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-deprecated-declarations',
            '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-unnecessary-virtual-specifier',
            '-Wno-deprecated-enum-enum-conversion']
(out / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
r = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
(out / 'compile.log').write_text(r.stdout + r.stderr)
if r.returncode:
    raise SystemExit((r.stdout + r.stderr)[-3500:])


def require(value, message):
    if not value:
        raise SystemExit(message)


obj = obj_path.read_bytes()
om = np.macho(obj)
require(om['kind'] == 1, 'Expected an ordinary ARM64 reference object')
dwarf = subprocess.check_output(['xcrun', 'dwarfdump', '--name=IOBlockStorageDevice', '--show-children', obj_path], text=True)
class_size = re.search(r'DW_AT_name\s+\("IOBlockStorageDevice"\)[\s\S]{0,200}?DW_AT_byte_size\s+\((0x[0-9a-f]+)\)', dwarf)
require(class_size and int(class_size[1], 16) == 0x90, 'Actual compiled public class size changed')
version_command = next(at for command, at, size in om['commands'] if command == 0x32)
require(struct.unpack_from('<II', obj, version_command + 8) == (2, 0x110000), 'Expected iOS 17 reference deployment')
sycmd = next(at for command, at, size in om['commands'] if command == 2)
symoff, count, stroff, _ = struct.unpack_from('<4I', obj, sycmd + 8)
symbols = []
for i in range(count):
    ix, kind, _, _, value = struct.unpack_from('<IBBHQ', obj, symoff + i * 16)
    symbols.append(dict(name=obj[stroff + ix:obj.index(b'\0', stroff + ix)].decode(), value=value, kind=kind))
vtable = next(s['value'] for s in symbols if s['name'] == '__ZTV20IOBlockStorageDevice')
end = min(s['value'] for s in symbols if not s['kind'] & 0xe0 and s['kind'] & 0xe == 0xe and s['value'] > vtable)
relocations = {}
for segment in om['segments']:
    for section in segment['sections']:
        if section['va'] <= vtable < section['va'] + section['size']:
            ro, rc = struct.unpack_from('<II', obj, section['command_offset'] + 56)
            for i in range(rc):
                address, word = struct.unpack_from('<II', obj, ro + i * 8)
                if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                    relocations[section['va'] + address] = symbols[word & 0xffffff]['name']
methods = []
for at in range(vtable + 16, end, 8):
    if at not in relocations:
        break
    methods.append(relocations[at])
require(len(methods) == 190 and methods.count('___cxa_pure_virtual') == 14, 'Public iOS class dispatch shape changed')
kernel_path = ROOT.parent / 'kernel/kernel.macho'
kernel = kernel_path.read_bytes()
plan = json.loads(kernel_path.with_suffix('.json').read_text())
require(hashlib.sha256(kernel).hexdigest() == plan['candidate_sha256'] and
        plan['original_sha256'] == 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7', 'Unexpected original candidate provenance')
native = np.macho(kernel)
exports = np.exports(kernel, native)
extents = [e for e in plan['extents'] if e['module'] == 'com.apple.iokit.IOStorageFamily']
const = next(e for e in extents if e['segment'] == '__DATA_CONST')
text = next(e for e in extents if e['segment'] == '__TEXT_EXEC')
# A previous port build also exports aliases into the original storage module.
# Only source-kernel implementations count as native method bindings.
by_name = {name: va for va, names in exports.items() for name in names
           if not any(e['va'] <= va < e['va'] + e['size'] for e in extents)}


def read(va, length):
    at = np.backed_offset(native['segments'], va, length, len(kernel))
    return kernel[at:at + length]


require(hashlib.sha256(read(text['va'], text['size'])).hexdigest() ==
        plan['original_executable_sha256']['com.apple.iokit.IOStorageFamily'], 'Original storage executable code changed')
blob = read(const['va'], const['size'])
found = []
for at in range(0, len(blob) - (len(methods) + 2) * 8 + 1, 8):
    if blob[at:at + 16] != bytes(16):
        continue
    pointers = struct.unpack_from('<' + 'Q' * len(methods), blob, at + 16)
    if all(pointers[i] == by_name[name] for i, name in enumerate(methods) if name in by_name):
        found.append((const['va'] + at, pointers))
require(len(found) == 1, 'Cannot uniquely identify the original block-storage class dispatch')
table, pointers = found[0]
matched = sum(name in by_name for name in methods)
require(matched == 176, 'Unexpected original/native kernel binding count')
for i, name in enumerate(methods):
    if name not in by_name:
        require(text['va'] <= pointers[i] < text['va'] + text['size'], 'Owned storage method outside original text')


def relative_branch(address, instruction):
    immediate = instruction & 0x3ffffff
    if immediate & 0x2000000:
        immediate -= 0x4000000
    return address + immediate * 4


def thunk_names(address):
    a, load, branch = struct.unpack('<3I', read(address, 12))
    require(a & 0x9f00001f == 0x90000010 and load & 0xffc003ff == 0xf9400210 and branch == 0xd61f0200,
            'Unqualified original native import thunk')
    immediate = ((a >> 5 & 0x7ffff) << 2) | (a >> 29 & 3)
    if immediate & 0x100000:
        immediate -= 0x200000
    slot = (address & ~4095) + immediate * 4096 + (load >> 10 & 4095) * 8
    target = struct.unpack('<Q', read(slot, 8))[0]
    return exports.get(target, [])


constructors = []
for address in range(text['va'], text['va'] + text['size'] - 32, 4):
    words = struct.unpack('<8I', read(address, 32))
    if (words[:2] != (0xa9bf7bfd, 0x910003fd) or words[2] & 0xfc000000 != 0x94000000 or
        words[3] & 0x9f00001f != 0x90000008 or words[4] & 0xffc003ff != 0x91000108 or
        words[5:] != (0xf9000008, 0xa8c17bfd, 0xd65f03c0)):
        continue
    immediate = ((words[3] >> 5 & 0x7ffff) << 2) | (words[3] >> 29 & 3)
    if immediate & 0x100000:
        immediate -= 0x200000
    stored = ((address + 12) & ~4095) + immediate * 4096 + (words[4] >> 10 & 4095)
    if stored == table + 16:
        constructors.append((address, relative_branch(address + 8, words[2])))
require(len(constructors) == 1 and '__ZN9IOServiceC2EPK11OSMetaClass' in thunk_names(constructors[0][1]),
        'Original protected block constructor does not forward to actual source IOService')
destructor = pointers[0] - 4
word = struct.unpack('<I', read(destructor, 4))[0]
require(word & 0xfc000000 == 0x14000000 and
        '__ZN9IOServiceD2Ev' in thunk_names(relative_branch(destructor, word)), 'Unqualified actual base destructor')
require(all(read(pointers[i], 4) == struct.pack('<I', 0xd4200020) for i in (0, 1)),
        'Unexpected original abstract complete-destructor behavior')
function = pointers[7]
a, add, ret = struct.unpack('<3I', read(function, 12))
require(a & 0x9f00001f == 0x90000000 and add & 0xffc003ff == 0x91000000 and ret == 0xd65f03c0,
        'Unqualified original storage metaclass getter')
immediate = ((a >> 5 & 0x7ffff) << 2) | (a >> 29 & 3)
if immediate & 0x100000:
    immediate -= 0x200000
meta = (function & ~4095) + immediate * 4096 + (add >> 10 & 4095)

# The public storage class's own methods retain their original implementations.
# Native IOKit methods are linked by the normal kernel linker.
out = ROOT / 'build/native-root-block'
out.mkdir(parents=True, exist_ok=True)
source = ROOT / 'kits/marble/NativeRootBlock.cpp'
obj_path = out / 'NativeRootBlock.o'
command = []
arguments = iter(record['arguments'])
for argument in arguments:
    if argument == record['file']:
        command.append(str(source))
    elif argument in ('-o', '-MF', '-MT', '-MJ'):
        next(arguments)
        if argument == '-o': command += ['-o', str(obj_path)]
    else:
        command.append(argument)
command += ['-I', str(ROOT / 'build/storage-abi/include'), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
            '-I', str(ROOT / 'package/UniXNUPkg/Include'), '-I', str(ROOT / 'drivers/NativeRamBlock')]
subprocess.run(command, cwd=record['directory'], check=True)
obj = obj_path.read_bytes()
image = np.macho(obj)
sycmd = next(at for cmd, at, size in image['commands'] if cmd == 2)
sy, count, st, _ = struct.unpack_from('<4I', obj, sycmd + 8)
undefined = []
for i in range(count):
    ix, kind, _, _, value = struct.unpack_from('<IBBHQ', obj, sy + i * 16)
    name = obj[st + ix:obj.index(b'\0', st + ix)].decode()
    if kind == 1 and 'IOBlockStorageDevice' in name: undefined.append(name)
possible = {name: pointers[i] for i, name in enumerate(methods) if name not in by_name}
possible.update({'__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': constructors[0][0],
                 '__ZN20IOBlockStorageDeviceD2Ev': destructor,
                 '__ZN20IOBlockStorageDevice10gMetaClassE': meta,
                 '__ZTV20IOBlockStorageDevice': table})
bindings = {name: possible[name] for name in undefined}
assembly = out / 'original-bindings.S'
assembly.write_text(''.join(f'.globl {name}\n.set {name}, {value:#x}\n' for name,value in sorted(bindings.items())))
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-c', assembly,
                '-o', out / 'original-bindings.o'], check=True)
late = out / 'NativeRootBlock.late.o'
subprocess.run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
                '--globalize-symbol=__GLOBAL__sub_I_NativeRootBlock.cpp', obj_path, late], check=True)
resident = out / 'NativeResident.o'
resident_command = [str(ROOT / 'kits/marble/NativeResident.cpp') if v == str(source) else str(resident) if v == str(obj_path) else v for v in command]
subprocess.run(resident_command, cwd=record['directory'], check=True)
policy_source = ROOT / 'kits/marble/RootWatchdogPolicy.c'
policy_object = out / 'RootWatchdogPolicy.o'
policy_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json').read_text().strip().rstrip(','))
policy_command = []
policy_arguments = iter(policy_recipe['arguments'])
for value in policy_arguments:
    if value == policy_recipe['file']:
        policy_command.append(str(policy_source))
    elif value in ('-MF', '-MT', '-MJ'):
        next(policy_arguments)
    elif value == '-o':
        next(policy_arguments)
        policy_command += ['-o', str(policy_object)]
    else:
        policy_command.append(value)
policy_command += ['-mgeneral-regs-only']
subprocess.run(policy_command, cwd=policy_recipe['directory'], check=True)
archive = out / 'libNativeRootBlock.a'
runtime = out / 'RuntimeUsb.o'
runtime_command = [str(ROOT / 'kits/marble/RuntimeUsb.cpp') if v == str(source) else str(runtime) if v == str(obj_path) else v for v in command]
subprocess.run(runtime_command, cwd=record['directory'], check=True)
touch = out / 'MarbleTouch.o'
touch_command = [str(ROOT / 'kits/marble/MarbleTouch.cpp') if v == str(source) else str(touch) if v == str(obj_path) else v for v in command]
touch_command += ['-I', str(ROOT / 'drivers/NativeGeniSpi')]
subprocess.run(touch_command, cwd=record['directory'], check=True)
gpu = out / 'MarbleGpu.o'
gpu_command = [str(ROOT / 'kits/marble/MarbleGpu.cpp') if v == str(source) else str(gpu) if v == str(obj_path) else v for v in command]
subprocess.run(gpu_command, cwd=record['directory'], check=True)
cpu = out / 'MarbleCpu.o'
cpu_command = [str(ROOT / 'kits/marble/MarbleCpu.cpp') if v == str(source) else str(cpu) if v == str(obj_path) else v for v in command]
subprocess.run(cpu_command, cwd=record['directory'], check=True)
gmu = out / 'MarbleGmu.o'
gmu_command = [str(ROOT / 'kits/marble/MarbleGmu.cpp') if v == str(source) else str(gmu) if v == str(obj_path) else v for v in command]
gmu_command += ['-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'kits/AdrenoFirmware')]
subprocess.run(gmu_command, cwd=record['directory'], check=True)
ufs = out / 'MarbleUfs.o'
ufs_command = [str(ROOT / 'kits/marble/MarbleUfs.cpp') if v == str(source) else str(ufs) if v == str(obj_path) else v for v in command]
ufs_command += ['-I', str(ROOT / 'drivers/NativeUfs')]
subprocess.run(ufs_command, cwd=record['directory'], check=True)
gpu_objects = [gmu, cpu, ufs]
for source_path in [ROOT / 'kits/marble/MarbleAdreno.cpp', ROOT / 'kits/marble/MarbleZap.cpp', *[
    ROOT / 'drivers/NativeAdreno' / ('NativeAdreno' + name + '.cpp')
    for name in ('Memory', 'Cpu', 'SmmuMemory', 'SmmuContext', 'Vm', 'GpuContext', 'Ring', 'CpInit', 'Pwrup', 'Submit', 'UserClient', 'Publish', 'Timer', 'KernelTime')
]]:
    target = out / (source_path.stem + '.o')
    recipe = [str(source_path) if v == str(source) else str(target) if v == str(obj_path) else v for v in command]
    for directory in ('drivers/NativeAdreno', 'kits/ArmSmmu4K', 'kits/AdrenoPM4'):
        recipe += ['-I', str(ROOT / directory)]
    subprocess.run(recipe, cwd=record['directory'], check=True)
    gpu_objects.append(target)
for directory, names in (
    ('ArmSmmu4K', tuple(path.name for path in sorted((ROOT / 'kits/ArmSmmu4K').glob('ArmSmmu4K*.c')))),
    ('AdrenoPM4', ('AdrenoPM4.c',)),
    ('AdrenoFirmware', ('AdrenoFirmware.c', 'AdrenoHfi.c', 'AdrenoHfiVotes.c')),
):
    for name in names:
        target = out / (Path(name).stem + '.o')
        recipe = [str(ROOT / 'kits' / directory / name) if v == str(policy_source) else str(target) if v == str(policy_object) else v for v in policy_command]
        subprocess.run(recipe, cwd=policy_recipe['directory'], check=True)
        gpu_objects.append(target)
firmware = ROOT / '.cache/firmware/marble/gmu_gen70000.bin'
if hashlib.sha256(firmware.read_bytes()).hexdigest() != 'b2d3f0e2a98fd6259ab9ffef16df63265542e6cc9a970d53d476e823d432042a':
    raise SystemExit('Marble GMU firmware checksum differs')
sqe_firmware = ROOT / '.cache/firmware/marble/a730_sqe.fw'
if hashlib.sha256(sqe_firmware.read_bytes()).hexdigest() != 'e67d1829f57fc8326d806234b932dc3b78e28a9940df34c70752f17d5f38b7aa':
    raise SystemExit('Marble SQE firmware checksum differs')
firmware_assembly = out / 'marble-firmware.S'
firmware_assembly.write_text('.section __DATA_CONST,__const\n.p2align 12\n.globl _marble_gmu_firmware\n_marble_gmu_firmware:\n.incbin ' + json.dumps(str(firmware)) + '\nmarble_gmu_end:\n.p2align 3\n.globl _marble_gmu_firmware_size\n_marble_gmu_firmware_size:\n.quad marble_gmu_end-_marble_gmu_firmware\n')
with firmware_assembly.open('a') as stream:
    stream.write('.p2align 12\n.globl _marble_sqe_firmware\n_marble_sqe_firmware:\n.incbin ' + json.dumps(str(sqe_firmware)) + '\nmarble_sqe_end:\n.p2align 3\n.globl _marble_sqe_firmware_size\n_marble_sqe_firmware_size:\n.quad marble_sqe_end-_marble_sqe_firmware\n')
zap_firmware = ROOT / '.cache/firmware/marble/a730_zap.mbn'
if hashlib.sha256(zap_firmware.read_bytes()).hexdigest() != 'f03e732d07046c01250998d7f1ee2a2fae3e3ae28872320cdd38334e60e9a10e':
    raise SystemExit('Marble ZAP firmware checksum differs')
with firmware_assembly.open('a') as stream:
    stream.write('.p2align 12\n.globl _marble_zap_firmware\n_marble_zap_firmware:\n.incbin ' + json.dumps(str(zap_firmware)) + '\nmarble_zap_end:\n.p2align 3\n.globl _marble_zap_firmware_size\n_marble_zap_firmware_size:\n.quad marble_zap_end-_marble_zap_firmware\n')
firmware_object = out / 'marble-firmware.o'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', str(sdk), '-c', str(firmware_assembly), '-o', str(firmware_object)], check=True)
gpu_objects.append(firmware_object)
subprocess.run([sys.executable, str(ROOT / 'scripts/build-native-adreno-accelerator.py')], cwd=ROOT, check=True)
accelerator = ROOT / 'build/native-adreno-accelerator'
shutil.copyfile(accelerator / 'original-bindings.o', out / 'adreno-accelerator-bindings.o')
gpu_objects += [accelerator / 'NativeAdrenoAccelerator.late.o', out / 'adreno-accelerator-bindings.o']
spi = out / 'GeniSpi.o'
spi_command = [str(ROOT / 'drivers/NativeGeniSpi/GeniSpi.c') if v == str(policy_source) else str(spi) if v == str(policy_object) else v for v in policy_command]
subprocess.run(spi_command, cwd=policy_recipe['directory'], check=True)
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, late, resident, out / 'original-bindings.o', policy_object, runtime, touch, gpu, spi, *gpu_objects], check=True)
report = dict(reference_kernel_sha256=plan['candidate_sha256'], original_executable_code_unchanged=True,
              method_slots=len(methods), compiled_public_class_size=int(class_size[1], 16),
              bindings={name:hex(value) for name,value in bindings.items()}, runtime_qualified=False,
              archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
(out / 'bindings.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
