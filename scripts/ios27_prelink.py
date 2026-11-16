#!/usr/bin/env python3
"""Stage the XnuPrelink toolkit, apply the ios27-24B119 reviewed patches, and
run fileset matching + candidate emission for the marble 24B119 migration.

The wrapper keeps the upstream toolkit pristine: every run copies
ios27/sources/XnuPrelink into <work>/toolkit, then overlays every fixture and
patch file found in patches/XnuPrelink/ios27-24B119/. It pins the authoritative
24B119 kernelcache by SHA-256, drives emit_prelink.py with the profile's root
closure and early security initializers, and writes
records/ios27-24B119/prelink-mapping.{json,md} with strict status fields.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parent.parent
WORKSPACE = REPO.parent.parent
DEFAULT_KERNELCACHE = (WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119/ipsw/24B119/'
                       'kernelcache.decompressed')
DEFAULT_REFERENCE = (WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119/tree/build/xnu/'
                     'fileset-reference/mach.release.bcm2837.unstripped.noctf')
DEFAULT_NATIVE = (WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119/tree/build/xnu/'
                  'ios-release-obj/RELEASE_ARM64_BCM2837/mach.release.bcm2837.unstripped.noctf')
PROFILE = REPO / 'profiles/os/ios-27.2-24B119/profile.toml'
PATCHES = REPO / 'patches/XnuPrelink/ios27-24B119'
SOURCE = REPO / 'ios27/sources/XnuPrelink'
PINNED_KERNELCACHE_SHA256 = '8cc8eccc429aa95397aef0ffe64b226e71b014d84190c1e3f84cf5c270a6e6bc'

try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def load_profile():
    config = tomllib.loads(PROFILE.read_text())
    prelink = config['kernel']['prelink']
    roots = list(prelink['roots'])
    initializers = prelink.get('early_security_init', [])
    if isinstance(initializers, str):
        initializers = [initializers]
    initializers = list(initializers)
    return roots, initializers, prelink['slide'], config


def stage_toolkit(work):
    """Regenerate the toolkit overlay: upstream source plus reviewed fixtures."""
    toolkit = work / 'toolkit'
    if toolkit.exists():
        shutil.rmtree(toolkit)
    shutil.copytree(SOURCE, toolkit)
    applied = []
    if PATCHES.is_dir():
        for patch_file in sorted(PATCHES.iterdir()):
            if patch_file.suffix == '.json' and patch_file.name.startswith('fileset-'):
                target = toolkit / 'fixtures' / patch_file.name
                target.parent.mkdir(exist_ok=True)
                shutil.copy2(patch_file, target)
                applied.append(f'fixture:{patch_file.name}')
            elif patch_file.suffix == '.patch':
                subprocess.run(['patch', '-s', '-p1', '-d', str(toolkit), '-i', str(patch_file)],
                               check=True)
                applied.append(f'patch:{patch_file.name}')
            elif patch_file.suffix == '.py':
                shutil.copy2(patch_file, toolkit / patch_file.name)
                applied.append(f'override:{patch_file.name}')
    return toolkit, applied


def run_emit(toolkit, args, roots, initializers, slide):
    output = args.out
    if output.exists():
        output.unlink()
    # A failed emitter must never inherit a prior candidate's success report.
    for sidecar in (output.with_suffix('.json'), output.with_suffix('.map.json')):
        sidecar.unlink(missing_ok=True)
    command = [sys.executable, str(toolkit / 'emit_prelink.py'),
               '--kernel', str(args.native), '--prelinked', str(args.kernelcache),
               f'--slide={slide}', '--output', str(output)]
    for root in roots:
        command += ['--root', root]
    for init in initializers:
        command += ['--early-security-init', init]
    if args.reference:
        command += ['--fileset-reference', str(args.reference)]
    if args.allow_missing:
        command.append('--allow-missing')
    proc = subprocess.run(command, cwd=str(toolkit), text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report = {}
    report_path = output.with_suffix('.json')
    if report_path.is_file():
        report = json.loads(report_path.read_text())
    return proc.returncode, proc.stdout, report, report_path


def summarize(report):
    missing = report.get('missing', {})
    stubbed = report.get('stubbed', {})
    return {
        'mapping_complete': report.get('mapping_complete', False),
        'runtime_qualified': report.get('runtime_qualified', False),
        'loadable': report.get('loadable', False),
        'missing_targets': sorted({t for targets in missing.values() for t in targets}),
        'missing_references': sum(sum(v.values()) for v in missing.values()),
        'stubbed_targets': sorted({t for targets in stubbed.values() for t in targets}),
        'stubbed_references': sum(sum(v.values()) for v in stubbed.values()),
        'components': sorted(set(missing) | set(stubbed)),
    }


def write_records(report, args, applied, status, log):
    records = REPO / 'records/ios27-24B119'
    records.mkdir(parents=True, exist_ok=True)
    payload = {
        'tool': 'scripts/ios27_prelink.py',
        'status': status,
        'inputs': {
            'kernelcache': str(args.kernelcache),
            'kernelcache_sha256': sha256(args.kernelcache),
            'native': str(args.native),
            'native_sha256': sha256(args.native),
            'reference': str(args.reference) if args.reference else None,
            'reference_sha256': sha256(args.reference) if args.reference else None,
        },
        'patches_applied': applied,
        'report': report,
        'summary': summarize(report),
        'source_patch_recommendations': (
            report.get('source_patch_recommendations', [])
            + [{'symbol': e['native_symbol'], 'site': e['site'],
                'note': 'board-provided symbol required in the final kernel; '
                        'deferred during reference analysis'}
               for e in report.get('deferred_sites', [])]),
        'log_tail': log[-4000:],
    }
    (records / 'prelink-mapping.json').write_text(json.dumps(payload, indent=2) + '\n')
    s = payload['summary']
    md = [
        '# ios27-24B119 fileset prelink mapping', '',
        f"- status: `{status}`",
        f"- kernelcache: `{payload['inputs']['kernelcache_sha256']}`",
        f"- native kernel: `{payload['inputs']['native_sha256']}` ({args.native.name})",
        f"- closure components: {len(report.get('closure') or [])}",
        f"- missing data references: {s['missing_references']} across {len(s['missing_targets'])} targets",
        f"- stubbed function references: {s['stubbed_references']} across {len(s['stubbed_targets'])} targets",
        f"- mapping_complete={s['mapping_complete']} runtime_qualified={s['runtime_qualified']} loadable={s['loadable']}",
        '',
        '## Outstanding targets', '',
    ]
    for t in s['missing_targets']:
        md.append(f'- data `{t}`')
    for t in s['stubbed_targets']:
        md.append(f'- function `{t}`')
    (records / 'prelink-mapping.md').write_text('\n'.join(md) + '\n')
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernelcache', type=Path, default=DEFAULT_KERNELCACHE)
    parser.add_argument('--native', type=Path,
                        help='symbol-bearing native kernel; default: release build if present, else the comparison reference')
    parser.add_argument('--reference', type=Path, default=DEFAULT_REFERENCE,
                        help='comparison kernel used only for fileset matching')
    parser.add_argument('--work', type=Path, default=REPO / 'build/ios27-prelink')
    parser.add_argument('--out', type=Path, default=REPO / 'build/ios27-prelink/fileset.macho')
    parser.add_argument('--allow-missing', action='store_true',
                        help='emit an experimental image even with unresolved references')
    args = parser.parse_args()

    if sha256(args.kernelcache) != PINNED_KERNELCACHE_SHA256:
        raise SystemExit('kernelcache is not the pinned 24B119 collection')
    if args.native is None:
        args.native = DEFAULT_NATIVE if DEFAULT_NATIVE.is_file() else args.reference
    if not args.native.is_file():
        raise SystemExit(f'native kernel absent: {args.native}')
    args.work.mkdir(parents=True, exist_ok=True)
    roots, initializers, slide, _ = load_profile()
    toolkit, applied = stage_toolkit(args.work)
    code, log, report, report_path = run_emit(toolkit, args, roots, initializers, slide)
    summary = summarize(report)
    if code != 0:
        status = 'emit-failed'
    elif summary['missing_references'] or summary['stubbed_references']:
        status = 'incomplete-mappings'
    else:
        status = 'candidate-complete' if report.get('loadable') else 'mapped-not-loadable'
    payload = write_records(report, args, applied, status, log)
    print(f'status: {status}')
    print(f"  missing: {summary['missing_references']} refs / {len(summary['missing_targets'])} targets")
    print(f"  stubbed: {summary['stubbed_references']} refs / {len(summary['stubbed_targets'])} targets")
    print(f"  report: {report_path}")
    print('  records: records/ios27-24B119/prelink-mapping.json')
    return 0 if status in ('candidate-complete', 'mapped-not-loadable') else 1


if __name__ == '__main__':
    sys.exit(main())
