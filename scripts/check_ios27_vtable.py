#!/usr/bin/env python3
"""Compare a compiled arm64e subclass vtable with its qualified 24B119 base ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def read_object(path):
    raw = path.read_bytes()
    magic, cpu, subtype, kind, commands, command_bytes = struct.unpack_from('<6I', raw)
    if (magic, cpu, subtype & 0xffffff, kind) != (0xfeedfacf, 0x100000c, 2, 1):
        raise ValueError('ABI audit requires an arm64e Mach-O object')
    offset, sections, symtab = 32, [], None
    for _ in range(commands):
        command, length = struct.unpack_from('<II', raw, offset)
        if length < 8 or offset + length > 32 + command_bytes:
            raise ValueError('Invalid object load-command length')
        if command == 0x19:
            count, = struct.unpack_from('<I', raw, offset + 64)
            for index in range(count):
                at = offset + 72 + index * 80
                name = raw[at:at+16].split(b'\0')[0].decode()
                address, size, fileoff = struct.unpack_from('<QQI', raw, at + 32)
                reloc, reloc_count = struct.unpack_from('<II', raw, at + 56)
                sections.append(dict(name=name, address=address, size=size, fileoff=fileoff,
                                     reloc=reloc, reloc_count=reloc_count))
        elif command == 2:
            symtab = struct.unpack_from('<4I', raw, offset + 8)
        offset += length
    if symtab is None:
        raise ValueError('Object symbol table required')
    symoff, count, stroff, string_bytes = symtab
    symbols = {}
    for index in range(count):
        name, kind, section, _, address = struct.unpack_from('<IBBHQ', raw, symoff + index * 16)
        if kind & 0xe0 or kind & 0xe != 0xe or section == 0:
            continue
        if name >= string_bytes:
            raise ValueError('Object symbol name lies outside its string table')
        text = raw[stroff + name:raw.index(b'\0', stroff + name)].decode()
        symbols[text] = address
    relocations = {}
    for section in sections:
        for index in range(section['reloc_count']):
            address, bits = struct.unpack_from('<iI', raw, section['reloc'] + 8 * index)
            if bits >> 28 != 11 or not (bits >> 27 & 1) or bits >> 25 & 3 != 3 or bits >> 24 & 1:
                continue
            if address < 0 or address + 8 > section['size']:
                raise ValueError('Authenticated relocation lies outside its section')
            value, = struct.unpack_from('<Q', raw, section['fileoff'] + address)
            relocations[section['address'] + address] = {
                'authenticated': bool(value >> 63), 'key': value >> 49 & 3,
                'address_diversity': value >> 48 & 1, 'diversity': value >> 32 & 0xffff}
    return symbols, relocations


def audit(object_path, profile_path, table):
    profile = json.loads(profile_path.read_text())
    if profile.get('build') != '24B119':
        raise ValueError('Select the qualified 24B119 class profile')
    symbols, relocations = read_object(object_path)
    if table not in symbols:
        raise ValueError('Compiled vtable symbol missing: ' + table)
    address, actual = symbols[table] + 16, []
    while address + len(actual) * 8 in relocations:
        actual.append(relocations[address + len(actual) * 8])
    expected = profile['slots']
    overrides = {row['slot']: row['caller_diversity'] for row in profile.get('semantics', {}).values()
                 if isinstance(row, dict) and 'slot' in row and 'caller_diversity' in row}
    differences = []
    if len(actual) != len(expected):
        differences.append({'kind': 'slot-count', 'compiled': len(actual), 'original': len(expected)})
    for index, (row, original) in enumerate(zip(actual, expected)):
        diversity = overrides.get(index, original[1])
        if isinstance(diversity, str):
            diversity = int(diversity, 0)
        if (not row['authenticated'] or row['key'] != 0 or row['address_diversity'] != 1
                or row['diversity'] != diversity):
            differences.append({'kind': 'caller-contract', 'slot': index,
                                'expected_diversity': hex(diversity), 'compiled': row})
    return {'build': '24B119', 'original_class': profile['class'], 'table': table,
            'object': str(object_path), 'object_sha256': hashlib.sha256(object_path.read_bytes()).hexdigest(),
            'profile': str(profile_path), 'profile_sha256': hashlib.sha256(profile_path.read_bytes()).hexdigest(),
            'compiled_slots': len(actual), 'original_slots': len(expected), 'differences': differences,
            'status': 'verified' if actual and not differences else 'pending'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--object', type=Path, required=True)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--table', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = audit(args.object.resolve(), args.profile.resolve(), args.table)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('status', 'table', 'compiled_slots', 'original_slots')}))
    raise SystemExit(report['status'] != 'verified')


if __name__ == '__main__':
    main()
