#!/usr/bin/env python3
"""Plan or remove reproducible boot-package intermediates and Python caches.

Boot images, kernels, manifests, diagnostics, system disks, Data, firmware
inputs, Android backups and source trees are retained. Apply consumes a saved
plan and checks every target's location, size and modification time.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil

INTERMEDIATES = frozenset(('KernelSeed.bin', 'firmware.bin', 'volume.img',
    'runtime.gpuaddon', 'wifi-runtime.gpuaddon', 'haptics-runtime.gpuaddon'))
RETAIN = ('ios18_gpu_v1_2026-11-07/hardware/gpu-producer-island',
    'charging-usage-buckets64-20261110/boot', 'audio-20261110/boot-lock2',
    'audio-20261110/boot-lock-state0-dsp')


def classify(workspace, path):
    if path.is_symlink() or not path.is_file():
        return None
    outputs = workspace / 'outputs/UniXNU_marble'
    if path.name in INTERMEDIATES and path.is_relative_to(outputs):
        relative = path.relative_to(outputs)
        if any(str(relative).startswith(prefix + '/') for prefix in RETAIN):
            return None
        parent = path.parent
        if (parent / 'temporary-boot.json').is_file() and (parent / 'marble_temporary_boot.img').is_file():
            return 'boot_package_intermediate'
    source = workspace / 'work/UniXNU-marble'
    if path.is_relative_to(source) and path.name.endswith('.pyc') and '__pycache__' in path.parts:
        return 'python_bytecode'
    if path.name == '.DS_Store' and (path.parent == workspace or path.is_relative_to(source)):
        return 'finder_metadata'
    return None


def plan(workspace):
    targets = []
    outputs = workspace / 'outputs/UniXNU_marble'
    for directory, children, files in os.walk(outputs):
        children[:] = [x for x in children if x not in ('.git', '.cache', 'ipsw', 'node_modules')]
        if 'temporary-boot.json' not in files or 'marble_temporary_boot.img' not in files:
            continue
        for name in sorted(INTERMEDIATES.intersection(files)):
            targets.append(Path(directory) / name)
    source = workspace / 'work/UniXNU-marble'
    for directory, children, files in os.walk(source):
        children[:] = [x for x in children if x not in ('.git', '.cache', 'venv', 'node_modules')]
        targets.extend(Path(directory) / name for name in files
                       if name == '.DS_Store' or (name.endswith('.pyc') and Path(directory).name == '__pycache__'))
    targets.append(workspace / '.DS_Store')
    entries = []
    for path in sorted(set(targets)):
        kind = classify(workspace, path)
        if kind:
            stat = path.stat()
            entries.append(dict(path=str(path.relative_to(workspace)), kind=kind,
                bytes=stat.st_size, mtime_ns=stat.st_mtime_ns))
    return dict(schema_version=1, created_at=datetime.now(timezone.utc).isoformat(),
        workspace=str(workspace), mode='plan', retain=list(RETAIN), files=entries,
        total_files=len(entries), logical_bytes=sum(x['bytes'] for x in entries),
        free_bytes_before=shutil.disk_usage(workspace).free)


def apply(workspace, report):
    if Path(report['workspace']) != workspace or report['mode'] != 'plan':
        raise ValueError('Plan workspace or state differs')
    for entry in report['files']:
        path = workspace / entry['path']
        if not path.resolve().is_relative_to(workspace) or classify(workspace, path) != entry['kind']:
            raise ValueError('Target is outside the cleanup policy: ' + str(path))
        stat = path.stat()
        if (stat.st_size, stat.st_mtime_ns) != (entry['bytes'], entry['mtime_ns']):
            raise ValueError('Target changed after planning: ' + str(path))
    for entry in report['files']:
        (workspace / entry['path']).unlink()
    for directory, children, files in os.walk(workspace / 'work/UniXNU-marble', topdown=False):
        path = Path(directory)
        if path.name == '__pycache__' and not list(path.iterdir()):
            path.rmdir()
    report.update(mode='applied', applied_at=datetime.now(timezone.utc).isoformat(),
        free_bytes_after=shutil.disk_usage(workspace).free)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    report = (apply(workspace, json.loads(args.report.read_text())) if args.apply else plan(workspace))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('mode', 'total_files', 'logical_bytes')}))


if __name__ == '__main__':
    main()
