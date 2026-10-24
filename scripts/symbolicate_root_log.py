#!/usr/bin/env python3
"""Resolve resident user addresses against the matching ARM64 dyld cache."""
import argparse
import json
import mmap
from pathlib import Path
import re
import struct


def symbolicate(cache, addresses, slide):
    # Apple dyld_cache_format.h: imagesTextOffset 136, sharedRegionStart 224,
    # localSymbolsOffset 72, symbolFileUUID 400; entries use 64-bit dylib offsets.
    main = (cache / 'dyld_shared_cache_arm64').read_bytes()
    region = struct.unpack_from('<Q', main, 224)[0]
    offset, count = struct.unpack_from('<QQ', main, 136)
    images = []
    for i in range(count):
        _, address, size, path = struct.unpack_from('<16sQII', main, offset + i * 32)
        images.append((address, size, main[path:main.index(b'\0', path)].decode()))
    rows = []
    with (cache / 'dyld_shared_cache_arm64.symbols').open('rb') as stream, \
            mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as symbols:
        if symbols[88:104] != main[400:416]:
            raise ValueError('Local symbols UUID differs from the main cache')
        local = struct.unpack_from('<Q', symbols, 72)[0]
        no, nc, so, ss, eo, ec = struct.unpack_from('<6I', symbols, local)
        entries = {struct.unpack_from('<Q', symbols, local + eo + i * 16)[0]:
                   struct.unpack_from('<II', symbols, local + eo + i * 16 + 8) for i in range(ec)}
        for address in addresses:
            va = address - slide
            image = next((im for im in images if im[0] <= va < im[0] + im[1]), None)
            row = dict(address=hex(address), unslid_address=hex(va), image=image[2] if image else None)
            if image and image[0] - region in entries:
                start, length = entries[image[0] - region]
                best = None
                for index in range(start, start + length):
                    string, kind, section, _, value = struct.unpack_from('<IBBHQ', symbols, local + no + index * 16)
                    if not kind & 0xe0 and kind & 0xe == 0xe and section and image[0] <= value <= va:
                        if best is None or value > best[0]:
                            best = value, string
                if best:
                    value, string = best
                    pos = local + so + string
                    row.update(symbol=symbols[pos:symbols.find(b'\0', pos)].decode(), offset=va - value)
            rows.append(row)
    return dict(cache_slide=hex(slide), local_symbols_uuid_matches=True, frames=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    text = args.log.read_text(errors='replace')
    slide = int(re.findall(r'cache_slide=([0-9a-f]+)', text)[-1], 16)
    pc, lr = re.findall(r'pc=([0-9a-f]+) lr=([0-9a-f]+)', text)[-1]
    addresses = [int(pc, 16), int(lr, 16) - 4]
    addresses += [int(value, 16) - 4 for value in re.findall(r'frame \d+ fp=[0-9a-f]+ return=([0-9a-f]+)', text) if int(value, 16)]
    result = symbolicate(args.cache, addresses, slide)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
