#!/usr/bin/env python3
"""Join marble's vendor adsp2 MDT segments for the kernel firmware loader."""
import argparse
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True,
                        help='directory containing the device vendor adsp2.mdt and adsp2.bNN files')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    metadata = (args.directory / 'adsp2.mdt').read_bytes()
    if len(metadata) < 52 or metadata[:6] != b'\x7fELF\x01\x01':
        parser.error('expected little-endian ELF32 adsp2.mdt')
    offset = struct.unpack_from('<I', metadata, 28)[0]
    stride, count = struct.unpack_from('<HH', metadata, 42)
    if stride != 32 or not 0 < count <= 64 or offset + stride * count > len(metadata):
        parser.error('invalid MDT program headers')
    segments = []
    for index in range(count):
        header = struct.unpack_from('<8I', metadata, offset + stride * index)
        start, size = header[1], header[4]
        if not size:
            continue
        data = (args.directory / f'adsp2.b{index:02d}').read_bytes()
        if len(data) != size or start + size > 0x3100000:
            parser.error(f'adsp2.b{index:02d} differs from the MDT segment geometry')
        segments.append((start, data))
    image = bytearray(max(start + len(data) for start, data in segments))
    for start, data in segments:
        image[start:start + len(data)] = data
    if image[:offset + stride * count] != metadata[:offset + stride * count]:
        parser.error('adsp2.b00 differs from the MDT headers')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f'{args.output}: {len(image)} bytes')


if __name__ == '__main__':
    main()
