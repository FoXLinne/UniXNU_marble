#!/usr/bin/env python3
"""Build the marble iOS 27.2 (24B119) userland/runtime inputs.

Validates the mounted 24B119 OS root and SystemOS cryptex read-only, qualifies
every kernel text fixup against the on-disk shared-cache bytes, then produces
the board runtime pieces: launchd cache + signature, Wi-Fi supplicant daemon,
Actuator HAL + N104->D84 tunings, the phone-UI addon (when the Metal addon is
supplied), the AppSync payload with 24B119 pins, a trust-hash manifest for the
parent's trust-append step, and a deterministic runtime.json report.

No 22H384 or 24A446 value is used unless it was re-qualified against 24B119;
every site below comes from profiles/runtime/ios27-24B119/.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

if sys.version_info < (3, 11):
    try:
        import tomllib
    except ImportError:
        import tomli
        sys.modules['tomllib'] = tomli

REPO = Path(__file__).resolve().parents[1]
PROFILE = REPO / 'profiles' / 'runtime' / 'ios27-24B119'
EXPECTED_BUILD = '24B119'
EXPECTED_CACHE_UUID = '555BFDAA-D930-31AC-A915-D472C273C527'
APPSYNC_DYLIB = '/usr/lib/UniXNUAppSync.dylib'
IOS27_TARGET = 'arm64e-apple-ios27.0'
MACHO_MAGICS = (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf')


def run(*args, capture=False):
    result = subprocess.run([str(a) for a in args], check=True,
                            capture_output=capture, text=capture)
    # codesign reports details on stderr; other tools on stdout.
    return (result.stdout or '') + (result.stderr or '') if capture else None


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def codesign(path, identifier=None, entitlements=None):
    options = ['--identifier', identifier] if identifier else \
        ['--preserve-metadata=identifier,entitlements,flags']
    if entitlements:
        options += ['--entitlements', str(entitlements)]
    run('codesign', '--force', '--sign', '-', '--pagesize', '4096',
        '--digest-algorithm', 'sha256', *options, path)
    run('codesign', '--verify', '--strict', path)


def cdhash(path):
    detail = run('codesign', '--display', '--verbose=4', path, capture=True)
    return re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M).group(1)


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def iphoneos_sdk():
    return run('xcrun', '--sdk', 'iphoneos', '--show-sdk-path', capture=True).strip()


# ---------- validation ----------

def validate_os_root(os_root, profile):
    version = plistlib.loads((os_root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    build = version.get('ProductBuildVersion')
    if build != EXPECTED_BUILD:
        raise SystemExit(f'os-root build {build} != {EXPECTED_BUILD}')
    pins = profile['appsync']['pins_24B119']
    checked = {}
    for relative, expected in pins.items():
        path = os_root / relative
        if not path.is_file():
            raise SystemExit(f'missing OS binary {relative}')
        actual = sha256(path)
        if actual != expected:
            raise SystemExit(f'OS binary {relative} sha256 {actual} != pinned {expected}')
        checked[relative] = actual
    launchd = os_root / 'System/Library/xpc/launchd.plist'
    if not launchd.is_file():
        raise SystemExit('missing System/Library/xpc/launchd.plist')
    return dict(status='validated', build=build, product=version.get('ProductType'),
                appsync_pins=checked, launchd_plist_sha256=sha256(launchd))


def validate_fixups(cryptex_root, fixups):
    cache_dir = cryptex_root / 'System/Library/Caches/com.apple.dyld'
    base = cache_dir / 'dyld_shared_cache_arm64e'
    if not base.is_file():
        raise SystemExit('missing dyld_shared_cache_arm64e in cryptex root')
    verified = []
    for item in fixups['entries']:
        entry = item['entry']
        cache_file, offset_s, original, replacement = entry.split(',')
        path = cache_dir / cache_file
        if not path.is_file():
            raise SystemExit(f'missing cache subcache {cache_file}')
        offset = int(offset_s, 16)
        with path.open('rb') as stream:
            stream.seek(offset)
            live = stream.read(len(bytes.fromhex(original)))
        if live.hex() != original:
            raise SystemExit(f'fixup bytes mismatch at {cache_file}:{offset_s} for {item["symbol"]}')
        if len(bytes.fromhex(original)) != len(bytes.fromhex(replacement)):
            raise SystemExit(f'fixup length mismatch for {item["symbol"]}')
        verified.append(dict(entry=entry, image=item['image'], symbol=item['symbol'],
                             va=item['va'], group=item['group'],
                             gated=item.get('gated') is not None))
    return verified


# ---------- wifi ----------

def build_wifi(components, os_root, output):
    component = components / 'Ath12kWifi'
    supplicant_tool = component / 'tools/build-supplicant.py'
    if not supplicant_tool.is_file():
        return dict(status='skipped', reason='Ath12kWifi component missing')
    builder = REPO / 'scripts' / 'build_marble_wifi_userspace.py'
    run(sys.executable, builder, '--component', component, '--os-root', os_root,
        '--output', output, '--build', EXPECTED_BUILD, '--runtime-target', 'ios27',
        '--source-scripts', REPO / 'ios27/source/scripts')
    report = json.loads((output / 'report.json').read_text())
    report['os_build'] = EXPECTED_BUILD
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return dict(status='built', report=report)


# ---------- haptics ----------

def build_haptics(os_root, output, profile):
    """ios27 variant of build_marble_haptics.py: N104 tunings, ios27 SDK target."""
    sdk = iphoneos_sdk()
    mac = Path(run('xcrun', '--sdk', 'macosx', '--show-sdk-path', capture=True).strip())
    hal = output / 'hal'
    hal.mkdir(parents=True, exist_ok=True)
    original_hal = os_root / 'System/Library/Audio/Plug-Ins/HAL'
    for path in original_hal.iterdir():
        if path.is_dir():
            shutil.copytree(path, hal / path.name, dirs_exist_ok=True, symlinks=True)
    bundle = hal / 'MarbleHaptics.driver'
    bundle.mkdir(exist_ok=True)
    binary = bundle / 'MarbleHaptics'
    with tempfile.TemporaryDirectory() as temporary:
        headers = Path(temporary) / 'CoreAudio'
        headers.mkdir()
        for name in ('AudioServerPlugIn.h', 'AudioHardware.h', 'AudioHardwareBase.h'):
            (headers / name).symlink_to(
                mac / 'System/Library/Frameworks/CoreAudio.framework/Headers' / name)
        run('xcrun', 'clang', '-target', IOS27_TARGET, '-isysroot', sdk,
            '-I', temporary, '-I', REPO / 'package', '-O2', '-g0',
            '-Wall', '-Wextra', '-Werror', '-bundle', '-fvisibility=hidden',
            '-framework', 'CoreFoundation',
            REPO / 'hal/MarbleHapticsPlugIn.c', '-o', binary)
    factory = 'EF40B271-9792-4B16-8844-D3E83423F28C'
    info = {'CFBundleDevelopmentRegion': 'en', 'CFBundleExecutable': 'MarbleHaptics',
            'CFBundleIdentifier': 'org.unixnu.marble.haptics',
            'CFBundleInfoDictionaryVersion': '6.0', 'CFBundleName': 'MarbleHaptics',
            'CFBundlePackageType': 'BNDL', 'CFBundleShortVersionString': '1.0',
            'CFBundleVersion': '1', 'CFBundleSupportedPlatforms': ['iPhoneOS'],
            'MinimumOSVersion': '27.0',
            'CFPlugInFactories': {factory: 'MarbleHaptics_Create'},
            'CFPlugInTypes': {'443ABAB8-E7B3-491A-B985-BEB9187030DB': [factory]}}
    (bundle / 'Info.plist').write_bytes(plistlib.dumps(info))
    codesign(bundle, 'org.unixnu.marble.haptics')
    hash_hal = cdhash(bundle)
    source_product = profile['tunings']['os_source_product']
    published = profile['tunings']['board_published_product']
    source_tunings = os_root / 'Library/Audio/Tunings' / source_product
    if not source_tunings.is_dir():
        raise SystemExit(f'missing OS tunings {source_tunings}')
    tunings = output / 'tunings'
    copied = 0
    for product in (source_product, published):
        for source in source_tunings.rglob('*'):
            if not source.is_file():
                continue
            relative = str(source.relative_to(source_tunings))
            if product != source_product:
                relative = relative.replace(source_product, product).replace(
                    source_product.lower(), product.lower())
            destination = tunings / product / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            copied += 1
    files = {str(p.relative_to(output)): sha256(p)
             for d in ('hal', 'tunings') for p in sorted((output / d).rglob('*')) if p.is_file()}
    return dict(status='built', hal_cdhash=hash_hal, tunings_copied=copied,
                tunings_source=source_product, tunings_published=published, files=files)


# ---------- phone ui ----------

def build_phone_ui(addon_blob, output):
    """ios27 packaging of the phone-UI addon (build_phone_ui.py caps at ios18)."""
    sys.path.insert(0, str(REPO / 'scripts'))
    from marble_display_geometry import write_ui_header
    blob = Path(addon_blob).read_bytes()
    if blob[:8] != b'NGPUADD2':
        raise SystemExit('expected NGPUADD2 addon')
    entries, at = [], 24
    prefix = 'addons/NativeAdrenoMetal.bundle/'
    bundle = output / 'NativeAdrenoMetal.bundle'
    for _ in range(struct.unpack_from('<I', blob, 8)[0]):
        length, mode, size, packed = struct.unpack_from('<IIQQ', blob, at)
        at += 24
        name = blob[at:at+length-1].decode()
        at += (length + 7) & ~7
        data = zlib.decompress(blob[at:at+packed])
        at += (packed + 7) & ~7
        if len(data) != size or '..' in Path(name).parts:
            raise ValueError('invalid addon entry')
        if name.startswith(prefix):
            target = bundle / name[len(prefix):]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            target.chmod(mode)
        else:
            entries.append((name, mode, data))
    if at != len(blob):
        raise ValueError('trailing addon bytes')
    turnip = bundle / 'libvulkan_freedreno.dylib'
    turnip_hash = sha256(turnip)
    library = bundle / 'MarblePhoneUI.dylib'
    # D84 screen category (11) retained for the rendered phone UI geometry.
    write_ui_header(output / 'MarbleScreenGeometryProfile.h', 'ios18')
    run('xcrun', 'clang', '-target', IOS27_TARGET, '-isysroot', iphoneos_sdk(),
        '-O2', '-Wall', '-Wextra', '-Werror', '-fno-objc-arc', '-dynamiclib',
        '-I', output, REPO / 'package/MarblePhoneUI.m',
        REPO / 'package/MarbleScreenGeometry.m',
        REPO / 'package/ios27-runtime/userspace/MarbleStorageCapacity27.c',
        '-lMobileGestalt', '-lobjc', '-framework', 'CoreFoundation',
        '-Wl,-reexport_framework,Foundation', '-install_name', '@loader_path/MarblePhoneUI.dylib',
        '-compatibility_version', '300.0.0', '-current_version', '300.0.0', '-o', library)
    executable = bundle / 'NativeAdrenoMetal'
    run('xcrun', 'install_name_tool', '-change',
        '/System/Library/Frameworks/Foundation.framework/Foundation',
        '@loader_path/MarblePhoneUI.dylib', executable)
    hashes = []
    for target, identifier in ((library, 'org.unixnu.marble.phone-ui'),
                               (bundle, 'org.blu-sharky.NativeAdrenoMetal')):
        codesign(target, identifier)
        hashes.append(cdhash(target))
    if sha256(turnip) != turnip_hash:
        raise RuntimeError('Turnip changed during UIKit packaging')
    for path in sorted(bundle.rglob('*')):
        if path.is_file():
            entries.append((prefix + path.relative_to(bundle).as_posix(),
                            0o755 if path.name in ('NativeAdrenoMetal', 'MarblePhoneUI.dylib',
                                                  'libvulkan_freedreno.dylib') else 0o644,
                            path.read_bytes()))
    body = bytearray()
    for name, mode, data in entries:
        encoded, packed = name.encode() + b'\0', zlib.compress(data, 9)
        if len(encoded) > 160:
            raise ValueError('addon path exceeds loader limit')
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed))
        body += encoded + bytes(-len(encoded) % 8) + packed + bytes(-len(packed) % 8)
    addon = output / 'phone-ui.gpuaddon'
    addon.write_bytes(b'NGPUADD2' + struct.pack('<IIQ', len(entries), 0, len(body) + 24) + body)
    return dict(status='built', addon=str(addon), cdhashes=hashes,
                compiler_target=IOS27_TARGET, screen_category=11,
                turnip_sha256=turnip_hash)


# ---------- wallpaper ----------

def merge_wallpaper(addon, os_root, cache, identity):
    """24B119 variant of prepare_ios18_wallpaper.prepare_wallpaper.

    Merges panel-sized wallpaper resources and event-timing profiles into the
    phone-UI addon. Differences from the 22H384 builder: event-timing sources
    are N104 (the only product shipped by 24B119) and the identity keys come
    from profiles/runtime/ios27-24B119/target.json.
    """
    sys.path.insert(0, str(REPO / 'scripts'))
    from prepare_phone_resources import pack_addon, unpack_addon
    from marble_display_geometry import display_geometry, display_layout
    source = os_root / 'Library/Wallpaper'
    entries = {name: value for name, value in unpack_addon(addon).items()
               if not name.startswith('wallpaper/')}
    cache.mkdir(parents=True, exist_ok=True)
    resources = {}
    converted = native = 0
    display, layout = display_geometry(), display_layout()
    screen = (f'{round(layout["logical_width"])}w-{round(layout["logical_height"])}h'
              f'@{display["scale"]}x~iphone')

    def add(path, destination, names=None):
        nonlocal converted
        names = names or {}
        if path.suffix.upper() == '.HEIC':
            digest = sha256(path)
            png = cache / (digest + '.png')
            if not png.exists():
                run('sips', '-s', 'format', 'png', path, '--out', png)
            data = png.read_bytes()
            destination = str(Path(destination).with_suffix('.png'))
            converted += 1
        else:
            data = path.read_bytes()
            if path.suffix in ('.caml', '.xml'):
                data = data.replace(b'.HEIC', b'.png')
                for original, replacement in names.items():
                    data = data.replace(original.encode(), replacement.encode())
        resources['wallpaper/' + destination] = (0o644, data)

    collections = {}
    for collection in sorted((source / 'Collections').iterdir()):
        if not collection.is_dir():
            continue
        metadata = plistlib.loads((collection / 'WallpaperCollection.plist').read_bytes())
        collections[metadata['identifier']] = metadata
        prefix = 'Collections/' + collection.name
        for path in sorted(collection.rglob('*')):
            relative = path.relative_to(collection)
            if path.is_file() and relative.parts[0] != 'Wallpapers':
                add(path, f'{prefix}/{relative.as_posix()}')
        for wallpaper in sorted((collection / 'Wallpapers').iterdir()):
            if not wallpaper.is_dir():
                continue
            metadata = plistlib.loads((wallpaper / 'Wallpaper.plist').read_bytes())
            names = {}
            folder = wallpaper.name
            if metadata['logicalScreenClass'] != 'any':
                folder = re.sub(r'\d+w-\d+h@\dx~iphone', screen, folder)
                metadata['logicalScreenClass'] = screen
                native += 1
                for variants in metadata['assets'].values():
                    for properties in variants.values():
                        for key, value in properties.items():
                            if isinstance(value, str) and value.endswith('.ca'):
                                short = key.split('Animation', 1)[0].capitalize() + '.ca'
                                names[value] = short
                                properties[key] = short
            target = f'{prefix}/Wallpapers/{folder}'
            for path in sorted(wallpaper.rglob('*')):
                if not path.is_file() or path.name == 'Wallpaper.plist':
                    continue
                relative = path.relative_to(wallpaper)
                parts = list(relative.parts)
                parts[0] = names.get(parts[0], parts[0])
                add(path, f'{target}/{"/".join(parts)}', names)
            resources[f'wallpaper/{target}/Wallpaper.plist'] = (
                0o644, plistlib.dumps(metadata, fmt=plistlib.FMT_BINARY))

    defaults = plistlib.loads((source / 'DefaultWallpapers~iphone.plist').read_bytes())
    default = defaults['default']
    if default['collectionIdentifier'] not in collections:
        raise ValueError('Default wallpaper collection is absent from the restore volume')
    for model in (identity['product'], identity['compatible_device_fallback']):
        defaults[model] = {'default': dict(default)}
    resources['wallpaper/DefaultWallpapers~iphone.plist'] = (
        0o644, plistlib.dumps(defaults, fmt=plistlib.FMT_BINARY))
    order = plistlib.loads((source / 'Collections/Collections~iphone.plist').read_bytes())
    order['order'] = [item for item in order['order'] if item in collections]
    resources['wallpaper/Collections/Collections~iphone.plist'] = (
        0o644, plistlib.dumps(order, fmt=plistlib.FMT_BINARY))
    timing_source = os_root / 'System/Library/EventTimingProfiles'
    timing_product = identity['event_timing_source_product']
    timing = {}
    for kind in ('Default', 'Touch'):
        original = timing_source / f'{timing_product}.{kind}.plist'
        name = f'event-timing/{identity["target_type"]}.{kind}.plist'
        payload = original.read_bytes()
        entries[name] = (0o644, payload)
        timing[kind] = dict(source=str(original),
                            sha256=hashlib.sha256(payload).hexdigest(),
                            profile=plistlib.loads(payload))
    entries.update(resources)
    pack_addon(entries, addon)
    return dict(status='built', source=str(source), screen=screen,
                product=identity['product'], timing_product=timing_product,
                collections=order['order'], native_variants=native,
                converted_layers=converted, files=len(resources),
                resource_bytes=sum(len(data) for _, data in resources.values()),
                default=default, event_timing=timing)


# ---------- appsync ----------

def build_appsync(source_scripts, os_root, output, profile):
    """24B119 AppSync payload; mirrors appsync/build.py with re-pinned inputs."""
    sys.path.insert(0, str(source_scripts / 'appsync'))
    macho = module(source_scripts / 'appsync' / 'macho.py', 'appsync_macho')
    signer = module(source_scripts / 'appsync' / 'signer.py', 'appsync_signer')
    if sys.version_info < (3, 12):
        import tarfile
        original_extractall = tarfile.TarFile.extractall
        def extractall_compat(self, path='.', members=None, *, filter=None, **kwargs):
            if filter == 'data':
                members = [m for m in self.getmembers()
                           if not m.name.startswith('/') and '..' not in m.name.split('/')]
            return original_extractall(self, path, members, **kwargs)
        tarfile.TarFile.extractall = extractall_compat
    helper = ('System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/'
              'com.apple.MobileInstallationHelperService.xpc')
    springboard = 'System/Library/CoreServices/SpringBoard.app'
    springboard_copy = 'coreservices/SpringBoard.app'
    pins = {
        'usr/libexec/installd': 'libexec/installd',
        'usr/libexec/backboardd': 'libexec/backboardd',
        helper + '/com.apple.MobileInstallationHelperService': 'helper/com.apple.MobileInstallationHelperService',
        springboard + '/SpringBoard': 'coreservices/SpringBoard.app/SpringBoard',
    }
    pin_hashes = profile['appsync']['pins_24B119']
    patched = {dest: macho.add_dylib((os_root / source).read_bytes(), APPSYNC_DYLIB,
                                     pin_hashes[source])
               for source, dest in pins.items()}
    work = output
    for directory in ('lib', 'libexec', 'helper', springboard_copy):
        (work / directory).mkdir(parents=True, exist_ok=True)
    sdk = iphoneos_sdk()
    flags = ['-target', IOS27_TARGET, '-isysroot', sdk, '-fobjc-arc', '-O2',
             '-Wall', '-Wextra', '-Werror', '-framework', 'Foundation']
    run('xcrun', 'clang', *flags, '-dynamiclib',
        source_scripts / 'appsync' / 'AppSync.m', source_scripts / 'appsync' / 'Signing.m',
        source_scripts / 'appsync' / 'BacklightMode.m', '-install_name', APPSYNC_DYLIB,
        '-Wl,-reexport_framework,Foundation', '-Wl,-compatibility_version,3000',
        '-Wl,-current_version,5027.0.69', '-o', work / 'lib/UniXNUAppSync.dylib')
    codesign(work / 'lib/UniXNUAppSync.dylib', 'org.unixnu.appsync')
    signing = signer.build(work / 'lib' / signer.LIBRARY)
    codesign(work / 'lib' / signer.LIBRARY, 'org.unixnu.ldid')
    shutil.copytree(Path(signing['cache']) / 'licenses', work / 'libexec/appsync-licenses',
                    dirs_exist_ok=True)
    run('xcrun', 'clang', *flags, source_scripts / 'appsync' / 'appinst.m',
        '-o', work / 'libexec/unixnu-appinst')
    entitlement_file = work / 'appinst-entitlements.plist'
    entitlement_file.write_bytes(plistlib.dumps({
        'application-identifier': 'org.unixnu.appinst', 'platform-application': True,
        'com.apple.private.mobileinstall.allowedSPI': ['Install', 'InstallForLaunchServices'],
        'com.apple.security.exception.mach-lookup.global-name': ['com.apple.mobile.installd']}))
    codesign(work / 'libexec/unixnu-appinst', 'org.unixnu.appinst', entitlement_file)
    for source, dest in ((helper, 'helper'), (springboard, springboard_copy)):
        shutil.copytree(os_root / source, work / dest, dirs_exist_ok=True)
        (work / dest / 'UniXNUAppSync.runtime').write_text(EXPECTED_BUILD + '\n')
    def entitlements(path):
        value = subprocess.run(['codesign', '-d', '--entitlements', ':-', str(path)],
                               check=True, capture_output=True, text=True).stdout
        return value
    originals = {}
    for source, dest in pins.items():
        original, target = os_root / source, work / dest
        before = entitlements(original)
        identity = re.search(r'^Identifier=(.+)$',
                             run('codesign', '-d', '--verbose=4', original, capture=True),
                             re.M).group(1)
        target.write_bytes(patched[dest])
        target.chmod(0o755)
        codesign(target)
        after = entitlements(target)
        identity_after = re.search(r'^Identifier=(.+)$',
                                   run('codesign', '-d', '--verbose=4', target, capture=True),
                                   re.M).group(1)
        if after != before or identity_after != identity:
            raise ValueError('OS entitlements or identifier changed: ' + source)
        originals[source] = dict(sha256=pin_hashes[source], entitlements_preserved=True)
    files = {}
    for relative in ['lib/UniXNUAppSync.dylib', 'lib/' + signer.LIBRARY,
                     'libexec/unixnu-appinst', *pins.values()]:
        path = work / relative
        files[relative] = dict(cdhash=cdhash(path), sha256=sha256(path))
    system_executables = {}
    for original_dir, directory in ((helper, 'helper'), (springboard, springboard_copy)):
        for path in (work / directory).rglob('*'):
            if not path.is_file() or path.relative_to(work).as_posix() in files:
                continue
            with path.open('rb') as stream:
                magic = stream.read(4)
            if magic in MACHO_MAGICS:
                original = os_root / original_dir / path.relative_to(work / directory)
                if sha256(path) != sha256(original):
                    raise ValueError('nested system executable changed')
                system_executables['appsync/' + path.relative_to(work).as_posix()] = sha256(original)
    return dict(status='built', build=EXPECTED_BUILD, originals=originals, files=files,
                scope='all-user-bundles', system_executables=system_executables)


# ---------- trust manifest ----------

def trust_manifest(output):
    entries = {}
    for path in sorted(output.rglob('*')):
        if not path.is_file() or path.suffix in ('.json', '.plist', '.wav', '.txt'):
            continue
        with path.open('rb') as stream:
            magic = stream.read(4)
        if magic in MACHO_MAGICS or (magic[:2] == b'#!' and path.stat().st_mode & 0o111):
            detail = subprocess.run(['codesign', '--display', '--verbose=4', str(path)],
                                    capture_output=True, text=True)
            match = re.search(r'^CDHash=([0-9a-f]{40})$', detail.stderr, re.M)
            entries[path.relative_to(output).as_posix()] = dict(
                cdhash=match.group(1) if match else None, sha256=sha256(path))
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os-root', type=Path, required=True,
                        help='read-only mounted 24B119 OS DMG root')
    parser.add_argument('--cryptex-root', type=Path, required=True,
                        help='read-only mounted 24B119 SystemOS cryptex root')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-scripts', type=Path,
                        default=REPO / 'ios27/source/scripts',
                        help='upstream source scripts directory (ios27/source/scripts)')
    parser.add_argument('--components', type=Path,
                        default=REPO / 'ios27/sources',
                        help='upstream components directory (ios27/sources)')
    parser.add_argument('--gpu-addon', type=Path,
                        help='NGPUADD2 Metal addon to repackage with MarblePhoneUI')
    parser.add_argument('--skip-appsync', action='store_true')
    parser.add_argument('--skip-wifi', action='store_true')
    parser.add_argument('--skip-haptics', action='store_true')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.os_root = args.os_root.resolve()
    args.cryptex_root = args.cryptex_root.resolve()
    args.source_scripts = args.source_scripts.resolve()
    args.components = args.components.resolve()
    if args.gpu_addon:
        args.gpu_addon = args.gpu_addon.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    profile = json.loads((PROFILE / 'target.json').read_text())
    fixups = json.loads((PROFILE / 'fixups.json').read_text())

    report = {'target': 'ios27', 'build': EXPECTED_BUILD,
              'cache_uuid': EXPECTED_CACHE_UUID, 'steps': {}}
    report['steps']['validate_os'] = validate_os_root(args.os_root, profile)
    verified = validate_fixups(args.cryptex_root, fixups)
    report['steps']['fixups'] = dict(status='verified', count=len(verified), verified=verified)
    inputs = args.output / 'patch-inputs'
    inputs.mkdir(exist_ok=True)
    (inputs / 'text-fixups.txt').write_text(
        '\n'.join(item['entry'] for item in fixups['entries']) + '\n')
    (inputs / 'text-fixups.json').write_text(json.dumps(fixups, indent=2) + '\n')
    (inputs / 'hook-sources.json').write_text(json.dumps({
        'ui_policy': 'package/ios27-runtime/MarbleUiPolicy27.c',
        'crash_info': 'package/ios27-runtime/MarbleCrashInfo27.c',
        'crash_info_define': 'MARBLE_IOS27_GPU',
        'supersedes': {'ui_policy': 'package/MarbleUiPolicy.c',
                       'crash_info_block': 'package/MarbleSystemOS.c'},
    }, indent=2) + '\n')

    if not args.skip_wifi:
        try:
            report['steps']['wifi'] = build_wifi(args.components, args.os_root,
                                                 args.output / 'wifi-runtime')
        except Exception as error:
            report['steps']['wifi'] = dict(status='failed', error=str(error))

    # build_marble_wifi_userspace.py merges the supplicant job into the OS
    # launchd cache and signs it; surface that report here.
    wifi_report = args.output / 'wifi-runtime' / 'report.json'
    if wifi_report.is_file():
        launchd_report = json.loads(wifi_report.read_text()).get('launchd_cache')
        if launchd_report:
            cache = args.output / 'wifi-runtime' / 'xpc' / 'launchd.plist'
            signature = cache.with_suffix('.plist.sig')
            report['steps']['launchd'] = dict(
                status='built', cache=str(cache), signature=str(signature),
                **launchd_report)
        else:
            report['steps']['launchd'] = dict(status='failed',
                                              error='wifi report lacks launchd_cache')
    else:
        report['steps']['launchd'] = dict(
            status='skipped', reason='wifi runtime did not produce a launchd cache')

    if not args.skip_haptics:
        try:
            report['steps']['haptics'] = build_haptics(args.os_root,
                                                       args.output / 'haptics', profile)
        except Exception as error:
            report['steps']['haptics'] = dict(status='failed', error=str(error))
    if args.gpu_addon:
        try:
            report['steps']['phone_ui'] = build_phone_ui(args.gpu_addon,
                                                       args.output / 'phone-ui')
        except Exception as error:
            report['steps']['phone_ui'] = dict(status='failed', error=str(error))
    else:
        report['steps']['phone_ui'] = dict(status='pending',
                                           reason='--gpu-addon not supplied')
    addon_path = args.output / 'phone-ui' / 'phone-ui.gpuaddon'
    if addon_path.is_file():
        try:
            report['steps']['wallpaper'] = merge_wallpaper(
                addon_path, args.os_root, args.output / 'wallpaper-cache',
                profile['wallpaper'])
        except Exception as error:
            report['steps']['wallpaper'] = dict(status='failed', error=str(error))
    if not args.skip_appsync:
        try:
            report['steps']['appsync'] = build_appsync(args.source_scripts, args.os_root,
                                                       args.output / 'appsync', profile)
        except Exception as error:
            report['steps']['appsync'] = dict(status='failed', error=str(error))

    report['trust_manifest'] = trust_manifest(args.output)
    report['input_hashes'] = {
        'fixups.json': sha256(PROFILE / 'fixups.json'),
        'target.json': sha256(PROFILE / 'target.json'),
        'MarbleUiPolicy27.c': sha256(REPO / 'package/ios27-runtime/MarbleUiPolicy27.c'),
        'MarbleCrashInfo27.c': sha256(REPO / 'package/ios27-runtime/MarbleCrashInfo27.c'),
    }
    (args.output / 'runtime.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: (v.get('status') if isinstance(v, dict) else v)
                      for k, v in report['steps'].items()}, indent=2))


if __name__ == '__main__':
    main()
