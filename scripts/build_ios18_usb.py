"""Bind the upstream DWC3 device controller to its iOS 18 binary interface."""
import hashlib
import json
from pathlib import Path
import struct
from collections import defaultdict
import plistlib
import sys

DEVICE = Path(__file__).resolve().parents[1]
PROFILE = DEVICE / 'profiles/usb-xdci-22G100.json'


def generate(directory):
    abi = json.loads(PROFILE.read_text())
    import tomllib
    sys.path.insert(0, str(DEVICE / 'ios18/sources/XnuPrelink'))
    from kc_image import Image
    workspace = DEVICE.parent.parent
    base = Image((workspace / 'outputs/UniXNU_marble/ios18_arm64e/ipsw/22H384/kernelcache.decompressed').read_bytes())
    donor = Image((workspace / abi['source']).read_bytes())
    roots = tomllib.loads((DEVICE / 'profiles/ios-18.7.11-22H384.toml').read_text())['kernel']['roots']
    _, abi['placements'] = component_layout(base, donor, roots)
    PROFILE.write_text(json.dumps(abi, indent=1) + '\n')
    methods = {int(k): v for k, v in abi['methods'].items()}
    macros = {179: 'XDCI_KEEP_CONFIGURATIONS',
              204: 'XDCI_CABLE_CONNECT', 205: 'XDCI_CABLE_DISCONNECT',
              260: 'XDCI_TRANSPORT_ACTIVE',
              261: 'XDCI_POWER_ON', 262: 'XDCI_POWER_OFF', 263: 'XDCI_PARAMETERS',
              268: 'XDCI_CONTROLLER_RESET', 270: 'XDCI_PERIODIC_USAGE', 271: 'XDCI_PERIODIC_SET'}
    header = '#pragma once\n#include <IOKit/IOService.h>\n'
    for slot, name in macros.items():
        header += f'#define {name} {methods[slot]["name"]}\n'
    header += '''class AppleUSBXDCI : public IOService {
 OSDeclareAbstractStructors(AppleUSBXDCI)
 unsigned char originalPrivate[0x2440-sizeof(IOService)];
public:
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool terminate(IOOptionBits options=0) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
 unsigned long maxCapabilityForDomainState(IOPMPowerFlags) override;
 unsigned long initialPowerStateForDomainState(IOPMPowerFlags) override;
 IOReturn powerStateWillChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;
 IOReturn powerStateDidChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;
'''
    for slot in range(168, len(abi['slots'])):
        method = methods[slot]
        pure = '=0' if abi['slots'][slot][0] == abi['pure_virtual'] else ''
        header += f' virtual {method["returns"]} {method["name"]}({method["parameters"]}){pure};\n'
    header += '};\nstatic_assert(sizeof(AppleUSBXDCI)==0x2440,"iOS 18 controller layout");\n'
    (directory / 'OriginalUsbXdciABI.h').write_text(header)


def bindings(obj):
    abi = json.loads(PROFILE.read_text())
    def moved(address):
        for begin, end, target in abi['placements']:
            if begin <= address < end:
                return address - begin + target
        raise RuntimeError(f'USB binding is outside the imported component: {address:#x}')
    raw = obj.read_bytes()
    sections, symbols, position = [], [], 32
    for _ in range(struct.unpack_from('<I', raw, 16)[0]):
        command, length = struct.unpack_from('<II', raw, position)
        if command == 0x19:
            for index in range(struct.unpack_from('<I', raw, position + 64)[0]):
                section = position + 72 + index * 80
                va, size, fileoff = struct.unpack_from('<QQI', raw, section + 32)
                reloc, count = struct.unpack_from('<II', raw, section + 56)
                sections.append((va, size, fileoff, reloc, count))
        elif command == 2:
            symoff, count, stroff, _ = struct.unpack_from('<4I', raw, position + 8)
            for index in range(count):
                string, kind, _, _, value = struct.unpack_from('<IBBHQ', raw, symoff + index * 16)
                symbols.append((raw[stroff + string:raw.index(b'\0', stroff + string)].decode(), value, kind))
        position += length
    entries = {}
    for va, size, fileoff, reloc, count in sections:
        for index in range(count):
            offset, bits = struct.unpack_from('<II', raw, reloc + index * 8)
            if bits >> 27 & 1 and bits >> 28 == 11 and bits >> 25 & 3 == 3:
                word = struct.unpack_from('<Q', raw, fileoff + offset)[0]
                entries[va + offset] = (symbols[bits & 0xffffff][0], word)
    data = {'__ZN12AppleUSBXDCI10gMetaClassE', '__ZTV12AppleUSBXDCI'}
    sys.path.insert(0, str(DEVICE / 'ios18/sources/XnuPrelink'))
    from kc_image import Image
    root = DEVICE.parent.parent / 'outputs/UniXNU_marble/ios18_arm64e'
    original = Image((root / 'ipsw/22H384/kernelcache.decompressed').read_bytes())
    kernel = Image(original.data, original.entries['com.apple.kernel'][1])
    core_map = json.loads((root / 'kernel.fileset.map.json').read_text())
    aliases = {}
    result = {n: moved(int(a, 16)) for n, a in abi['bindings'].items() if n not in data}
    table = next(value for name, value, _ in symbols if name == '__ZTV12QcomDwc3Xdci')
    for slot, (target, diversity) in enumerate(abi['slots']):
        name, word = entries[table + 16 + slot * 8]
        if word >> 32 & 0xffff != int(diversity, 16):
            raise RuntimeError(f'iOS 18 USB vtable slot {slot} authentication differs')
        if '12AppleUSBXDCI' in name:
            if target == abi['pure_virtual']:
                raise RuntimeError(f'USB controller has an abstract slot {slot}')
            address = int(target, 16)
            # IOService inherited implementations remain in the target kernel.
            if 0xfffffff009e20700 <= address < 0xfffffff009e4185c:
                result[name] = moved(address)
            else:
                target = int(abi['imports'][hex(address)], 16)
                if any(s['name'] == '__TEXT_EXEC' and s['va'] <= target < s['va'] + s['size']
                       for s in kernel.segments):
                    aliases[name] = core_map['functions'][hex(target)]['name']
                else:
                    result[name] = target
    return result, {n: moved(int(abi['bindings'][n], 16)) for n in data}, aliases


def component_layout(base, source, roots):
    from kc_image import Image
    from fileset_emit import dependency_closure
    abi = json.loads(PROFILE.read_text())
    closure = dependency_closure(base, [n for n in roots if n != abi['bundle']])
    closure.add('com.apple.kernel')
    occupied = []
    for name in closure:
        if name in base.entries:
            for segment in Image(base.data, base.entries[name][1]).segments:
                if segment['name'] != '__LINKEDIT' and segment['size']:
                    occupied.append((segment['va'], segment['va'] + segment['size']))
    entry = Image(source.data, source.entries[abi['bundle']][1])
    names = {'__TEXT': '__PRELINK_TEXT', '__TEXT_EXEC': '__TEXT_EXEC',
             '__DATA': '__DATA', '__DATA_CONST': '__DATA_CONST'}
    placements = []
    for segment in entry.segments:
        if segment['name'] not in names:
            continue
        region = next(s for s in base.segments if s['name'] == names[segment['name']])
        begin, end = region['va'], region['va'] + region['size']
        ranges = sorted((max(x, begin), min(y, end)) for x, y in occupied if x < end and y > begin)
        gaps, cursor = [], begin
        for x, y in ranges + [(end, end)]:
            if x > cursor:
                gaps.append((cursor, x))
            cursor = max(cursor, y)
        for lo, hi in reversed(gaps):
            address = ((hi - segment['size']) & ~4095) + (segment['va'] & 4095)
            if address + segment['size'] > hi:
                address -= 4096
            if address >= lo:
                placements.append([segment['va'], segment['va'] + segment['size'], address])
                occupied.append((address, address + segment['size']))
                break
        else:
            raise RuntimeError(f'No collection space for USB {segment["name"]}')
    return closure, placements


def library_cache(original, destination):
    """Import the missing DWC3 class into unselected component space."""
    sys.path.insert(0, str(DEVICE / 'ios18/sources/XnuPrelink'))
    from kc_image import Image, scan, signed
    from fileset_emit import chain_word, prelink_info, Emit
    from fileset_shape import checked_shape
    import tomllib
    abi = json.loads(PROFILE.read_text())
    raw = original.read_bytes()
    donor = (DEVICE.parent.parent / abi['source']).read_bytes()
    if hashlib.sha256(donor).hexdigest() != abi['kernelcache_sha256'] or hashlib.sha256(raw).hexdigest() != abi['target_kernelcache_sha256']:
        raise RuntimeError('USB collection inputs differ from their version pins')
    base, source = Image(raw), Image(donor)
    roots = tomllib.loads((DEVICE / 'profiles/ios-18.7.11-22H384.toml').read_text())['kernel']['roots']
    closure, placements = component_layout(base, source, roots)
    if placements != abi['placements']:
        raise RuntimeError('USB component placement differs from its compiled bindings')
    entry = Image(donor, source.entries[abi['bundle']][1])
    segments = [s for s in entry.segments if s['name'] != '__LINKEDIT']
    imports = {int(a, 16): int(b, 16) for a, b in abi['imports'].items()}
    def translated(address):
        for lo, hi, target in placements:
            if lo <= address < hi:
                return address - lo + target
        if address not in imports:
            raise RuntimeError(f'Unbound USB reference {address:#x}')
        return imports[address]
    left = Image(donor, source.entries['com.apple.iokit.IOUSBDeviceFamily'][1])
    right = Image(raw, base.entries['com.apple.iokit.IOUSBDeviceFamily'][1])
    for ls in left.segments:
        if ls['name'] == '__LINKEDIT':
            continue
        rs = next(s for s in right.segments if s['name'] == ls['name'])
        if ls['filesize'] != rs['filesize']:
            raise RuntimeError('USB family storage layout differs')
        if ls['name'] == '__TEXT_EXEC' and checked_shape(source.read(ls['va'], ls['filesize']), ls['va'])[0] != checked_shape(base.read(rs['va'], rs['filesize']), rs['va'])[0]:
            raise RuntimeError('USB family implementation differs')
    if source.read(0xfffffff00a6c7d08, 40) != base.read(0xfffffff00a5017d8, 40):
        raise RuntimeError('USB common logging object differs')
    data = bytearray(raw)
    owned = []
    for name in closure:
        if name in base.entries:
            for segment in Image(raw, base.entries[name][1]).segments:
                if segment['name'] != '__LINKEDIT' and segment['size']:
                    owned.append((segment['va'], segment['va'] + segment['size']))
    pointers = {a: p for a, p in base.fixups().items() if any(lo <= a < hi for lo, hi in owned)}
    for segment in segments:
        at = base.offset(translated(segment['va']), segment['filesize'])
        data[at:at + segment['filesize']] = source.read(segment['va'], segment['filesize'])
    for address, pointer in source.fixups().items():
        if any(lo <= address < hi for lo, hi, _ in placements):
            pointers[translated(address)] = (translated(pointer[0]), *pointer[1:])
    patcher = object.__new__(Emit)
    patcher.data, patcher.report = data, defaultdict(int)
    for segment in segments:
        if segment['name'] != '__TEXT_EXEC':
            continue
        delta = translated(segment['va']) - segment['va']
        moved = dict(segment, va=segment['va'] + delta, fileoff=base.offset(segment['va'] + delta))
        calls, references, _ = scan(source.read(segment['va'], segment['filesize']), segment['va'], with_origins=True)
        for pc, target, _ in calls:
            displacement = translated(target) - (pc + delta)
            if displacement % 4 or not -(1 << 27) <= displacement < 1 << 27:
                raise RuntimeError('USB import branch exceeds its range')
            at = base.offset(pc + delta)
            word = struct.unpack_from('<I', data, at)[0]
            struct.pack_into('<I', data, at, (word & 0xfc000000) | ((displacement >> 2) & 0x3ffffff))
        groups = defaultdict(list)
        for pc, target, kind, producer, scale in references:
            rebound = translated(target)
            if producer is None:
                immediate = rebound - (pc + delta)
                if not -(1 << 20) <= immediate < 1 << 20:
                    raise RuntimeError('USB import ADR exceeds its range')
                at = base.offset(pc + delta)
                word = struct.unpack_from('<I', data, at)[0]
                struct.pack_into('<I', data, at, (word & 0x9f00001f) | (((immediate >> 2) & 0x7ffff) << 5) | ((immediate & 3) << 29))
            else:
                groups[producer].append((pc, rebound, scale))
        for producer, uses in groups.items():
            if len({target & ~4095 for _, target, _ in uses}) != 1:
                raise RuntimeError('USB ADRP consumers require different pages')
            for pc, target, scale in uses:
                patcher.patch_adrp_pair(moved, producer - segment['va'], pc - segment['va'], target, scale)
    new_header = base.offset(translated(entry.base))
    for command, old_at, size in entry.commands:
        at = new_header + old_at - entry.at
        if command == 0x19:
            name = donor[old_at + 8:old_at + 24].rstrip(b'\0').decode()
            if name != '__LINKEDIT':
                segment = next(s for s in segments if s['name'] == name)
                delta = translated(segment['va']) - segment['va']
                struct.pack_into('<Q', data, at + 24, translated(segment['va']))
                struct.pack_into('<Q', data, at + 40, base.offset(translated(segment['va'])))
                for index in range(struct.unpack_from('<I', donor, old_at + 64)[0]):
                    section = at + 72 + index * 80
                    address = struct.unpack_from('<Q', data, section + 32)[0]
                    offset = struct.unpack_from('<I', data, section + 48)[0]
                    struct.pack_into('<Q', data, section + 32, address + delta)
                    if offset:
                        struct.pack_into('<I', data, section + 48, offset - segment['fileoff'] + base.offset(segment['va'] + delta))
            else:
                link = next(s for s in base.segments if s['name'] == '__LINKEDIT')
                struct.pack_into('<QQQQ', data, at + 24, link['va'], link['size'], link['fileoff'], link['filesize'])
        elif command in (0x26, 0x29, 0x1e, 0x1d, 0x80000033, 0x80000034):
            struct.pack_into('<II', data, at + 8, 0, 0)
        elif command in (2, 0xb):
            data[at + 8:at + size] = bytes(size - 8)
    info_segment, metadata = prelink_info(base)
    _, donor_info = prelink_info(source)
    record = dict(next(e for e in donor_info['_PrelinkInfoDictionary'] if e['CFBundleIdentifier'] == abi['bundle']))
    for field in ('_PrelinkExecutableLoadAddr', '_PrelinkExecutableSourceAddr', '_PrelinkKmodInfo'):
        record[field] = signed(translated(record[field] & ((1 << 64) - 1)), 64)
    kmod = base.offset(record['_PrelinkKmodInfo'] & ((1 << 64) - 1))
    address = struct.unpack_from('<Q', data, kmod + 156)[0]
    struct.pack_into('<Q', data, kmod + 156, translated(address))
    metadata['_PrelinkInfoDictionary'] = [e for e in metadata['_PrelinkInfoDictionary'] if e['CFBundleIdentifier'] in closure] + [record]
    xml = plistlib.dumps(metadata, sort_keys=False) + b'\0'
    if len(xml) > info_segment['filesize']:
        raise RuntimeError('USB metadata exceeds its reservation')
    at = info_segment['fileoff'];data[at:at + info_segment['filesize']] = xml + bytes(info_segment['filesize'] - len(xml))
    commands, count = bytearray(), 0
    for command, at, size in base.commands:
        if command == 0x80000035:
            name_at = struct.unpack_from('<I', raw, at + 24)[0]
            name = raw[at + name_at:raw.index(b'\0', at + name_at)].decode()
            if name not in closure:
                continue
        commands.extend(data[at:at + size]);count += 1
    name = abi['bundle'].encode() + b'\0';size = (32 + len(name) + 7) & ~7
    added = struct.pack('<IIQQII', 0x80000035, size, translated(entry.base), new_header, 32, 0) + name
    commands.extend(added + bytes(size - len(added)));count += 1
    data[32:32 + len(commands)] = commands;struct.pack_into('<II', data, 16, count, len(commands))
    image = Image(bytes(data));roots = image.segments;page = 0x4000
    grouped = defaultdict(lambda: defaultdict(list))
    for site, pointer in sorted(pointers.items()):
        index = next(i for i, s in enumerate(roots) if s['va'] <= site and site + 8 <= s['va'] + s['filesize'])
        relative = site - roots[index]['va'];grouped[index][relative // page].append((relative % page, pointer))
    starts = bytearray(struct.pack('<I', len(roots)) + bytes(4 * len(roots)))
    for index, pages in grouped.items():
        segment=roots[index];pages_count=max(pages)+1
        piece=bytearray(struct.pack('<IHHQIH',22+2*pages_count,page,8,segment['va']-base.base,0,pages_count)+b'\xff\xff'*pages_count)
        for number, sites in pages.items():
            struct.pack_into('<H',piece,22+2*number,sites[0][0])
            for i,(offset,pointer) in enumerate(sites):
                target,auth,key,diversity,addrdiv=pointer
                step=(sites[i+1][0]-offset)//4 if i+1<len(sites) else 0
                struct.pack_into('<Q',data,segment['fileoff']+number*page+offset,chain_word(auth,key or 0,diversity or 0,addrdiv or 0,target-base.base,step))
        struct.pack_into('<I',starts,4+4*index,len(starts));starts.extend(piece)
    fixups=struct.pack('<7I',0,28,28,28,0,1,0)+starts;fixups_at=(len(data)+15)&~15
    data.extend(bytes(fixups_at-len(data)));data.extend(fixups)
    at=image.command(0x80000034)[0];struct.pack_into('<II',data,at+8,fixups_at,len(fixups))
    link=next(s for s in roots if s['name']=='__LINKEDIT');at=link['command_offset']
    struct.pack_into('<Q',data,at+32,(len(data)-link['fileoff']+page-1)&-page)
    struct.pack_into('<Q',data,at+48,len(data)-link['fileoff'])
    normalize=lambda entries:{a:(p[0],p[1],p[2] or 0,p[3] or 0,p[4] or 0) for a,p in entries.items()}
    if normalize(Image(bytes(data)).fixups())!=normalize(pointers):
        raise RuntimeError('USB import changed its pointer chains')
    destination.write_bytes(data)
    return destination


def prelink(output, original, native, roots, security):
    sys.path.insert(0, str(DEVICE / 'ios18/sources/XnuPrelink'))
    from fileset_match import match_images, MATCHER_VERSION, matcher_signature
    from fileset_emit import emit_images, write_candidate
    raw, native_data = original.read_bytes(), native.read_bytes()
    expected = {'kernelcache_sha256': hashlib.sha256(raw).hexdigest(),
                'native_sha256': hashlib.sha256(native_data).hexdigest(), 'entry': 'com.apple.kernel'}
    map_path = output / 'kernel.usb-base.map.json'
    pairing = json.loads(map_path.read_text()) if map_path.is_file() else None
    if (pairing is None or pairing.get('inputs') != expected or
            pairing.get('matcher_version') != MATCHER_VERSION or pairing.get('matcher_signature') != matcher_signature()):
        pairing = match_images(raw, native_data)
        map_path.write_text(json.dumps(pairing, indent=1) + '\n')
    combined = library_cache(original, output / 'kernelcache.usb')
    combined_data = combined.read_bytes()
    pairing['inputs'] = dict(expected, kernelcache_sha256=hashlib.sha256(combined_data).hexdigest())
    (output / 'kernel.fileset.map.json').write_text(json.dumps(pairing, indent=1) + '\n')
    image, report = emit_images(combined_data, native_data, pairing, roots,
                                early_security_init=security, base_cache_data=raw)
    report.update(kernelcache=str(combined), native=str(native),
                  base_kernelcache_sha256=expected['kernelcache_sha256'],
                  controller_profile=str(PROFILE), map=str(map_path))
    destination = output / 'kernel.fileset.macho'
    write_candidate(image, report, destination, destination.with_suffix('.json'), False)
    return destination
