#!/usr/bin/env python3
"""Build the iOS 18 arm64e storage base and retained root-disk adapter."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess

DEVICE = Path(__file__).resolve().parents[1]


def build(tree, storage, ram):
    out = tree / 'build/marble-storage'
    out.mkdir(parents=True, exist_ok=True)
    headers = out / 'include/IOKit/storage'
    headers.mkdir(parents=True, exist_ok=True)
    for source in storage.glob('*.h'):
        shutil.copyfile(source, headers / source.name)
    sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    shutil.copyfile(sdk / 'usr/include/TargetConditionals.h', out / 'include/TargetConditionals.h')
    shutil.copyfile(ram / 'SparseDisk.h', out / 'include/SparseDisk.h')
    original = (storage / 'IOBlockStorageDevice.cpp').read_text()
    marker = 'OSDefineMetaClassAndAbstractStructors(IOBlockStorageDevice,IOService)'
    if original.count(marker) != 1:
        raise RuntimeError('Original storage base registration macro changed')
    base = out / 'IOBlockStorageDevice.cpp'
    base.write_text(original.replace(marker, '''
extern "C" const OSMetaClass *native_source_storage_parent(void);
OSDefineMetaClassWithInit(IOBlockStorageDevice, IOService, )
IOBlockStorageDevice::MetaClass::MetaClass()
    : OSMetaClass("UniXNUBlockStorageDevice", native_source_storage_parent(), sizeof(IOBlockStorageDevice)) {}
OSDefineAbstractStructors(IOBlockStorageDevice, IOService)
OSDefineOperatorMethods(IOBlockStorageDevice)
'''))
    record = json.loads((tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
    objects = []
    with (out / 'compile.log').open('w') as log:
        for source in (base, DEVICE / 'package/MarbleRootDisk.cpp', DEVICE / 'package/MarbleUfs.cpp'):
            obj = out / (source.stem + '.o')
            command = []
            arguments = iter(record['arguments'])
            for value in arguments:
                if value == record['file']:
                    command.append(str(source))
                elif value in ('-o', '-MF', '-MT', '-MJ'):
                    next(arguments)
                    if value == '-o':
                        command += ['-o', str(obj)]
                else:
                    command.append(value)
            command += ['-isystem', str(out / 'include'), '-I', str(tree / '.cache/xnu-work/osfmk'),
                        '-I', str(tree / 'package/UniXNUPkg/Include'),
                        '-I', str(DEVICE / 'package'),
                        '-I', str(DEVICE / 'ios17/source/drivers/NativeUfs'),
                        '-Wno-missing-include-dirs', '-Wno-unnecessary-virtual-specifier']
            if source != base:
                command += ['-Werror', '-Wno-c99-designator', '-Wno-nrvo', '-Wno-unused-function']
            subprocess.run(command, cwd=record['directory'], stdout=log, stderr=subprocess.STDOUT, check=True)
            magic, cpu, subtype, kind = struct.unpack_from('<4I', obj.read_bytes())
            if (magic, cpu, subtype & 0xffffff, kind) != (0xfeedfacf, 0x100000c, 2, 1):
                raise RuntimeError('Storage archive requires arm64e objects')
            if source.name == 'MarbleUfs.cpp':
                objects.append(obj)
                continue
            late = obj.with_suffix('.late.o')
            subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
                            f'--globalize-symbol=__GLOBAL__sub_I_{source.name}', obj, late], check=True)
            objects.append(late)
    archive = out / 'libMarbleStorage.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
    return archive


def qualify(native_path, cache_path, tools, report_path):
    import sys
    sys.path.insert(0, str(tools))
    from kc_image import Image, scan
    native, original = Image(native_path.read_bytes()), Image(cache_path.read_bytes())
    symbols = {name: address for address, names in native.symbols().items() for name in names}
    first = symbols['__ZTV20IOBlockStorageDevice'] + 16
    native_fixups, original_fixups = native.fixups(), original.fixups()
    slots = []
    while first + 8 * len(slots) in native_fixups:
        slots.append(native_fixups[first + 8 * len(slots)][1:])
    if len(slots) != 190:
        raise RuntimeError(f'Source block class has {len(slots)} virtual entries')
    member = Image(original.data, original.entries['com.apple.iokit.IOStorageFamily'][1])
    const = next(segment for segment in member.segments if segment['name'] == '__DATA_CONST')
    functions = member.function_starts()
    text = next(segment for segment in member.segments if segment['name'] == '__TEXT_EXEC')
    named_constructors = []
    for index, start in enumerate(functions):
        end = functions[index + 1] if index + 1 < len(functions) else text['va'] + text['filesize']
        _, references, _ = scan(original.read(start, end - start), start)
        if any(member.cstring(address) == 'IOBlockStorageDevice' for _, address, kind in references):
            named_constructors.append({address for _, address, kind in references})
    matches = []
    for address in original_fixups:
        if not const['va'] <= address < const['va'] + const['size']:
            continue
        if not all(original_fixups.get(address + 8 * index, (None,))[1:] == signature
                   for index, signature in enumerate(slots)):
            continue
        getter = original_fixups[address + 7 * 8][0]
        _, references, _ = scan(original.read(getter, 16), getter)
        objects = {target for _, target, kind in references if kind == 'add'}
        if any(objects & references for references in named_constructors):
            matches.append(address)
    if len(matches) != 1:
        raise RuntimeError(f'Original block class has {len(matches)} matching authenticated tables')
    report = {'source': 'IOStorageFamily-323', 'original_table': hex(matches[0]),
              'authenticated_virtual_entries': len(slots), 'class_size_checked_at_registration': True,
              'native': str(native_path), 'kernelcache': str(cache_path)}
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--storage', type=Path, required=True)
    parser.add_argument('--ram-source', type=Path, required=True)
    args = parser.parse_args()
    print(build(args.tree.resolve(), args.storage.resolve(), args.ram_source.resolve()))
