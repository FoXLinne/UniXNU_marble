#!/usr/bin/env python3
"""Record the complete 24B119 source inputs, with caches and build outputs excluded."""
import argparse
from datetime import datetime, timezone, timedelta
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess

from ios27_migration import DEVICE, RECORDS, digest, save

EXCLUDED = {'.git', '.cache', '__pycache__', 'target', 'build', 'out', 'deps', 'node_modules',
            '.pytest_cache', '.mypy_cache'}
INPUTS = ('ios27/source', 'ios27/sources', 'package', 'hal', 'scripts', 'patches', 'profiles', 'configs',
          'dts', 'socs/sm7475')
ROOT_FILES = ('board.toml', 'feeds.toml', 'source_origins.json', 'upstream_revisions.json')


def source_files():
    for relative in INPUTS:
        root = DEVICE / relative
        for directory, directories, names in os.walk(root, followlinks=False):
            directories[:] = sorted(name for name in directories if name not in EXCLUDED
                and not (name == 'observations' and Path(directory).name == 'corpus')
                and not (name == 'docs' and Path(directory) == DEVICE / 'ios27/source'))
            for name in list(directories):
                path = Path(directory) / name
                if path.is_symlink():
                    yield path
                    directories.remove(name)
            for name in sorted(names):
                if name not in ('.DS_Store',) and not name.endswith(('.pyc', '.pyo', '.orig', '.rej', '.stage')):
                    yield Path(directory) / name
    for relative in ROOT_FILES:
        yield DEVICE / relative


def snapshot():
    records, aggregate = {}, hashlib.sha256()
    for path in sorted(set(source_files())):
        relative = path.relative_to(DEVICE).as_posix()
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            target = os.readlink(path)
            row = {'kind': 'symlink', 'target': target,
                   'sha256': hashlib.sha256(os.fsencode(target)).hexdigest()}
        elif stat.S_ISREG(mode):
            row = {'kind': 'file', 'sha256': digest(path), 'bytes': path.stat().st_size,
                   'executable': bool(mode & 0o111)}
        else:
            raise ValueError('Source input requires a regular file or symlink: ' + str(path))
        records[relative] = row
        aggregate.update(relative.encode() + b'\0' + json.dumps(row, sort_keys=True).encode() + b'\0')
    return {'target': 'ios-27.2-24B119', 'branch': subprocess.check_output(
                ['git', '-C', str(DEVICE), 'branch', '--show-current'], text=True).strip(),
            'source_base': subprocess.check_output(['git', '-C', str(DEVICE), 'rev-parse', 'HEAD'], text=True).strip(),
            'generated_at': datetime.now(timezone(timedelta(hours=8))).isoformat(),
            'excluded_directory_names': sorted(EXCLUDED), 'source_sha256': aggregate.hexdigest(),
            'file_count': len(records), 'files': records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', type=Path, help='Compare actual inputs with this saved source snapshot')
    args = parser.parse_args()
    actual = snapshot()
    if args.check:
        previous = json.loads(args.check.read_text())
        changed = [name for name in sorted(set(previous['files']) | set(actual['files']))
                   if previous['files'].get(name) != actual['files'].get(name)]
        print(json.dumps({'matches': not changed, 'changed': changed, 'source_sha256': actual['source_sha256']}, indent=2))
        raise SystemExit(bool(changed))
    path = RECORDS / 'source-snapshot.json'
    save(path, actual)
    current = json.loads((DEVICE / 'CURRENT.json').read_text())
    current['updated_at'] = actual['generated_at']
    current['latest_source'].update(record='records/ios27-24B119/source-snapshot.json',
                                     sha256=digest(path),
                                     snapshot='records/ios27-24B119/source-snapshot.json',
                                     snapshot_sha256=digest(path), source_sha256=actual['source_sha256'],
                                     file_count=actual['file_count'])
    save(DEVICE / 'CURRENT.json', current)
    print(json.dumps({'path': str(path), 'source_sha256': actual['source_sha256'], 'files': actual['file_count']}))


if __name__ == '__main__':
    main()
