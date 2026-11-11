#!/usr/bin/env python3
"""Pair a symbol-bearing fileset kernel with a native kernel by symbol name.

A kernel collection that keeps the kernel's full symbol table (macOS kernel collections do) names every
address its kexts reference. Each such target is paired with the native kernel's definition of the same
name; nothing is inferred from code. The payload has the layout of fileset_match.py's, so fileset_emit.py
consumes either; `method` tells them apart and `matcher_signature` covers this file.

A target that no symbol of the original kernel starts at, or whose names the native kernel does not
define, is reported under `missing` (with the kexts that reference it) and left unpaired.
"""
import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import hashlib
import json
import struct
from pathlib import Path

from kc_image import Image

METHOD = 'symbol-names'
EXEC_SEGMENTS = ('__TEXT_EXEC', '__TEXT_BOOT_EXEC', '__KLD', '__LAST')


def names_signature():
    """Invalidate cached maps when this pairing or the image reader changes."""
    root = Path(__file__).parent
    digest = hashlib.sha256()
    for path in (Path(__file__), root / 'kc_image.py'):
        digest.update(path.name.encode() + b'\0' + path.read_bytes())
    return digest.hexdigest()


def expected_signature(payload):
    """The signature a pairing must carry: this file's for a name map, the code matcher's otherwise."""
    from fileset_match import matcher_signature
    return names_signature() if payload.get('method') == METHOD else matcher_signature()


def symbol_bearing(raw, entry_name='com.apple.kernel', minimum=4000):
    """True when the collection's kernel entry carries a full symbol table."""
    cache = Image(raw)
    if cache.kind != 12 or entry_name not in cache.entries:
        return False
    entry = Image(raw, cache.entries[entry_name][1])
    return sum(len(names) for names in entry.symbols().values()) >= minimum


def external_definitions(image):
    """Name -> address of each section symbol defined once. An external definition wins over file-local
    statics of the same name (the kernel has a static xts_encrypt beside the exported one); a name
    defined more than once at the same level is left out."""
    found = image.command(0x2)
    by_name = {}
    if not found:
        return {}
    symoff, count, stroff, _ = struct.unpack_from('<4I', image.data, found[0] + 8)
    for index in range(count):
        strx, kind, _, _, value = struct.unpack_from('<IBBHQ', image.data, symoff + 16 * index)
        if kind & 0xe0 or kind & 0x0e != 0x0e:
            continue
        end = image.data.index(b'\0', stroff + strx)
        name = image.data[stroff + strx:end].decode()
        by_name.setdefault(name, {}).setdefault(bool(kind & 1), set()).add(value)
    result = {}
    for name, levels in by_name.items():
        chosen = levels.get(True) or levels.get(False)
        if len(chosen) == 1:
            result[name] = next(iter(chosen))
    return result


def owner_of(cache, raw):
    """Address ranges of every fileset entry, for naming the kexts that reference a target."""
    ranges = []
    for name, (_, at) in cache.entries.items():
        for segment in Image(raw, at).segments:
            if segment['size'] and segment['name'] != '__LINKEDIT':
                ranges.append((segment['va'], segment['va'] + segment['size'], name))
    ranges.sort()
    starts = [r[0] for r in ranges]

    def find(address):
        index = bisect_right(starts, address) - 1
        if index >= 0 and ranges[index][0] <= address < ranges[index][1]:
            return ranges[index][2]
        return None
    return find


def match_by_names(raw, native_data, entry_name='com.apple.kernel'):
    cache = Image(raw)
    if entry_name not in cache.entries:
        raise ValueError(f'No fileset entry {entry_name!r}')
    original = Image(raw, cache.entries[entry_name][1])
    native = Image(native_data)
    if native.kind != 2:
        raise ValueError('Expected an MH_EXECUTE native kernel')
    kernel = original.mapped()
    inside = lambda address: any(b <= address < e for b, e in kernel)  # noqa: E731
    exec_ranges = [(s['va'], s['va'] + s['size']) for s in original.segments if s['name'] in EXEC_SEGMENTS and s['size']]
    executable = lambda address: any(b <= address < e for b, e in exec_ranges)  # noqa: E731
    owner = owner_of(cache, raw)
    referrers = defaultdict(set)
    for site, fix in cache.fixups().items():
        if inside(fix[0]) and not inside(site):
            referrers[fix[0]].add(owner(site) or '?')
    original_names = original.symbols()
    native_by_name = external_definitions(native)
    functions, data, missing, conflicts = {}, {}, {}, []
    for target in sorted(referrers):
        names = sorted(original_names.get(target, ()))
        found = {native_by_name[name] for name in names if name in native_by_name}
        if not names or not found:
            missing[f'{target:#x}'] = {'names': names, 'referrers': sorted(referrers[target])}
            continue
        if len(found) != 1:
            conflicts.append({'kind': 'names-disagree', 'original': f'{target:#x}', 'names': names,
                              'native': sorted(f'{a:#x}' for a in found)})
            continue
        address = found.pop()
        name = next(n for n in names if n in native_by_name)
        record = {'native': f'{address:#x}', 'name': name, 'how': 'symbol'}
        if executable(target):
            functions[f'{target:#x}'] = dict(record, addend=0)
        else:
            section = original.section(target)
            native_section = native.section(address)
            data[f'{target:#x}'] = dict(record, sections=[
                f"{section['segment']},{section['name']}" if section else '?',
                f"{native_section['segment']},{native_section['name']}" if native_section else '?'])
    from fileset_match import MATCHER_VERSION
    payload = {
        'matcher_version': MATCHER_VERSION,
        'matcher_signature': names_signature(),
        'method': METHOD,
        'functions': functions,
        'data': data,
        'missing': missing,
        'conflicts': conflicts[:100],
        'stats': {
            'kext_targets': len(referrers),
            'functions_matched': len(functions),
            'data_matched': len(data),
            'missing': len(missing),
            'conflicts': dict(Counter(c['kind'] for c in conflicts)),
            'missing_by_referrer': dict(Counter(r for record in missing.values() for r in record['referrers'])),
        },
        'inputs': {'kernelcache_sha256': hashlib.sha256(raw).hexdigest(),
                   'native_sha256': hashlib.sha256(native_data).hexdigest(), 'entry': entry_name},
    }
    print(f'by name: {len(functions)} functions, {len(data)} data of {len(referrers)} kext-facing targets; '
          f'{len(missing)} unpaired, {len(conflicts)} conflicts')
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    parser.add_argument('native', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    payload = match_by_names(args.kernelcache.read_bytes(), args.native.read_bytes())
    for target, record in sorted(payload['missing'].items(), key=lambda item: item[1]['names']):
        print('missing', target, ' '.join(record['names']) or '(no symbol)', '<-', ', '.join(record['referrers'][:4]))
    if args.out:
        args.out.write_text(json.dumps(payload, indent=1) + '\n')


if __name__ == '__main__':
    main()
