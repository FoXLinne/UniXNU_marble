#!/usr/bin/env python3
"""Record the device repository's source trees and their upstream origins."""
import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def source_digest(path):
    digest = hashlib.sha256()
    count = 0
    for directory, directories, filenames in os.walk(path):
        directories[:] = sorted(name for name in directories
                                if name not in ('.git', '__pycache__'))
        for name in sorted(filenames):
            source = Path(directory) / name
            relative = str(source.relative_to(path))
            data = os.readlink(source).encode() if source.is_symlink() else source.read_bytes()
            digest.update(relative.encode() + b'\0')
            digest.update(hashlib.sha256(data).digest())
            count += 1
    return {'sha256': digest.hexdigest(), 'files': count}


def main():
    origins = json.loads((ROOT / 'source_origins.json').read_text())
    records = []
    for source in origins['sources']:
        record = dict(source)
        record.update(source_digest(ROOT / source['local']))
        records.append(record)
    output = ROOT / 'source_snapshot.json'
    output.write_text(json.dumps({'device': 'marble', 'sources': records}, indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    main()
