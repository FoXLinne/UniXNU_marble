#!/usr/bin/env python3
"""Extract checked AW88263 Music registers from the stock marble ACF input."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

def extract(path):
    data = path.read_bytes()
    if len(data) < 80 or struct.unpack_from('<I', data)[0] != 0x0a15f908:
        raise ValueError('AW ACF header mismatch')
    size, count, offset, version = struct.unpack_from('<4I', data, 52)
    if version != 0x01000000 or count != 8 or size != count * 96 or offset + size > len(data):
        raise ValueError('Unsupported ACF table geometry')
    profiles = []
    for index in range(count):
        row = data[offset + index * 96:offset + (index + 1) * 96]
        dev, bus, address, profile = struct.unpack_from('<4H', row, 20)
        kind, length, start, crc = struct.unpack_from('<4I', row, 28)
        name = row[44:76].split(b'\0')[0].decode()
        chip, = struct.unpack_from('<I', row, 76)
        if chip != 0x2032 or name != 'Music' or kind != 4: continue
        section = data[start:start + length]
        if len(section) != length or length != 240 or struct.unpack_from('<I', section, 4)[0] != 0x01000000:
            raise ValueError('AW register section geometry mismatch')
        if sum(section[4:]) & 0xffffffff != struct.unpack_from('<I', section)[0]:
            raise ValueError('AW binary checksum mismatch')
        registers = list(struct.iter_unpack('<HH', section[64:]))
        if len(registers) != 44 or struct.unpack_from('<I', section, 60)[0] != 44 or any(reg >= 128 for reg, value in registers):
            raise ValueError('AW register list mismatch')
        profiles.append({'channel': dev, 'chip_id': chip, 'profile': name, 'registers': registers})
    if sorted(row['channel'] for row in profiles) != [0, 1]: raise ValueError('Both speaker profiles required')
    return {'device': 'marble', 'source': 'stock vendor/firmware/aw882xx_acf.bin',
        'source_sha256': hashlib.sha256(data).hexdigest(), 'profiles': profiles}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_text(json.dumps(extract(args.input), indent=2)+'\n')
