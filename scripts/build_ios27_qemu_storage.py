#!/usr/bin/env python3
"""marble iOS 27.2 (24B119, arm64e, XNU 12377.121.6) QEMU virtual-storage build.

Compiles the pinned upstream VirtioMmioBlock NativeVirtioBlock driver
(4e1013eb8c45077b57f8d38d61b389fe899ea1c3) with NATIVE_VERIFIED_BLOCK
multi-policy support, the pinned VerifiedRoot verifier and XNU crypto adapter
(974f043d0c71ba2e60331483a41c6c6d35274ee3), the board-owned writable
NativeVirtioBlockData scratch-disk class, and the QemuStorage entrypoint
glue. Sources are extracted from the pinned git objects, never from working
trees.

The owner RSA3072 key is generated once under this output directory
(private half chmod 600, external only); the pinned VerifiedRoot
tools/manifest.py signs every 4096-byte block digest of the original OS and
SystemOS raw images. Both signed envelopes and the independent public trust
anchor embed into the archive through the real NativeVerifiedBlockPolicy
handoff.

Objects compile with the assembled tree's RELEASE arm64e iOS27.2 compile
records and bind original-class imports at 24B119 collection addresses by
importing scripts/build_ios27_drivers.py's actual mechanism; no host SDK or
guessed PAC is used. The archive links beside libMarbleStorage.a through the
main agent's UNIXNU_QEMU_ROOT_ARCHIVE hook. Output:

    outputs/UniXNU_marble/ios27_24B119/qemu/storage/libQemuVirtStorage.a

CLI:

    build_ios27_qemu_storage.py [--tree T] [--kernelcache K] [--output D]
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import uuid as uuid_mod

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parents[1]
OUTPUT = (WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119/qemu/storage')
RECORDS = ROOT / 'records/ios27-24B119'
PACKAGE = ROOT / 'package/ios27-qemu'

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_ios27_drivers as drv  # noqa: E402

VIRTIO_REPO = WORKSPACE / 'work/upstream/VirtioMmioBlock'
VIRTIO_PIN = '4e1013eb8c45077b57f8d38d61b389fe899ea1c3'
VIRTIO_FILES = ['NativeVirtioBlock.cpp', 'VerifiedBlockPolicy.h']
VR_REPO = WORKSPACE / 'work/UniXNU/kits/VerifiedRoot'
VR_PIN = '974f043d0c71ba2e60331483a41c6c6d35274ee3'
VR_FILES = ['src/verified_root.c', 'adapters/xnu_crypto.c',
            'include/verified_root.h', 'tools/manifest.py']
ARCHIVE_NAME = 'libQemuVirtStorage.a'

IMAGES = [
    {'name': 'os', 'role': 'root',
     'uuid': 'E0577111-0369-4BCF-A4E5-59B612AA9E31',
     'bytes': 9265217536,
     'sha256': '60dbb74c46a598e60707e6e5048ee29d3473e88bb5d2862899fa9818d0997717',
     'path': (WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119/ipsw/24B119'
              / '141-08279-215.dmg')},
    {'name': 'systemos', 'role': 'cache',
     'uuid': '46885FBD-E14C-4630-805A-8AC4BFD8D326',
     'bytes': 6178209792,
     'sha256': 'fa3596065f16e955a96b2671718ced0992e4957e6d1ec28fc87465bf573ec42f',
     'path': (WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119/ipsw/24B119'
              / '141-10128-227.dmg')},
]
GENERATION = 1

REQUIRED_EXPORTS = [
    '_native_qemu_storage_metaclass_init', '_native_qemu_storage_start',
    '_native_qemu_volume_device', '_native_qemu_root_authorized',
    '_native_qemu_cache_authorized',
    '__GLOBAL__sub_I_NativeVirtioBlock.cpp',
    '__GLOBAL__sub_I_NativeVirtioBlockData.cpp',
    '_native_verified_block_root_authorized',
    '_native_verified_block_volume_device',
    '_native_verified_block_manifest_sha384',
    '_native_virtio_ready',
    '_vr_open', '_vr_verify_block', '_vr_xnu_crypto',
    '_verified_root_policies', '_verified_root_policy_count',
    '_verified_root_key', '_verified_root_manifest',
    '_verified_root_manifest_bytes',
    '__ZN17NativeVirtioBlock10gMetaClassE',
    '__ZN21NativeVirtioBlockData10gMetaClassE',
    '_native_qemu_data_ready',
]
VTABLE_AUDIT = [  # (object stem, vtable symbol) -> original IOBlockStorageDevice table
    ('NativeVirtioBlock', '__ZTV17NativeVirtioBlock'),
    ('NativeVirtioBlockData', '__ZTV21NativeVirtioBlockData'),
]


def fail(message):
    raise SystemExit(message)


def require(value, message):
    if not value:
        fail(message)


def sha256(path_or_data):
    digest = hashlib.sha256()
    if isinstance(path_or_data, (bytes, bytearray)):
        digest.update(path_or_data)
        return digest.hexdigest()
    with Path(path_or_data).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def git(repo, *args, binary=False):
    result = subprocess.run(['git', '-C', str(repo)] + list(args), check=True,
                            capture_output=True)
    return result.stdout if binary else result.stdout.decode().strip()


def extract_pin(repo, pin, paths, out, report):
    """Extract every path at `pin` from `repo` into out; record blob pins."""
    require(subprocess.run(['git', '-C', str(repo), 'cat-file', '-t', pin],
                           capture_output=True, text=True).stdout.strip() == 'commit',
            f'{repo.name}: {pin} is not a commit object')
    src = out / 'src' / repo.name
    for rel in paths:
        blob = git(repo, 'show', f'{pin}:{rel}', binary=True)
        target = src / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(blob)
        report.setdefault('source_pins', {}).setdefault(repo.name, {
            'repository': str(repo), 'commit': pin, 'files': {}})['files'][rel] = {
            'git_blob_sha1': git(repo, 'rev-parse', f'{pin}:{rel}'),
            'sha256': hashlib.sha256(blob).hexdigest(),
            'staged': str(target)}
    return src


def owner_keys(out, report):
    """Owner RSA3072 pair; private key stays chmod 600 inside `out` only."""
    keys = out / 'keys'
    keys.mkdir(parents=True, exist_ok=True)
    os.chmod(keys, 0o700)
    private, public = keys / 'owner-private.pem', keys / 'owner-public.pem'
    if not private.exists():
        fd = os.open(private, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(fd)
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt',
                        'rsa_keygen_bits:3072', '-out', str(private)],
                       check=True, capture_output=True)
    os.chmod(private, 0o600)
    subprocess.run(['openssl', 'pkey', '-in', str(private), '-pubout',
                    '-out', str(public)], check=True, capture_output=True)
    require(private.stat().st_mode & 0o777 == 0o600,
            'owner private key must remain mode 600')
    report['owner_key'] = {
        'private': str(private), 'private_mode': oct(0o600),
        'public': str(public), 'public_sha256': sha256(public),
        'private_copied_elsewhere': False}
    return private, public


def load_manifest_tool(src_dir):
    spec = importlib.util.spec_from_file_location(
        'vr_manifest', src_dir / 'tools/manifest.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def valid_manifest_record(path, image):
    if not path.is_file():
        return False
    record = json.loads(path.read_text())
    return (record.get('image_bytes') == image['bytes'] and
            record.get('image_sha256') == image['sha256'] and
            record.get('volume_uuid') == image['uuid'] and
            record.get('generation') == GENERATION and
            record.get('block_bytes') == 4096 and
            record.get('block_count') == image['bytes'] // 4096)


def build_manifests(manifest_tool, private, public, out, report):
    """Sign every 4096-byte block digest of each input through the pinned tool."""
    manifests = {}
    report['manifests'] = {}
    expected_anchor = manifest_tool.public_anchor(public)
    require(len(expected_anchor) == 395,
            'public anchor must be VROOTKEY + RSA3072 modulus + exponent')
    for image in IMAGES:
        require(image['path'].is_file(), f"input image absent: {image['path']}")
        require(image['path'].stat().st_size == image['bytes'],
                f"input image length differs from its declared identity: {image['path']}")
        target = out / 'manifests' / image['name']
        record_path = target / 'manifest.json'
        envelope = target / 'manifest.bin'
        anchor = target / 'trusted-key.bin'
        reused = (valid_manifest_record(record_path, image) and
                  envelope.is_file() and anchor.is_file() and
                  anchor.read_bytes() == expected_anchor and
                  envelope.stat().st_size ==
                  544 + image['bytes'] // 4096 * 32)
        if reused:
            record = json.loads(record_path.read_text())
        else:
            with image['path'].open('rb') as stream:
                record = manifest_tool.make(
                    stream, image['bytes'], private, public,
                    image['uuid'], GENERATION, target)
        require(record['image_sha256'] == image['sha256'],
                f"{image['name']}: signed image digest differs from declared identity")
        require(record['volume_uuid'] == image['uuid'],
                f"{image['name']}: signed volume identity differs from declared UUID")
        require(record['image_bytes'] == image['bytes'] and
                record['block_count'] == image['bytes'] // 4096 and
                record['block_bytes'] == 4096 and
                envelope.stat().st_size == 544 + record['block_count'] * 32,
                f"{image['name']}: signed envelope shape invalid")
        # Envelope layout + spot verification: header fields, rehashed table,
        # openssl-verified signature, and sampled image blocks.
        blob_head = envelope.open('rb').read(160)
        (magic, version, header_bytes, block_bytes, zero, image_bytes, count,
         volume, image_sha, table_sha, key_sha, generation) = struct.unpack(
            '<8s4I2Q16s32s32s32sQ', blob_head)
        require(magic == b'VROOT01\0' and version == 1 and header_bytes == 160 and
                block_bytes == 4096 and zero == 0 and image_bytes == image['bytes'] and
                count == record['block_count'] and generation == GENERATION,
                f"{image['name']}: signed header fields invalid")
        require(volume == uuid_mod.UUID(image['uuid']).bytes and
                image_sha.hex() == image['sha256'],
                f"{image['name']}: signed identity fields invalid")
        table = envelope.read_bytes()[544:]
        require(len(table) == count * 32,
                f"{image['name']}: digest table extent invalid")
        require(hashlib.sha256(table).digest() == table_sha,
                f"{image['name']}: rehashed digest table differs from the signed header")
        rng = random.Random(0x24B119)
        sampled = {0, 1, count // 3, count // 2, count - 2, count - 1}
        sampled |= {rng.randrange(count) for _ in range(26)}
        failures = []
        with image['path'].open('rb') as stream:
            for index in sorted(sampled):
                stream.seek(index * 4096)
                if hashlib.sha256(stream.read(4096)).digest() != \
                        table[index * 32:index * 32 + 32]:
                    failures.append(index)
        require(not failures,
                f"{image['name']}: {len(failures)} sampled blocks differ from signed digests")
        subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(public),
                        '-signature', str(target / 'signature.bin'),
                        str(target / 'header.bin')], check=True, capture_output=True)
        require(anchor.read_bytes() == expected_anchor and
                hashlib.sha256(anchor.read_bytes()).digest() == key_sha,
                f"{image['name']}: public trust anchor does not match the signed header")
        manifests[image['name']] = {'envelope': envelope, 'anchor': anchor,
                                    'record': record, 'reused': reused}
        report['manifests'][image['name']] = {
            'envelope': str(envelope), 'envelope_sha256': sha256(envelope),
            'envelope_bytes': envelope.stat().st_size,
            'block_count': record['block_count'],
            'volume_uuid': record['volume_uuid'],
            'generation': record['generation'],
            'image_sha256': record['image_sha256'],
            'hash_table_sha256': record['hash_table_sha256'],
            'trusted_key_sha256': record['trusted_key_sha256'],
            'signature_algorithm': record['signature_algorithm'],
            'sampled_blocks_verified': len(sampled),
            'signature_reverified_openssl': True,
            'reused': reused}
    anchors = {m['anchor'].read_bytes() for m in manifests.values()}
    require(len(anchors) == 1,
            'both disks must share the single independent trust anchor')
    report['trust_anchor_sha256'] = sha256(manifests['os']['anchor'])
    report['trust_anchor_bytes'] = len(manifests['os']['anchor'].read_bytes())
    return manifests


def stage_include(tree, src_dirs, out):
    include = out / 'include'
    storage = include / 'IOKit/storage'
    storage.mkdir(parents=True, exist_ok=True)
    reference = drv.storage_reference(tree)
    require(reference is not None, 'IOStorageFamily reference source absent')
    for header in reference.glob('*.h'):
        (storage / header.name).write_bytes(header.read_bytes())
    sdk = Path(subprocess.check_output(
        ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    (include / 'TargetConditionals.h').write_bytes(
        (sdk / 'usr/include/TargetConditionals.h').read_bytes())
    for src_dir in src_dirs:
        for name in ('verified_root.h', 'VerifiedBlockPolicy.h'):
            found = list(src_dir.rglob(name))
            if found:
                (include / name).write_bytes(found[0].read_bytes())
    return include


def emit_manifest_assembly(manifests, out):
    """Policy table + embedded envelopes + independent public trust anchor."""
    os_env = manifests['os']['envelope']
    sys_env = manifests['systemos']['envelope']
    anchor = manifests['os']['anchor']
    source = out / 'generated/manifests.S'
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_text(f'''// Generated signed-policy handoff for NativeVirtioBlock
// NATIVE_VERIFIED_BLOCK_MULTI_POLICY. Envelopes and the public anchor are the
// outputs of pinned VerifiedRoot tools/manifest.py; the private owner key is
// never part of this archive.
.section __QEMUPOLICY,__const
.p2align 12
.globl _qemu_os_manifest
.globl _verified_root_manifest
_qemu_os_manifest:
_verified_root_manifest:
.incbin "{os_env}"
.globl _qemu_os_manifest_end
_qemu_os_manifest_end:
.p2align 12
.globl _qemu_systemos_manifest
_qemu_systemos_manifest:
.incbin "{sys_env}"
.globl _qemu_systemos_manifest_end
_qemu_systemos_manifest_end:
.p2align 4
.globl _verified_root_key
_verified_root_key:
.incbin "{anchor}"
.p2align 3
.section __DATA_CONST,__const
.p2align 3
.globl _verified_root_policies
_verified_root_policies:
.quad _qemu_os_manifest
.quad _qemu_os_manifest_end - _qemu_os_manifest
.quad _qemu_systemos_manifest
.quad _qemu_systemos_manifest_end - _qemu_systemos_manifest
.globl _verified_root_policy_count
_verified_root_policy_count:
.quad 2
.globl _verified_root_manifest_bytes
_verified_root_manifest_bytes:
.quad _qemu_os_manifest_end - _qemu_os_manifest
''')
    obj = out / 'obj/manifests.o'
    obj.parent.mkdir(parents=True, exist_ok=True)
    drv.assemble(source, obj)
    return obj


def audit_vtable(obj, table_symbol, collection, original_ap, report):
    """Compiled subclass vtable signature must equal the original
    IOBlockStorageDevice table's 24B119 authenticated entries slot-for-slot."""
    blob = Path(obj).read_bytes()
    info = drv.macho_obj(blob)
    symbols = drv.object_symbols(blob, info)
    tables = drv.owned_vtables(blob, info, symbols, None)
    entries = tables.get(table_symbol)
    require(entries, f'{table_symbol}: no authenticated vtable in {obj}')
    original = drv.vtable_entries(collection, original_ap)
    differences = []
    for i, ((site, rel), expected) in enumerate(
            zip(entries, original + [None] * len(entries))):
        if expected is None:
            differences.append({'slot': i, 'evidence': 'extra slot'})
            continue
        want = (expected['key'], expected['address_diversity'],
                expected['diversity'])
        got = (rel['key'], rel['address_diversity'], rel['diversity'])
        if got != want:
            differences.append({'slot': i, 'expected': want, 'got': got})
    if len(entries) != len(original):
        differences.append({'evidence': f'{len(entries)} slots != {len(original)}'})
    report.setdefault('vtable_audit', {})[table_symbol] = {
        'object': str(obj), 'slots': len(entries),
        'original_slots': len(original),
        'differences': differences,
        'status': 'verified' if not differences else 'pending'}
    return not differences


def defined_symbols(obj):
    blob = Path(obj).read_bytes()
    info = drv.macho_obj(blob)
    out = set()
    for s in drv.object_symbols(blob, info):
        if s['section'] and s['kind'] & 0xe0 == 0 and s['kind'] & 0xe == 0xe:
            out.add(s['name'])
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', default=str(drv.DEFAULT_TREE))
    parser.add_argument('--kernelcache', default=str(drv.DEFAULT_KERNELCACHE))
    parser.add_argument('--output', default=str(OUTPUT))
    args = parser.parse_args()

    tree, kernelcache = Path(args.tree).resolve(), Path(args.kernelcache).resolve()
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = {'family': 'ios27-qemu-storage', 'archive': str(out / ARCHIVE_NAME),
              'target': drv.TARGET, 'status': 'incomplete',
              'build_source_revision': json.loads(
                  (RECORDS / 'build-source.json').read_text())['revision'],
              'sources': [], 'source_hashes': {}, 'objects': [],
              'missing': [], 'failed': [], 'pending': []}
    log = out / 'build.log'

    def note(line):
        with log.open('a') as stream:
            stream.write(line + '\n')
        print(line, flush=True)

    drv.import_xnuprelink(tree)
    collection = drv.Collection(kernelcache)
    report['kernelcache_sha256'] = collection.sha256

    # 1. Pinned upstream git objects.
    virtio_src = extract_pin(VIRTIO_REPO, VIRTIO_PIN, VIRTIO_FILES, out, report)
    vr_src = extract_pin(VR_REPO, VR_PIN, VR_FILES, out, report)
    report['source_pins']['VirtioMmioBlock']['component'] = \
        'https://github.com/UniXNU/VirtioMmioBlock.git'
    report['source_pins']['VerifiedRoot']['component'] = \
        'https://github.com/UniXNU/VerifiedRoot.git'

    # 2. Owner key + signed manifests (inputs read in place, never copied).
    private, public = owner_keys(out, report)
    manifests = build_manifests(load_manifest_tool(vr_src), private, public,
                                out, report)

    # 3. Stage IOKit/storage headers + compile the five units.
    include = stage_include(tree, [virtio_src, vr_src], out)
    extra_includes = [include, virtio_src, vr_src / 'include',
                      ROOT / 'package', PACKAGE]
    cxx_record = drv.compile_record(tree, '.cpp')
    c_record = drv.compile_record(tree, '.c')
    units = [
        (virtio_src / 'NativeVirtioBlock.cpp', cxx_record,
         ['-DNATIVE_VERIFIED_BLOCK=1', '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1',
          '-include', str(PACKAGE / 'QemuVmCompatibility.h')]),
        (PACKAGE / 'NativeVirtioBlockData.cpp', cxx_record, []),
        (PACKAGE / 'QemuStorage.cpp', cxx_record, []),
        (vr_src / 'src/verified_root.c', c_record, []),
        (vr_src / 'adapters/xnu_crypto.c', c_record, []),
    ]
    objects = []
    for source, record, extra in units:
        require(source.is_file(), f'source missing: {source}')
        obj = out / 'obj' / f'{source.stem}.o'
        try:
            drv.compile_source(record, source, obj, extra_includes,
                               extra_args=extra)
        except subprocess.CalledProcessError as e:
            log_path = out / 'obj' / f'{source.stem}.compile.log'
            log_path.write_bytes(e.output)
            report['failed'].append({'source': str(source), 'log': str(log_path),
                                     'error': e.output.decode(errors='replace')})
            continue
        blob = obj.read_bytes()
        final = obj
        if drv.has_mod_init(blob, drv.macho_obj(blob)):
            final = obj.with_suffix('.late.o')
            drv.late_object(obj, final, source.name)
        objects.append(final)
        report['objects'].append(str(final))
        report['sources'].append(str(source))
        report['source_hashes'][str(source)] = sha256(source)
        note(f'compiled {source.name} -> {final.name}')

    # 4. Signed-policy handoff object.
    objects.append(emit_manifest_assembly(manifests, out))
    report['objects'].append(str(out / 'obj/manifests.o'))

    # 5. Same 24B119 binding mechanism as the driver builder.
    derived_report, derived_all = drv.derive_abi(kernelcache, out_dir=out / 'abi')
    for name, info in derived_report.items():
        if info.get('status') == 'pending':
            report['pending'].append({'class': name,
                                      'evidence': info['evidence']})
    bindings, pending = {}, []
    for obj in objects:
        if obj.name == 'manifests.o':
            continue
        bound, pend, _, _, _ = drv.resolve_object_bindings(
            obj, derived_all, collection)
        bindings.update(bound)
        pending += pend
    report['pending'] += pending
    report['bound'] = {k: hex(v) for k, v in bindings.items()}
    if bindings:
        for asm in drv.emit_bindings(bindings, out):
            bo = Path(asm).with_suffix('.o')
            drv.assemble(asm, bo)
            objects.append(bo)
            report['objects'].append(str(bo))

    # 6. Subclass vtables audited against the verified base ABI record.
    abi_path = (WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119'
                / 'marble-storage/storage-abi.json')
    if abi_path.is_file():
        abi = json.loads(abi_path.read_text())
        require(abi.get('status') == 'verified',
                'marble-storage base ABI is not verified')
        original_ap = int(abi['original_table'], 0) + 0x10
        report['storage_base'] = abi
        for stem, table in VTABLE_AUDIT:
            obj = out / 'obj' / f'{stem}.o'
            if not obj.is_file():
                report['pending'].append({'vtable_audit': table,
                                          'evidence': f'{stem}.o not compiled'})
                continue
            if not audit_vtable(obj, table, collection, original_ap, report):
                report['pending'].append({'vtable_audit': table})
    else:
        report['pending'].append({'vtable_audit': 'all',
                                  'evidence': f'{abi_path} absent'})

    # 7. Export surface required by package/MarbleVirtualStorage.h contract.
    defined = set()
    for obj in objects:
        defined |= defined_symbols(obj)
    missing_exports = [s for s in REQUIRED_EXPORTS if s not in defined]
    report['exports'] = {s: s in defined for s in REQUIRED_EXPORTS}
    if missing_exports:
        report['pending'].append({'exports': missing_exports,
                                  'evidence': 'required module symbols absent'})

    # 8. Archive only when nothing is pending.
    blockers = report['missing'] + report['failed'] + report['pending']
    archive = out / ARCHIVE_NAME
    if blockers:
        report['status'] = 'pending'
    else:
        archive.unlink(missing_ok=True)
        subprocess.check_call(['xcrun', 'ar', 'rcs', str(archive)]
                              + [str(o) for o in objects])
        report['archive_sha256'] = sha256(archive)
        report['archive_bytes'] = archive.stat().st_size
        report['status'] = 'built'
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')

    # 9. Repository record (the only shared path this build writes).
    record = dict(report)
    record['qemu_acceptance'] = (
        'not run: this build produces host-side evidence only; QEMU and '
        'hardware runs belong to the main agent')
    record['writable_scratch'] = {
        'status': 'implemented_unrun',
        'class': 'NativeVirtioBlockData',
        'protocol': 'VIRTIO_BLK_T_OUT writes, negotiated VIRTIO_BLK_F_FLUSH, '
                    'VIRTIO_BLK_F_RO absent required',
        'root_authentication': 'rejected: the class has no verification '
                               'policy and fails the NativeVirtioBlock '
                               'provider-chain cast',
        'remaining_work': [
            'main agent attaches qemu/disks/scratch-data.qcow2 as a writable '
            'virtio-blk-device and runs the QEMU acceptance pass',
            'guest-level mount/format of the scratch Data volume is not yet '
            'covered by an automated acceptance check']}
    RECORDS.mkdir(parents=True, exist_ok=True)
    (RECORDS / 'qemu-storage-build.json').write_text(
        json.dumps(record, indent=2) + '\n')
    note(json.dumps({'status': report['status'],
                     'archive': str(archive) if report['status'] == 'built' else None,
                     'pending': len(report['pending'])}, indent=2))
    return 0 if report['status'] == 'built' else 1


if __name__ == '__main__':
    sys.exit(main())
