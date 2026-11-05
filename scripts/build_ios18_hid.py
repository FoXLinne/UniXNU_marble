#!/usr/bin/env python3
"""Bind marble HID sources to the 22H384 IOHIDDevice."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys


def prepare(tree):
    source = tree.parent / 'IOHIDFamily'
    include = tree / 'build/marble-boot/include'
    for origin, relative in ((source / 'IOHIDFamily', 'IOKit/hid'),
                             (source / 'IOHIDSystem/IOKit/hidsystem', 'IOKit/hidsystem')):
        destination = include / relative
        destination.mkdir(parents=True, exist_ok=True)
        for header in origin.glob('*.h'):
            shutil.copyfile(header, destination / header.name)
    sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    return ['-I', str(include), '-idirafter', str(sdk / 'System/Library/Frameworks/Kernel.framework/Versions/A/Headers')]


def bindings(output, obj):
    sys.path.insert(0, str(output / 'XnuPrelink'))
    from kc_image import Image
    fixups = Image((output / 'ipsw/22H384/kernelcache.decompressed').read_bytes()).fixups()
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
                string, kind, _, _, value = struct.unpack_from('<IBBHQ', raw, symoff + index * 16)
                symbols.append((raw[stroff + string:raw.index(b'\0', stroff + string)].decode(), value, kind))
        position += length
    entries = {}
    for va, size, fileoff, reloc, count in sections:
        for index in range(count):
            offset, bits = struct.unpack_from('<II', raw, reloc + index * 8)
            if bits >> 27 & 1 and bits >> 28 == 11 and bits >> 25 & 3 == 3:
                word = struct.unpack_from('<Q', raw, fileoff + offset)[0]
                entries[va + offset] = (symbols[bits & 0xffffff][0], word)
    result = {'__ZN11IOHIDDeviceC2EPK11OSMetaClass': 0xfffffff0099de950,
              '__ZN11IOHIDDeviceD2Ev': 0xfffffff0099de984}
    tables = ('__ZTV15NativeHomeInput', '__ZTV22NativeMarbleTouchInput')
    for table in tables:
        vtable = next(value for name, value, _ in symbols if name == table)
        for slot in range(207):
            name, word = entries[vtable + 16 + slot * 8]
            target, auth, key, diversity, address = fixups[0xfffffff007d37378 + slot * 8]
            if (auth, key, address) != (1, 0, 1) or word >> 32 & 0xffff != diversity:
                raise RuntimeError(f'22H384 {table} virtual slot {slot} authentication differs')
            if '11IOHIDDevice' in name:
                result[name] = target
        if vtable + 16 + 207 * 8 in entries:
            raise RuntimeError(f'22H384 {table} virtual table size differs')
    data = {'__ZN11IOHIDDevice10gMetaClassE', '__ZTV11IOHIDDevice'}
    undefined = {name for name, _, kind in symbols if kind == 1 and '11IOHIDDevice' in name}
    if undefined - result.keys() - data:
        raise RuntimeError('Unbound IOHIDDevice imports: ' + str(undefined - result.keys() - data))
    (output / 'hid-abi.json').write_text(json.dumps({'slots': 207, 'tables': tables, 'bindings': result}, indent=2) + '\n')
    return result
