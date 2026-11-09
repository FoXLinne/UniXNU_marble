#!/usr/bin/env python3
"""Refresh upstream branches and merge their changes into device-owned sources."""
import argparse
import configparser
from urllib.parse import urljoin, urlparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

DEVICE = Path(__file__).resolve().parents[1]
WORKSPACE = DEVICE.parent.parent
OUTPUT = DEVICE / 'build/upstream'


def git(path, *arguments, **kwargs):
    return subprocess.run(['git', '-C', str(path), *arguments], capture_output=True, **kwargs)


def normalize(url):
    return url.rstrip('/').removesuffix('.git').lower()


def refresh(record):
    record = dict(record)
    path = WORKSPACE / record['path']
    for attempt in range(4):
        response = git(path, 'ls-remote', '--symref', 'origin', 'HEAD', text=True)
        if response.returncode == 0:
            break
        print(record['url'] + ': ' + response.stderr.strip(), flush=True)
        if attempt < 3:
            time.sleep(4 * (attempt + 1))
    response.check_returncode()
    branch = ('development' if 'steelbrain/metal2vulkan' in record['url'] else
              re.search(r'^ref: refs/heads/(\S+)\s+HEAD$', response.stdout, re.M)[1])
    for attempt in range(4):
        response = git(path, 'fetch', '--prune', '--depth=1', '--filter=blob:none', 'origin',
                       f'+refs/heads/{branch}:refs/remotes/origin/{branch}', text=True)
        if response.returncode == 0:
            break
        print(record['url'] + ': ' + response.stderr.strip(), flush=True)
        time.sleep(4 * (attempt + 1))
    if response.returncode:
        raise RuntimeError(record['url'] + ': ' + response.stderr.strip())
    revision, date, subject = git(path, 'show', '-s', '--format=%H%n%cI%n%s',
                                   f'refs/remotes/origin/{branch}', text=True).stdout.splitlines()[:3]
    record.update(branch=branch, revision=revision, commit_date=date, subject=subject,
                  status='fetched', fetched_at=datetime.now().astimezone().isoformat())
    print(f'{record["url"]}: {branch} {revision}', flush=True)
    return record


def merge_source(source, reference):
    repo = WORKSPACE / reference['path']
    destination = DEVICE / source['local']
    old, new = source['upstream_commit'], reference['revision']
    result = dict(name=source['name'], old=old, new=new, changed=[], conflicts=[])
    if old == new:
        return result
    if git(repo, 'cat-file', '-e', old + '^{commit}').returncode:
        git(repo, 'fetch', '--depth=1', '--filter=blob:none', 'origin', old).check_returncode()
    scopes = source.get('source_paths', [])
    changes = git(repo, 'diff', '--raw', '--no-abbrev', '--no-renames', '-z', old, new,
                  '--', *scopes).stdout.split(b'\0')
    entries = [(changes[i].decode().split(), changes[i + 1].decode())
               for i in range(0, len(changes) - 1, 2)]
    objects = {oid for fields, name in entries for oid in fields[2:4]
               if set(oid) != {'0'} and fields[0][1:] != '160000' and fields[1] != '160000'}
    missing = []
    if objects:
        check = git(repo, 'cat-file', '--batch-check', input='\n'.join(objects) + '\n', text=True,
                    env=dict(os.environ, GIT_NO_LAZY_FETCH='1'))
        missing = [line.split()[0] for line in check.stdout.splitlines() if line.endswith(' missing')]
    if missing:
        git(repo, '-c', 'protocol.version=2', '-c', 'fetch.negotiationAlgorithm=noop', 'fetch',
            '--no-tags', '--no-write-fetch-head', '--filter=blob:none', 'origin', '--stdin',
            input='\n'.join(missing) + '\n', text=True).check_returncode()

    def blob(oid):
        if set(oid) == {'0'}:
            return None
        value = git(repo, 'cat-file', 'blob', oid)
        value.check_returncode()
        return value.stdout

    with tempfile.TemporaryDirectory(dir=OUTPUT) as temporary:
        scratch = Path(temporary)
        for fields, name in entries:
            if fields[1] == '160000' or Path(name).name == 'AGENTS.md':
                continue
            path = destination / name
            before, incoming = blob(fields[2]), blob(fields[3])
            local = (os.readlink(path).encode() if path.is_symlink() else
                     path.read_bytes() if path.is_file() else None)
            if local == incoming or (local is None and before is not None):
                continue
            if local == before or local is None:
                merged = incoming
            elif incoming is None or before is None or b'\0' in local + before + incoming:
                result['conflicts'].append(name)
                continue
            else:
                for label, data in [('local', local), ('base', before), ('upstream', incoming)]:
                    (scratch / label).write_bytes(data.replace(b'\r\n', b'\n'))
                response = subprocess.run(['git', 'merge-file', '-p', str(scratch / 'local'),
                                           str(scratch / 'base'), str(scratch / 'upstream')],
                                          capture_output=True)
                merged = response.stdout
                if response.returncode:
                    conflict = OUTPUT / 'conflicts' / source['name'] / name
                    conflict.parent.mkdir(parents=True, exist_ok=True)
                    conflict.write_bytes(merged)
                    result['conflicts'].append(name)
                    continue
            if path.is_symlink():
                path.unlink()
            if merged is None:
                path.unlink(missing_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                if fields[1] == '120000':
                    path.symlink_to(merged.decode())
                else:
                    path.write_bytes(merged)
                    path.chmod(int(fields[1], 8) & 0o777)
            result['changed'].append(name)
    if result['conflicts']:
        source['pending_upstream_commit'] = new
    else:
        source.update(upstream_commit=new, upstream_branch=reference['branch'])
        source.pop('pending_upstream_commit', None)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch-only', action='store_true')
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    registry = DEVICE / 'upstream_revisions.json'
    records = json.loads(registry.read_text())['repositories']
    with ThreadPoolExecutor(max_workers=4) as pool:
        records = list(pool.map(refresh, records))
    seen = {normalize(record['url']) for record in records}
    inspected = set()
    while True:
        additions = []
        for record in records:
            if normalize(record['url']) in inspected:
                continue
            inspected.add(normalize(record['url']))
            modules = git(WORKSPACE / record['path'], 'show', record['revision'] + ':.gitmodules', text=True)
            if modules.returncode:
                continue
            configuration = configparser.ConfigParser()
            configuration.read_string(modules.stdout)
            for section in configuration.sections():
                url = configuration[section].get('url')
                if not url:
                    continue
                if url.startswith('../'):
                    url = urljoin(record['url'] + '/', url)
                url = url.replace('git://github.com/', 'https://github.com/').replace('git@github.com:', 'https://github.com/')
                if normalize(url) in seen:
                    continue
                seen.add(normalize(url))
                parts = urlparse(url).path.strip('/').removesuffix('.git').split('/')
                name = '-'.join(parts[-2:])
                path = Path('work/upstream') / name
                destination = WORKSPACE / path
                if not destination.exists():
                    git(WORKSPACE, 'init', '--bare', str(destination)).check_returncode()
                    git(destination, 'remote', 'add', 'origin', url).check_returncode()
                additions.append(dict(url=url, path=str(path)))
        if not additions:
            break
        with ThreadPoolExecutor(max_workers=4) as pool:
            records.extend(pool.map(refresh, additions))
    report = dict(updated_at=datetime.now().astimezone().isoformat(), repositories=records)
    registry.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    origins = json.loads((DEVICE / 'source_origins.json').read_text())
    by_url = {normalize(record['url']): record for record in records}
    merged = []
    if not args.fetch_only:
        for source in origins['sources']:
            if source['local'].startswith('ios18/') or source['local'].startswith('firmware/'):
                merged.append(merge_source(source, by_url[normalize(source['upstream_url'])]))
        (DEVICE / 'source_origins.json').write_text(json.dumps(origins, ensure_ascii=False, indent=2) + '\n')
    report['sources'] = merged
    (OUTPUT / 'latest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    current = json.loads((DEVICE / 'CURRENT.json').read_text())
    current['updated_at'] = report['updated_at']
    current['上游同步'] = dict(repositories=len(records), report='build/upstream/latest.json',
                            sources=merged, status=('上游拉取完成，源码整合进行中' if args.fetch_only else
                                    '源码冲突待处理' if any(x['conflicts'] for x in merged) else '最新提交整合完成'))
    (DEVICE / 'CURRENT.json').write_text(json.dumps(current, ensure_ascii=False, indent=2) + '\n')
    print(f'Updated {len(records)} upstream repositories.', flush=True)


if __name__ == '__main__':
    main()
