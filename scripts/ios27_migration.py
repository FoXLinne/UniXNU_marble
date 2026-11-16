#!/usr/bin/env python3
"""Resume the marble 24B119 migration from recorded source and image inputs."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone, timedelta
import hashlib
import fcntl
import fnmatch
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tomllib
import tempfile
import traceback
import urllib.request
import zipfile
import zlib
from types import SimpleNamespace

from marble_sources import copy_sources

DEVICE = Path(__file__).resolve().parents[1]
WORKSPACE = DEVICE.parents[1]
SOURCE = DEVICE / 'ios27/source'
RECORDS = DEVICE / 'records/ios27-24B119'
PROFILE = DEVICE / 'profiles/os/ios-27.2-24B119/profile.toml'
DEFAULT_OUTPUT = WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119'
TZ = timezone(timedelta(hours=8))


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, prefix=path.name + '.', delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(json.dumps(data, ensure_ascii=False, indent=2) + '\n')
    temporary.replace(path)


def record_stage(name, status, **details):
    lock_path = DEVICE / 'build/ios27-progress.lock'
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        _record_stage(name, status, **details)


def _record_stage(name, status, **details):
    progress = json.loads((RECORDS / 'progress.json').read_text())
    progress['updated_at'] = datetime.now(TZ).isoformat()
    progress['stages'][name] = {'status': status, **details}
    save(RECORDS / 'progress.json', progress)
    current = json.loads((DEVICE / 'CURRENT.json').read_text())
    current['updated_at'] = progress['updated_at']
    current['migration'].update(stage=name, stage_status=status, status='in-progress')
    current['migration']['last_stage_result'] = details
    save(DEVICE / 'CURRENT.json', current)


def execute(command, cwd=None, env=None, log=None):
    print('run:', ' '.join(map(str, command)), flush=True)
    if log:
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        with Path(log).open('w') as stream:
            subprocess.run(command, cwd=cwd, env=env, stdout=stream,
                           stderr=subprocess.STDOUT, check=True)
    else:
        subprocess.run(command, cwd=cwd, env=env, check=True)


def ipsw_context(out):
    sys.path.insert(0, str(SOURCE / 'scripts'))
    from unixnu.ipsw import Ipsw
    profile = tomllib.loads(PROFILE.read_text())
    context = SimpleNamespace(board=profile, out_root=out)
    return context, Ipsw(context, spec=profile['ipsw']['url'])


def fetch(out, system=False):
    from_context, archive = ipsw_context(out)
    from unixnu.ipsw import kernelcache
    profile = from_context.board
    for name in profile['ipsw']['fetch']:
        archive.extract(name)
    cache = kernelcache(from_context, archive)
    magic, cpu, subtype, kind = struct.unpack('<4I', cache.read_bytes()[:16])
    if (magic, cpu, subtype & 0xffffff, kind) != (0xfeedfacf, 0x100000c, 2, 12):
        raise ValueError('24B119 requires an arm64e MH_FILESET kernel collection')
    expected = profile['kernel']['prelink']['kernelcache_sha256']
    if digest(cache) != expected:
        raise ValueError('24B119 kernel collection digest differs from the profile')
    record_stage('kernelcache', 'complete', kernelcache_path=str(cache), kernelcache_sha256=expected)
    if system:
        images = {}
        for component in ('OS', 'Cryptex1,SystemOS'):
            stream_component(archive, component)
            path = archive.disk_image(component)
            images[component] = {'path': str(path), 'sha256': digest(path),
                                 'bytes': path.stat().st_size}
        save(RECORDS / 'system-images.json', {'build': '24B119', 'images': images})
        record_stage('system_images', 'complete', images_record='records/ios27-24B119/system-images.json')


def stream_component(archive, component):
    """Stream stored IPSW members with resumable ranges and ZIP CRC verification."""
    member = archive.member(component)
    entry = archive.zip.getinfo(member)
    if not archive.remote or entry.compress_type != zipfile.ZIP_STORED:
        return archive.extract(component)
    destination = archive.cache / Path(member).name
    record = destination.with_suffix(destination.suffix + '.json')
    if destination.is_file() and record.is_file():
        metadata = json.loads(record.read_text())
        if (destination.stat().st_size == entry.file_size and metadata.get('member') == member
                and metadata.get('size') == entry.file_size and digest(destination) == metadata.get('sha256')):
            return destination
    archive.cache.mkdir(parents=True, exist_ok=True)
    partial = destination.with_suffix(destination.suffix + '.partial')
    have = partial.stat().st_size if partial.is_file() else 0
    if have > entry.file_size:
        raise ValueError(f'partial image is larger than the IPSW member: {partial}')
    archive.stream.seek(entry.header_offset)
    header = archive.stream.read(30)
    if header[:4] != b'PK\x03\x04':
        raise ValueError('IPSW member has an invalid local ZIP header')
    name_bytes, extra_bytes = struct.unpack_from('<HH', header, 26)
    start = entry.header_offset + 30 + name_bytes + extra_bytes
    checksum, hash_state = 0, hashlib.sha256()
    if have:
        with partial.open('rb') as previous:
            while chunk := previous.read(8 << 20):
                checksum = zlib.crc32(chunk, checksum)
                hash_state.update(chunk)
    if have < entry.file_size:
        request = urllib.request.Request(archive.spec, headers={
            'Range': f'bytes={start + have}-{start + entry.file_size - 1}'})
        print(f'stream: {component} {have}/{entry.file_size} bytes', flush=True)
        with urllib.request.urlopen(request, timeout=120) as response, partial.open('ab') as target:
            if response.status != 206 or not response.headers.get('Content-Range', '').startswith(f'bytes {start + have}-'):
                raise ValueError('IPSW server returned an unexpected byte range')
            checkpoint = have // (256 << 20)
            while chunk := response.read(8 << 20):
                if have + len(chunk) > entry.file_size:
                    raise ValueError('IPSW member range exceeds its recorded length')
                target.write(chunk)
                checksum = zlib.crc32(chunk, checksum)
                hash_state.update(chunk)
                have += len(chunk)
                if have // (256 << 20) != checkpoint:
                    checkpoint = have // (256 << 20)
                    print(f'stream: {component} {have}/{entry.file_size} bytes', flush=True)
    if have != entry.file_size or checksum != entry.CRC:
        raise ValueError(f'IPSW member length or CRC differs: {member}')
    expected = archive.config.get('sha256', {}).get(Path(member).name)
    if expected and hash_state.hexdigest() != expected:
        raise ValueError(f'IPSW member differs from its SHA-256 pin: {member}')
    partial.replace(destination)
    save(record, {'component': component, 'member': member, 'size': have,
                  'sha256': hash_state.hexdigest(), 'source': 'remote', 'zip_crc32': checksum})
    return destination


def references(out):
    """Use separate pinned Apple trees; existing iOS 17/18 trees keep their identity."""
    specs = {s['name']: s for s in tomllib.loads((SOURCE / 'feeds.toml').read_text())['source']
             if s['kind'] == 'refer' and s['name'] != 'edk2'}
    profile = tomllib.loads(PROFILE.read_text())
    for source in profile.get('source', []):
        specs[source['name']] = source
    specs['xnu'].update(commit=profile['os']['xnu_revision'], ref='xnu-' + profile['os']['xnu_version'])
    cached = json.loads((DEVICE / 'upstream_revisions.json').read_text())['repositories']
    by_url = {r['url'].rstrip('/').removesuffix('.git').lower(): WORKSPACE / r['path'] for r in cached}

    def prepare(item):
        name, spec = item
        target = out / 'references' / name
        revision = spec['commit']
        url = spec['git']
        reference = by_url.get(url.rstrip('/').removesuffix('.git').lower())
        if not (target / '.git').exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            execute(['git', 'init', '--quiet', str(target)])
            execute(['git', '-C', str(target), 'remote', 'add', 'origin', url])
            execute(['git', '-C', str(target), 'config', 'remote.origin.promisor', 'true'])
            execute(['git', '-C', str(target), 'config', 'remote.origin.partialclonefilter', 'blob:none'])
            if reference and reference.exists():
                objects = subprocess.check_output(['git', '-C', str(reference), 'rev-parse',
                                                   '--path-format=absolute', '--git-path', 'objects'], text=True).strip()
                (target / '.git/objects/info/alternates').write_text(objects + '\n')
        present = subprocess.run(['git', '-C', str(target), 'cat-file', '-e', revision + '^{commit}'],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if present.returncode:
            execute(['git', '-C', str(target), 'fetch', '--depth=1', '--filter=blob:none', 'origin', revision])
        result = subprocess.run(['git', '-C', str(target), 'rev-parse', '--verify', 'HEAD'],
                                text=True, capture_output=True)
        head = result.stdout.strip() if result.returncode == 0 else ''
        initialized = target / '.git/marble-reference.json'
        if not initialized.is_file():
            # A no-checkout clone has HEAD but no populated index or source files.
            # This directory was created as this migration's generated reference.
            execute(['git', '-C', str(target), 'checkout', '--quiet', '--force', '--detach', revision])
            save(initialized, {'revision': revision, 'repository': url})
        elif head != revision:
            changes = subprocess.check_output(['git', '-C', str(target), 'status', '--porcelain'], text=True)
            if changes:
                raise RuntimeError(f'reference has local changes: {target}')
            execute(['git', '-C', str(target), 'checkout', '--quiet', '--detach', revision])
            save(initialized, {'revision': revision, 'repository': url})
        return name, {'path': str(target), 'repository': url, 'revision': revision}

    with ThreadPoolExecutor(max_workers=4) as pool:
        result = dict(pool.map(prepare, specs.items()))
    save(RECORDS / 'references.json', result)
    record_stage('references', 'complete', reference_record='records/ios27-24B119/references.json')
    return result


def link(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_symlink():
        if target.resolve() == Path(source).resolve():
            return
        target.unlink()
    if target.exists():
        if target.is_dir() and not any(target.iterdir()):
            target.rmdir()
        else:
            raise FileExistsError(target)
    target.symlink_to(Path(source).resolve(), target_is_directory=Path(source).is_dir())


def prepare(out):
    tree = out / 'tree'
    # Build scripts resolve their tree from __file__; give them actual copies.
    copy_sources(SOURCE, tree, ('out', 'build', '.cache', 'refer', 'drivers', 'kits'))
    for category, names in {
        'drivers': ('ArmCpu', 'ArmDTPlatform', 'DarwinBoot', 'Adreno740', 'AdrenoMetal',
                    'ShengUfs', 'GopFramebuffer', 'RamBlock', 'SoftwareAES', 'AppleKeyStoreState',
                    'QcomGeniSpi', 'Ath12kWifi'),
        'kits': ('ArmSmmu4K', 'AdrenoPM4', 'AdrenoFirmware', 'XnuPrelink'),
    }.items():
        for name in names:
            source = DEVICE / 'ios27/sources' / name
            if source.is_dir():
                if name == 'DarwinBoot':
                    override = out / 'component-overrides/DarwinBoot'
                    copy_sources(source, override)
                    loader_patches = [DEVICE / 'patches/DarwinBoot' / name for name in
                                      ('ios27-resident-memory.patch', 'ios27-resident-reader.patch',
                                       'ios27-runtime-capacity.patch',
                                       'ios27-resident-cache-skip.patch',
                                       'ios27-handoff-firmware-context.patch',
                                       'ios27-gic-awake-handoff.patch')]
                    for patch in loader_patches:
                        execute(['git', 'apply', '--check', str(patch)], cwd=override)
                        execute(['git', 'apply', str(patch)], cwd=override)
                    save(RECORDS / 'loader-memory.json', {
                        'source': str(source), 'override': str(override),
                        'patches': [{'path': str(p), 'sha256': digest(p)} for p in loader_patches],
                        'contract': 'ROOT_STAGE_DRAM_TABLE v1',
                        'policy': 'eight-core retained boot; reserve outside VM DRAM; retain firmware return allocation area',
                        'device_result': 'pending'})
                    source = override
                link(source, tree / category / name)
    # Compatibility names used by the shared source storage/display builders.
    for alias, name in (('NativeUfs', 'ShengUfs'), ('NativeGeniSpi', 'QcomGeniSpi')):
        source = tree / 'drivers' / name
        if source.exists():
            link(source, tree / 'drivers' / alias)
    link(prepare_board_sources(out), tree / 'kits/marble')
    refs = references(out)
    for name, spec in refs.items():
        link(spec['path'], tree / 'refer' / name)
    for name in ('build', '.cache'):
        (tree / name).mkdir(exist_ok=True)
    generate_board_headers(tree)
    if (out / 'gpu/coreanimation-target.json').is_file():
        from build_ios27_gpu_userspace import plugin_target_source
        plugin_target_source(out)
    # Board builders imported from their repository location keep DEVICE stable.
    record_stage('build_tree', 'prepared', tree=str(tree))
    return tree


def generate_board_headers(tree):
    """Share the board's DSI command implementation with its kernel adapter."""
    source = (DEVICE / 'package/MarblePkg/RootDisplay.c').read_text()
    begin, end = source.index('STATIC VOID PanelPublishState('), source.index('/* Prefix ABI')
    snippet = source[begin:end]
    for old, new in [('(VOID *)(UINTN)0xae94000', 'nativeDsi'),
                     ('(VOID *)(UINTN)0xae00000', 'nativeMdp'),
                     ('(CONST VOID *)(UINTN)0xae94004', '(CONST VOID *)(nativeDsi+1)'),
                     ('PANEL_MODE_COMMIT *Commit=Context;', 'PANEL_MODE_COMMIT *Commit=(PANEL_MODE_COMMIT *)Context;'),
                     ('gBS->Stall(', 'NativePanelDelay(')]:
        snippet = snippet.replace(old, new)
    output = tree / 'build/marble-boot/MarblePanelCommands.inc'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(snippet)


def prepare_board_sources(out):
    """Separate 24B119 runtime policies from the shared board mount implementation."""
    target = out / 'board-overrides/marble'
    target.mkdir(parents=True, exist_ok=True)
    for source in (DEVICE / 'package').iterdir():
        if source.name not in ('MarbleSystemOS.c', 'MarbleUiPolicy.c', 'MarbleRuntime18.cpp', 'MarbleMetal.c'):
            link(source, target / source.name)
    metal=(DEVICE/'package/MarbleMetal.c').read_text()
    replacements={
        '/System/Library/EventTimingProfiles/N841.Default.plist':'/System/Library/EventTimingProfiles/N104.Default.plist',
        '/Library/Audio/Tunings/N841/Haptics/hapticengineconfig.plist':'/Library/Audio/Tunings/N104/Haptics/hapticengineconfig.plist'}
    for before,after in replacements.items():
        if metal.count(before)==1:
            metal=metal.replace(before,after)
        elif metal.count(before)!=0 or metal.count(after)!=1:
            raise ValueError('Target runtime overlay path changed: '+before)
    (target/'MarbleMetal.c').write_text(metal)
    system = (DEVICE / 'package/MarbleSystemOS.c').read_text()
    start = system.index('struct marble_ui_exit {')
    end = system.index('int native_marble_bindfs_allowed(void)\n{', start)
    generated = system[:start] + system[end:]
    storage_profile=tomllib.loads(PROFILE.read_text())['root']
    legacy_system_uuid='const char *uuid = "9F1E9F09-B2A0-4844-98CB-E1025B99C4CE";'
    if generated.count(legacy_system_uuid)!=1:
        raise ValueError('SystemOS mount UUID declaration changed')
    generated=generated.replace(legacy_system_uuid,
        'const char *uuid = "'+storage_profile['systemos_volume_uuid']+'";')
    generated += ('\n/* CoreEntitlements validates its stack query against the original 16 KiB ABI. */\n'
                  '__attribute__((used)) const uint32_t native_ios27_ce_page_shift = 14;\n')
    (target / 'MarbleSystemOS.c').write_text(generated)
    # The version-qualified translation units are selected by runtime_sources
    # in build_ios27_drivers. Keep the legacy manifest's slot declaration-only.
    (target / 'MarbleUiPolicy.c').write_text(
        '/* 24B119 policy implementation: package/ios27-runtime/MarbleUiPolicy27.c */\n'
        'typedef int marble_ios27_policy_translation_unit;\n')
    runtime = (DEVICE / 'package/MarbleRuntime18.cpp').read_text()
    expected = 'location[1] > (UINT64_C(64) << 20)'
    if runtime.count(expected) != 1:
        raise ValueError('Board addon range-limit contract changed')
    runtime = runtime.replace(expected, 'location[1] > (UINT64_C(128) << 20)', 1)
    runtime = runtime.replace('extern "C" {\n',
                              'extern "C" {\nextern char native_root_block_panic_text[4096];\n', 1)
    runtime = runtime.replace('unsigned(sizeof(runtimeLog->text) - 4096)',
                              'unsigned(sizeof(runtimeLog->text) - 8192)', 1)
    panic_marker = '    runtimeLog->length = used;'
    if runtime.count(panic_marker) != 1:
        raise ValueError('Retained runtime log finalization changed')
    runtime = runtime.replace(panic_marker,
        '    if (native_root_block_panic_text[0])\n'
        '        used += snprintf(runtimeLog->text + used, sizeof(runtimeLog->text) - used,\n'
        '                         "\\nPANIC\\n%.4095s\\n", native_root_block_panic_text);\n'
        + panic_marker)
    runtime_path = target / 'MarbleRuntime18.cpp'
    if runtime_path.is_symlink():
        runtime_path.unlink()
    runtime_path.write_text(runtime)
    save(RECORDS / 'board-runtime-source.json', {
        'mount_implementation': {'source': 'package/MarbleSystemOS.c',
                                 'sha256': digest(DEVICE / 'package/MarbleSystemOS.c'),
                                 'generated': str(target / 'MarbleSystemOS.c'),
                                 'generated_sha256': digest(target / 'MarbleSystemOS.c')},
        'target_policy': 'package/ios27-runtime/MarbleUiPolicy27.c',
        'target_crash_info': 'package/ios27-runtime/MarbleCrashInfo27.c',
        'coreentitlements_query_page_shift': {'symbol': '_native_ios27_ce_page_shift', 'value': 14,
                                             'binding_scope': '24B119 AMFI GOT 0xfffffff007ea0060',
                                             'review': 'records/ios27-24B119/amfi-page-shift-review.json'},
        'runtime_capacity': {'source': 'package/MarbleRuntime18.cpp',
                             'generated': str(runtime_path), 'bytes': 128 << 20,
                             'generated_sha256': digest(runtime_path)},
        'panic_capture': {'source': 'package/ios27-runtime/MarblePanic27.c',
                          'buffer_bytes': 4096, 'runtime_log_reserve_bytes': 8192},
        'selection': 'shared SystemOS mount code plus target runtime translation units',
        'device_result': 'pending'})
    return target


def environment(out):
    p = tomllib.loads(PROFILE.read_text())['os']
    env = dict(os.environ, ZERO_AR_DATE='1', JOBS=str(min(os.cpu_count() or 4, 8)),
               UNIXNU_DEVICE=str(DEVICE), UNIXNU_XNU_TAG='xnu-' + p['xnu_version'],
               UNIXNU_XNU_COMMIT=p['xnu_revision'], UNIXNU_XNU_VERSION=p['xnu_version'],
               UNIXNU_XNU_DARWIN=p['darwin'], UNIXNU_XNU_ARCH='arm64e',
               UNIXNU_DRIVERKIT_TAG=p['driverkit_tag'], UNIXNU_TRUSTCACHE_TAG=p['trustcache_tag'],
               UNIXNU_IOS_VERSION=p['ios_version'], UNIXNU_IPSW_BUILD='24B119',
               PATH=os.pathsep.join([str(Path(sys.executable).parent), '/opt/homebrew/opt/llvm/bin',
                                     '/opt/homebrew/opt/lld/bin', os.environ['PATH']]))
    sys.path.insert(0, str(SOURCE / 'scripts'))
    from unixnu.config import parse_config
    config = parse_config((DEVICE / 'configs/marble_ios27_defconfig').read_text())
    for key, value in config.items():
        if key.startswith('XNU_'):
            env[key] = ('1' if value else '0') if isinstance(value, bool) else str(value).replace('{tree}', str(out / 'tree'))
    for name, prefix in (('libplatform', 'LIBPLATFORM'), ('libdispatch', 'LIBDISPATCH'),
                         ('AvailabilityVersions', 'AVAILABILITYVERSIONS'), ('libpthread', 'LIBPTHREAD')):
        spec = next((s for s in tomllib.loads(PROFILE.read_text()).get('source', []) if s['name'] == name), None)
        if spec:
            env['UNIXNU_' + prefix + '_TAG'] = spec['ref']
            env['UNIXNU_' + prefix + '_COMMIT'] = spec['commit']
    return env


def patches(out):
    """Apply the recorded patch order to a new, signature-specific XNU tree."""
    board = tomllib.loads((DEVICE / 'board.toml').read_text())
    roots = {'arch': SOURCE / 'profiles/arch/arm64e', 'soc': DEVICE / 'socs/sm7475',
             'os': SOURCE / 'profiles/os/ios-27.2-24B119', 'device': DEVICE}

    def resolve(value, parent):
        prefix, separator, suffix = value.partition(':')
        if separator and prefix in roots:
            return (roots[prefix] / suffix).resolve()
        return (parent / value).resolve()

    selected = []
    for name in board['kernel']['patch_series']:
        series = resolve(name, DEVICE)
        for line in series.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith('#'):
                patch = resolve(line, series.parent)
                if not patch.is_file():
                    raise FileNotFoundError(patch)
                if patch in selected:
                    raise ValueError(f'duplicate kernel patch: {patch}')
                selected.append(patch)
    profile = tomllib.loads(PROFILE.read_text())
    signature = {'xnu': profile['os']['xnu_revision'],
                 'patches': [{'path': str(p), 'sha256': digest(p)} for p in selected]}
    key = hashlib.sha256(json.dumps(signature, sort_keys=True).encode()).hexdigest()
    work = out / 'xnu-work' / key[:16]
    stamp = work / '.marble-applied.json'
    if not stamp.is_file():
        if work.exists():
            raise RuntimeError(f'previous patch attempt requires inspection: {work}')
        work.mkdir(parents=True)
        execute(['git', 'init', '--quiet', str(work)])
        objects = (out / 'references/xnu/.git/objects').resolve()
        (work / '.git/objects/info/alternates').write_text(str(objects) + '\n')
        execute(['git', '-C', str(work), 'remote', 'add', 'origin', 'https://github.com/apple-oss-distributions/xnu.git'])
        execute(['git', '-C', str(work), 'config', 'remote.origin.promisor', 'true'])
        execute(['git', '-C', str(work), 'checkout', '--quiet', '--detach', signature['xnu']])
        for patch in selected:
            execute(['git', '-C', str(work), 'apply', '--check', str(patch)])
            execute(['git', '-C', str(work), 'apply', str(patch)])
        save(stamp, signature)
    cached = out / 'tree/.cache/xnu-work'
    if cached.is_symlink():
        cached.unlink()
    link(work, cached)
    patch_list = out / 'xnu-patches.list'
    patch_list.write_text(''.join(str(p) + '\n' for p in selected))
    save(RECORDS / 'applied-patches.json', {**signature, 'work': str(work), 'identity': key})
    record_stage('kernel_patches', 'applied', patch_count=len(selected), patch_identity=key,
                 patches_record='records/ios27-24B119/applied-patches.json')
    return patch_list


def bootstrap(out):
    tree = prepare(out)
    patch_list = patches(out)
    env = environment(out)
    env.update(UNIXNU_PATCH_LIST=str(patch_list), XNU_BOOTSTRAP='1', XNU_BOOT_PROGRESS='0')
    log = out / 'logs/kernel-bootstrap.log'
    try:
        execute(['bash', str(tree / 'scripts/build-xnu-4k-baseline.sh')], cwd=tree, env=env, log=log)
    except subprocess.CalledProcessError:
        # The bootstrap pass may reach the link before board archives exist.
        # Compile records are the declared output needed by the driver builders.
        expected = ('bsd/DEVELOPMENT/kern_exec.o.json', 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json',
                    'osfmk/DEVELOPMENT/startup.o.json')
        missing = [name for name in expected
                   if not (tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / name).is_file()]
        if missing:
            raise
        record_stage('bootstrap', 'compile-records-ready', bootstrap_link='pending-board-archives', log=str(log))
        return
    record_stage('bootstrap', 'complete', log=str(log))


def archives(out):
    tree = out / 'tree'
    env = environment(out)
    recipes = [('corecrypto', ['build-corecrypto-14.py']),
               ('corecrypto-namespace', ['build-corecrypto-14.py', '--namespace']),
               ('cpu', ['build-arm-driver.py', 'ArmCpu']),
               ('platform', ['build-arm-driver.py', 'ArmDTPlatform']),
               ('pthread', ['build-libpthread-kernel.py'])]
    for name, recipe in recipes:
        log = out / 'logs' / (name + '.log')
        execute([sys.executable, str(tree / 'scripts' / recipe[0]), *recipe[1:]],
                cwd=tree, env=env, log=log)
        record_stage(name, 'built', log=str(log))


def drivers(out):
    script = DEVICE / 'scripts/build_ios27_drivers.py'
    if not script.is_file():
        raise FileNotFoundError(script)
    command = [sys.executable, str(script), '--tree', str(out / 'tree'),
               '--output', str(out), '--kernelcache', str(out / 'ipsw/24B119/kernelcache.decompressed')]
    env = environment(out)
    # Existing board translation units use IOS18 names to select their
    # arm64e hardware glue. Original-class ABI comes from the 24B119 adapters;
    # the IOS27 names select the version-qualified crash/policy code.
    defines = ('MARBLE_IOS18', 'MARBLE_IOS18_GPU', 'MARBLE_IOS27', 'MARBLE_IOS27_GPU')
    display_defines = ('NATIVE_LEGACY_MACOS=0', 'NATIVE_LEGACY_DEFAULT_SURFACE=1')
    env['CCC_OVERRIDE_OPTIONS'] = '# ' + ' '.join(
        ['+-D' + name + '=1' for name in defines] + ['+-D' + value for value in display_defines])
    save(out / 'driver-environment.json', {'target': '24B119', 'board_defines': defines,
                                         'display_defines': display_defines,
                                         'CCC_OVERRIDE_OPTIONS': env['CCC_OVERRIDE_OPTIONS']})
    execute(command, cwd=DEVICE, env=env, log=out / 'logs/drivers.log')
    required = ('marble-boot/libMarbleBoot.a', 'marble-storage/libMarbleStorage.a', 'marble-gpu/libMarbleGpu.a')
    outputs = {}
    for name in required:
        path = out / 'tree/build' / name
        family = name.split('/')[0]
        report_path = out / family / 'build-report.json'
        report = json.loads(report_path.read_text())
        if (report.get('status') != 'built' or report.get('failed') or report.get('pending')
                or report.get('missing') or report.get('archive') != str(path)):
            raise ValueError('Driver family requires its complete successful build: ' + family)
        if not path.is_file():
            raise FileNotFoundError(path)
        if digest(path) != report.get('archive_sha256'):
            raise ValueError('Driver archive changed after its recorded build: ' + family)
        if family == 'marble-storage' and report.get('storage_abi', {}).get('status') != 'verified':
            raise ValueError('Storage class requires its target PAC and instance-size contract')
        outputs[name] = digest(path)
    record_stage('driver_archives', 'built', archives=outputs, log=str(out / 'logs/drivers.log'))


def kernel(out):
    """Compile the complete board, then retain a symbol-bearing RELEASE kernel."""
    bootstrap(out)
    archives(out)
    drivers(out)
    tree, env = out / 'tree', environment(out)
    env.update(UNIXNU_PATCH_LIST=str(out / 'xnu-patches.list'), XNU_BOOTSTRAP='0')
    execute(['bash', str(tree / 'scripts/build-xnu-4k-baseline.sh')], cwd=tree, env=env,
            log=out / 'logs/kernel-development.log')
    native = release(out, env)
    save(out / 'kernel.native.json', {'target': 'ios-27.2-24B119', 'path': str(native),
                                     'sha256': digest(native), 'patches': json.loads((RECORDS / 'applied-patches.json').read_text())})
    record_stage('kernel_build', 'complete', kernel_native=str(native), native_sha256=digest(native))


def release(out, env):
    tree = out / 'tree'
    # A signature-specific source checkout needs the same pinned external
    # headers as the initial bootstrap path before an incremental RELEASE.
    execute([sys.executable, str(tree / 'scripts/prepare-xnu-source.py')],
            cwd=tree, env=env, log=out / 'logs/release-source-prepare.log')
    profile = tomllib.loads(PROFILE.read_text())['os']
    sdk = tree / '.cache/fakeroot'
    crypto = tree / 'build/corecrypto-2023/libcorecrypto_kernel_noasm-namespaced.a'
    forced = [tree / 'build' / name for name in ('armcpu/libArmCpu.a', 'armdt-platform/libArmDTPlatform.a',
              'libpthread-kernel/libpthread_kernel.a', 'marble-boot/libMarbleBoot.a',
              'marble-storage/libMarbleStorage.a', 'marble-gpu/libMarbleGpu.a')]
    if env.get('UNIXNU_QEMU_ROOT_ARCHIVE'):
        virtual_storage = Path(env['UNIXNU_QEMU_ROOT_ARCHIVE']).resolve()
        if not virtual_storage.is_file():
            raise FileNotFoundError(virtual_storage)
        forced.append(virtual_storage)
    flags = env.get('XNU_EXTRA_CFLAGS', '') + (' -DXNU_NATIVE_PAN=1 -DXNU_SOURCE_CORECRYPTO14=1'
            ' -DXNU_SOURCE_PTHREAD=1 -DXNU_ARM_DT_PLATFORM=1 -DXNU_ARMCPU=1'
            ' -DXNU_NATIVE_CS_HASH_CACHE=1 -DXNU_BOOT_PROGRESS=0')
    command = ['make', '-C', str(tree / '.cache/xnu-work'), 'build', 'SDKROOT=iphoneos',
               'ARCH_CONFIGS=ARM64', 'MACHINE_CONFIGS=BCM2837', 'KERNEL_CONFIGS=RELEASE',
               'ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64e', 'LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY',
               'ARM64_LINK_ADDR=0xfffffff007004000', 'ARM64_PLKSEG_ADDR=0xfffffff004004000',
               f'DEPLOYMENT_TARGET_FLAGS=-miphoneos-version-min={profile["ios_version"]} -DXNU_TARGET_OS_IOS',
               f'DEPLOYMENT_LINKER_FLAGS=-Wl,-ios_version_min,{profile["ios_version"]}',
               'RC_DARWIN_KERNEL_VERSION=' + profile['darwin'], 'RC_ProjectSourceVersion=' + profile['xnu_version'],
               'BUILD_LTO=1', 'BOUND_CHECKS=0', 'BUILD_WERROR=0', 'MAKEJOBS=-j' + env['JOBS'],
               'XNU_KERNEL_CHAINED_FIXUPS=1',
               f'LDFLAGS_KERNEL_LTO=-miphoneos-version-min={profile["ios_version"]} -Wl,-mllvm,-inline-threshold=100',
               'CFLAGS_EXTRA=' + flags,
               'LD_KERNEL_ARCHIVES=' + ' '.join([f'-L{sdk}/usr/local/lib/kernel', '-lfirehose_kernel', str(crypto),
                                               *(f'-Wl,-force_load,{p}' for p in forced)]),
               'XNU_EXTERNAL_KERNEL_ARCHIVES=' + ' '.join(map(str, [crypto, *forced])),
               f'OBJROOT={tree}/build/xnu/ios-release-obj', f'SYMROOT={tree}/build/xnu/ios-release-sym',
               f'DSTROOT={tree}/build/xnu/ios-release-dst']
    if env.get('UNIXNU_QEMU_ROOT_ARCHIVE'):
        command = [value + ' -Wl,-segprot,__QEMUPOLICY,r,r'
                   if value.startswith('LDFLAGS_KERNEL_LTO=') else value for value in command]
    object_root = tree / 'build/xnu/ios-release-obj/RELEASE_ARM64_BCM2837'
    original_exports = object_root / 'all-kpi.exp'
    lto = object_root / 'justlto.o'
    if original_exports.is_file() and lto.exists():
        files = (object_root / 'nonltolink.filelist').read_text().splitlines()
        objects = [str(Path(p) if Path(p).is_absolute() else object_root / p)
                   for p in files if p.strip()]
        objects += ([str(p) for p in sorted(lto.glob('*.o'))]
                    if lto.is_dir() else [str(lto)])
        objects += list(map(str, [crypto, *forced]))
        names = set()
        for at in range(0, len(objects), 80):
            listing = subprocess.check_output(['xcrun', 'nm', '-gUj', *objects[at:at + 80]], text=True)
            names.update(n for n in listing.splitlines() if n.startswith('_') and ' ' not in n)
        aliases = []
        for line in (object_root / 'all-alias.exp').read_text().splitlines():
            words = line.split()
            if len(words) == 2:
                aliases.append(words)
        while True:
            inherited = {alias for original, alias in aliases if original in names} - names
            if not inherited:
                break
            names.update(inherited)
        selected, omitted = [], []
        for line in original_exports.read_text().splitlines():
            symbol = line.strip()
            if not symbol or symbol.startswith('#'):
                continue
            (selected if any(fnmatch.fnmatchcase(n, symbol) for n in names) else omitted).append(symbol)
        exports = out / 'board-release.exports'
        exports.write_text(''.join(s + '\n' for s in selected))
        save(RECORDS / 'release-exports.json', {
            'source': str(original_exports), 'source_sha256': digest(original_exports),
            'selected': len(selected), 'omitted_unimplemented_platform_exports': omitted,
            'export_list': str(exports), 'export_list_sha256': digest(exports),
            'policy': 'export implemented source-kernel symbols; foreign kext references remain subject to strict prelink closure'})
        command.append('LDFLAGS_KERNEL_ONLY_CONFIG_RELEASEARM64=-Wl,-exported_symbols_list,' + str(exports))
    save(out / 'release-command.json', command)
    env = dict(env, KERNEL_SDKROOT=str(sdk), XNU_AVAILABILITY_PL=str(sdk / 'usr/local/libexec/availability.pl'))
    execute(command, cwd=tree, env=env, log=out / 'logs/kernel-release.log')
    raw = tree / 'build/xnu/ios-release-obj/RELEASE_ARM64_BCM2837/mach.release.bcm2837.unstripped.noctf'
    execute([sys.executable, str(tree / 'scripts/seal-source-crypto-kernel.py'), str(raw)], cwd=tree, env=env)
    native = out / 'kernel.native.macho'
    execute(['xcrun', 'strip', '-S', '-o', str(native), str(raw)])
    from build_ios18_arm64e import build_capture_layout
    build_capture_layout(tree, out)
    save(out / 'kernel.native.json', {
        'target': 'ios-27.2-24B119', 'path': str(native), 'sha256': digest(native),
        'source_xnu': profile['xnu_version'], 'source_darwin': profile['darwin'],
        'deployment_target': profile['ios_version'],
        'link_command': str(out / 'release-command.json'),
        'link_command_sha256': digest(out / 'release-command.json'),
        'archives': [{'path': str(path), 'sha256': digest(path)} for path in [crypto, *forced]],
        'patches': json.loads((RECORDS / 'applied-patches.json').read_text()),
        'runtime_result': 'pending'})
    return native


def prelink(out):
    """Preserve fixed-input diagnostics and require the complete target kext closure."""
    p = tomllib.loads(PROFILE.read_text())
    spec = p['kernel']['prelink']
    native = out / 'kernel.native.macho'
    cache = out / 'ipsw/24B119/kernelcache.decompressed'
    if digest(cache) != spec['kernelcache_sha256']:
        raise ValueError('prelink original kernelcache differs from 24B119')
    fixture = json.loads((DEVICE / 'patches/XnuPrelink/ios27-24B119/fileset-24B119-symbols.json').read_text())
    assignments = {row['original']: row for row in fixture['functions']}
    for review_name in ('parent-proc-mapping-review.json', 'parent-csblob-mapping-review.json',
                        'parent-memory-mappings.json', 'parent-vnode-mapping-review.json',
                        'parent-image4-registration-review.json', 'parent-csm-contract-review.json',
                        'parent-qemu-code-signing-review.json', 'parent-cpu-topology-review.json',
                        'parent-vfs-signal-review.json', 'parent-framework-api-review.json',
                        'parent-profile-contract-review.json', 'parent-image4-monitor-review.json',
                        'trust-cache-contract.json', 'executable-vnode-contract.json',
                        'launch-constraint-blob-contract.json', 'self-pid-contract.json'):
        reviewed = json.loads((RECORDS / review_name).read_text())
        for row in reviewed['functions']:
            actual = assignments.get(row['original'], {})
            if (actual.get('native_symbol') != row['native_symbol']
                    or actual.get('original_sha256') != row['original_sha256']):
                raise ValueError('Reviewed API call contract changed: ' + row['original'])
    reference = out / 'tree/build/xnu/fileset-reference/mach.release.bcm2837.unstripped.noctf'
    if not reference.is_file():
        execute([sys.executable, str(out / 'tree/scripts/build-fileset-release-reference.py'),
                 '--tree', str(out / 'tree'), '--lto'], cwd=out / 'tree', env=environment(out),
                log=out / 'logs/fileset-reference.log')
    candidate = out / 'prelink-candidate/kernel.fileset.macho'
    candidate.parent.mkdir(exist_ok=True)
    target_wrapper = DEVICE / 'scripts/ios27_prelink.py'
    if not target_wrapper.is_file():
        raise FileNotFoundError(target_wrapper)
    command = [sys.executable, str(target_wrapper), '--native', str(native),
               '--kernelcache', str(cache), '--reference', str(reference),
               '--work', str(out / 'prelink-work'), '--out', str(candidate)]
    execute(command, cwd=out / 'tree', env=environment(out), log=out / 'logs/prelink.log')
    report = json.loads(candidate.with_suffix('.json').read_text())
    if (not report.get('candidate_written') or not report.get('mapping_complete')
            or report.get('missing') or report.get('stubbed') or report.get('allow_missing')):
        raise ValueError('prelink candidate requires complete target kernel mappings')
    final = out / candidate.name
    shutil.copyfile(candidate, final)
    save(final.with_suffix('.json'), report)
    mapping = candidate.with_suffix('.map.json')
    if mapping.is_file():
        shutil.copyfile(mapping, final.with_suffix('.map.json'))
    record_stage('prelink', 'complete', kernel_fileset=str(final), fileset_sha256=digest(final))


def status(out):
    print(json.dumps(json.loads((RECORDS / 'progress.json').read_text()), ensure_ascii=False, indent=2))
    for name in ('kernel.native.macho', 'kernel.fileset.macho'):
        path = out / name
        if path.is_file():
            print(name, digest(path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('status', 'fetch', 'assets', 'references', 'prepare', 'patches', 'bootstrap',
                                          'archives', 'drivers', 'kernel', 'prelink'))
    parser.add_argument('--output', type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    try:
        if args.command == 'status':
            status(out)
        elif args.command in ('fetch', 'assets'):
            fetch(out, system=args.command == 'assets')
        elif args.command == 'references':
            references(out)
        elif args.command == 'prepare':
            prepare(out)
        elif args.command == 'patches':
            patches(out)
        elif args.command == 'bootstrap':
            bootstrap(out)
        elif args.command == 'archives':
            archives(out)
        elif args.command == 'drivers':
            drivers(out)
        elif args.command == 'kernel':
            kernel(out)
        elif args.command == 'prelink':
            prelink(out)
    except Exception as error:
        record_stage(args.command, 'failed', error=f'{type(error).__name__}: {error}')
        traceback.print_exc()
        raise SystemExit(1)


if __name__ == '__main__':
    main()
