#!/usr/bin/env python3
"""Resolve shared-cache stack addresses in the resident UI trace."""
import argparse
import json
from pathlib import Path
import re
from symbolicate_root_log import symbolicate


def parse(text):
    snapshot = 0
    roles = {}
    for line in text.splitlines():
        match = re.match(r'\[snapshot (\d+) begin\]', line)
        if match:
            snapshot = int(match[1])
            continue
        match = re.match(r'STK([A-Z]) p(\d+) cache\+([0-9a-f]+) image=([0-9a-f]+) pc=([0-9a-f]+) lr=([0-9a-f]+)', line)
        if match:
            role, pid, slide, image, pc, lr = match.groups()
            roles[snapshot, role] = dict(snapshot=snapshot, role=role, pid=int(pid), cache_slide=int(slide, 16),
                                         main_image=hex(int(image, 16)), addresses=[int(pc, 16), int(lr, 16)], rows=[line])
            continue
        match = re.match(r'STK([A-Z])(?:#| )', line)
        if match and (snapshot, match[1]) in roles:
            record = roles[snapshot, match[1]]
            record['rows'].append(line)
            if 'trap=' not in line:
                record['addresses'] += [int(value, 16) for value in re.findall(r'\b([0-9a-f]{9,10})\b', line)]
    return list(roles.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    results = []
    for record in parse(args.trace.read_text()):
        addresses = list(dict.fromkeys(address for address in record.pop('addresses') if address))
        slide = record.pop('cache_slide')
        record.update(symbolicate(args.cache, addresses, slide))
        results.append(record)
    args.output.write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
