#!/usr/bin/env python3
"""iOS 27.2 (24B119, arm64e, XNU 12377.121.6) marble original-class driver build.

Derives the bound original-class ABI from the 24B119 kernelcache collection:
every vtable address, slot count, per-slot PAC discriminator, constructor,
destructor, metaclass and instance size is read from the target collection;
nothing reuses 22H384 offsets. Per-class discriminators are computed with
Clang's arm64e rule, verifierd against compiled probes:

    diversity = SipHash-2-4-64(mangled root-declaration name, K) % 0xffff + 1

Generated Original*ABI.h headers declare opaque slots under names whose
discriminator matches what original callers authenticate with, plus #define
aliases for the source-facing names (unclassified<N>, abiSlot<N>, XDCI_*).
Owned subclasses compile against the assembled 12377 tree's compile records;
undefined original imports are bound at their 24B119 collection addresses with
absolute data symbols and BTI-aware function stubs. Output:

    build/marble-boot/libMarbleBoot.a
    build/marble-storage/libMarbleStorage.a
    build/marble-gpu/libMarbleGpu.a

Public API: derive_abi(), build_family(), build_all(), check(). CLI:

    build_ios27_drivers.py derive --kernelcache K [--output D]
    build_ios27_drivers.py build  --tree T --kernelcache K [--output D] [--family F]
    build_ios27_drivers.py check  --tree T --kernelcache K [--output D]
"""
import argparse
import bisect
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parents[1]
DEFAULT_TREE = WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119/tree'
DEFAULT_KERNELCACHE = (WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119/ipsw/24B119'
                       / 'kernelcache.decompressed')
PRIOR_KERNELCACHE = (WORKSPACE / 'outputs/UniXNU_marble/ios18_arm64e/ipsw/22H384'
                     / 'kernelcache.decompressed')
ABI_PROFILES = ROOT / 'profiles/abi/ios27-24B119'
RECORDS = ROOT / 'records/ios27-24B119'
SCRATCH = ROOT / 'build/ios27-drivers'
TARGET = {'os': 'iOS', 'version': '27.2', 'build': '24B119',
          'xnu': '12377.121.6', 'arch': 'arm64e', 'device': 'iPhone12,1',
          'board': 'N104AP'}
OBJ_ROOT = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
CXX_RECORD = 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_RECORD = 'bsd/DEVELOPMENT/kern_exec.o.json'
BASE_SLOTS = 168  # IOService virtual entries in 24B119 (verified derivation)


def fail(message):
    raise SystemExit(message)


def require(value, message):
    if not value:
        fail(message)


# ---------------------------------------------------------------------------
# XnuPrelink import (vendored in the staged tree / component sources).
# ---------------------------------------------------------------------------

_kc = None
_np = None


def import_xnuprelink(tree=None):
    global _kc, _np
    if _kc is not None:
        return _kc, _np
    bases = []
    if tree:
        bases += [Path(tree) / 'kits/XnuPrelink', Path(tree) / 'drivers/XnuPrelink']
    bases += [ROOT / 'ios27/sources/XnuPrelink', ROOT / 'ios18/sources/XnuPrelink']
    for base in bases:
        if (base / 'kc_image.py').exists():
            sys.path.insert(0, str(base))
            break
    else:
        fail('XnuPrelink kit (kc_image.py) not found; pass --tree with the kit staged')
    import kc_image
    import native_prelink
    _kc, _np = kc_image, native_prelink
    return _kc, _np


# ---------------------------------------------------------------------------
# arm64e vtable diversity: SipHash-2-4-64 with the key recovered from the
# installed LLVMSupport getPointerAuthStableSipHash. Verified against compiled
# probes: _ZN10IOWatchdog10enable3234Ev -> 0xe2f7, _ZN3Foo10enable3234Ev ->
# 0x4db7, _ZN9IOServiceD1Ev -> 0x16f0 (root-decl destructor), documented
# ptrauth_string_discriminator("isa") -> 0x6ae1.
# ---------------------------------------------------------------------------

SIPHASH_KEY = bytes.fromhex('b5d4c9eb79104a796fec8b1b428781d4')


def _rotl(value, count):
    return ((value << count) | (value >> (64 - count))) & 0xffffffffffffffff


def _sipround(v):
    v0, v1, v2, v3 = v
    v0 = (v0 + v1) & 0xffffffffffffffff
    v1 = _rotl(v1, 13) ^ v0
    v0 = _rotl(v0, 32)
    v2 = (v2 + v3) & 0xffffffffffffffff
    v3 = _rotl(v3, 16) ^ v2
    v0 = (v0 + v3) & 0xffffffffffffffff
    v3 = _rotl(v3, 21) ^ v0
    v2 = (v2 + v1) & 0xffffffffffffffff
    v1 = _rotl(v1, 17) ^ v2
    v2 = _rotl(v2, 32)
    return v0, v1, v2, v3


def siphash24(data):
    k0, k1 = struct.unpack_from('<2Q', SIPHASH_KEY)
    v = (0x736f6d6570736575 ^ k0, 0x646f72616e646f6d ^ k1,
         0x6c7967656e657261 ^ k0, 0x7465646279746573 ^ k1)
    length = len(data)
    for at in range(0, length - length % 8, 8):
        m, = struct.unpack_from('<Q', data, at)
        v = v[0], v[1], v[2], v[3] ^ m
        v = _sipround(_sipround(v))
        v = v[0] ^ m, v[1], v[2], v[3]
    b = length << 56
    for i, byte in enumerate(data[length - length % 8:]):
        b |= byte << (8 * i)
    v = v[0], v[1], v[2], v[3] ^ b
    v = _sipround(_sipround(v))
    v = v[0] ^ b, v[1], v[2] ^ 0xff, v[3]
    for _ in range(4):
        v = _sipround(v)
    return v[0] ^ v[1] ^ v[2] ^ v[3]


_sip_lib = None


def _load_sip_lib():
    """Optional compiled SipHash helper for brute-force speed."""
    global _sip_lib
    if _sip_lib is not None:
        return _sip_lib
    try:
        import ctypes
        cache = SCRATCH / 'libsip.dylib'
        if not cache.exists():
            cache.parent.mkdir(parents=True, exist_ok=True)
            source = SCRATCH / 'libsip.c'
            source.write_text(
                'unsigned long long sip24(const void *p, unsigned long long n,'
                ' const unsigned char k[16]);')
            source.write_text(r'''
#include <stdint.h>
static uint64_t rotl(uint64_t v, int c){return (v<<c)|(v>>(64-c));}
uint64_t sip24(const void *p, uint64_t n, const unsigned char k[16]){
  const unsigned char *d=p; uint64_t k0,k1,m,b;
  __builtin_memcpy(&k0,k,8); __builtin_memcpy(&k1,k+8,8);
  uint64_t v0=0x736f6d6570736575ULL^k0, v1=0x646f72616e646f6dULL^k1,
           v2=0x6c7967656e657261ULL^k0, v3=0x7465646279746573ULL^k1;
  uint64_t i;
#define R() v0+=v1;v1=rotl(v1,13);v1^=v0;v0=rotl(v0,32);v2+=v3;v3=rotl(v3,16);v3^=v2;v0+=v3;v3=rotl(v3,21);v3^=v0;v2+=v1;v1=rotl(v1,17);v1^=v2;v2=rotl(v2,32);
  for(i=0;i+8<=n;i+=8){__builtin_memcpy(&m,d+i,8);v3^=m;R();R();v0^=m;}
  b=n<<56; for(i=0;i<n%8;i++)b|=(uint64_t)d[n-n%8+i]<<(8*i);
  v3^=b;R();R();v0^=b;v2^=0xff;R();R();R();R();
  return v0^v1^v2^v3;
}
''')
            subprocess.check_call(['clang', '-O2', '-shared', '-o', str(cache), str(source)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        _sip_lib = ctypes.CDLL(str(cache))
        _sip_lib.sip24.restype = ctypes.c_uint64
        _sip_lib.sip24.argtypes = [ctypes.c_char_p, ctypes.c_uint64, ctypes.c_char_p]
    except Exception:
        _sip_lib = False
    return _sip_lib or None


def siphash24_fast(data):
    lib = _load_sip_lib()
    if lib:
        return lib.sip24(data, len(data), SIPHASH_KEY)
    return siphash24(data)


def discriminator(mangled):
    """Clang's arm64e vtable diversity for a mangled root-declaration name."""
    return siphash24(mangled.encode()) % 0xffff + 1


def discriminator_fast(mangled_bytes):
    return siphash24_fast(mangled_bytes) % 0xffff + 1


assert discriminator('isa') == 0x6ae1
assert discriminator('_ZN10IOWatchdog10enable3234Ev') == 0xe2f7
assert discriminator('_ZN3Foo10enable3234Ev') == 0x4db7


def mangle(classname, decl, signature='Ev', const=False):
    """Itanium mangled name for classname::decl(signature); const raises _ZNK."""
    k = 'K' if const else ''
    return f'_ZN{k}{len(classname)}{classname}{len(decl)}{decl}{signature}'


def find_disc_name(classname, label, slot, want, signature='Ev', const=False):
    """Shortest `label<slot>_<counter>` name hashing to diversity `want`."""
    k = 'K' if const else ''
    head = f'_ZN{k}{len(classname)}{classname}'
    for counter in range(1, 4000000):
        name = f'{label}{slot}_{counter}'
        if discriminator_fast(f'{head}{len(name)}{name}{signature}'.encode()) == want:
            return name
    return None


# ---------------------------------------------------------------------------
# ARM64 instruction decoding (constructor / metaclass identification).
# ---------------------------------------------------------------------------

def _s(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def bitmask_imm(n_bit, imms, immr, width):
    """ARM bitmask-immediate decode for mov/orr #imm aliases."""
    length = 31 - (imms | (n_bit << 6)).bit_length() + 1 if (imms | (n_bit << 6)) else 0
    # Standard algorithm: find the highest set bit of (N:~imms).
    combined = (n_bit << 6) | (~imms & 0x3f)
    length = combined.bit_length() - 1
    require(0 < length < 7, f'invalid bitmask immediate N={n_bit} imms={imms:#x}')
    esize = 1 << length
    require(esize <= width, 'bitmask element wider than register')
    levels = esize - 1
    s_val = imms & levels
    r_val = immr & levels
    require(s_val != levels, 'reserved bitmask immediate')
    welem = (1 << (s_val + 1)) - 1
    welem &= (1 << esize) - 1
    welem = ((welem >> r_val) | (welem << (esize - r_val))) & ((1 << esize) - 1)
    out = 0
    for base in range(0, width, esize):
        out |= welem << base
    return out & ((1 << width) - 1)


def decode_function(member, va, max_instructions=200):
    """Straight-line facts for the function at va inside a member image.

    Returns {'adds': {reg: target}, 'loads': {reg: addr}, 'movs': {reg: imm},
             'calls': [(pc,target)], 'stores': [(pc,target)], 'refs': [target]}.
    Only the last write to each register survives; unknown instructions drop
    page provenance like kc_image.scan.
    """
    code = member.read(va, max_instructions * 4)
    pages, adds, loads, movs, calls, stores, refs = {}, {}, {}, {}, [], [], []
    call_args = []

    def arg(reg):
        if reg in adds:
            return adds[reg]
        if reg in loads:
            return loads[reg]
        return movs.get(reg)

    for i in range(len(code) // 4):
        word, = struct.unpack_from('<I', code, 4 * i)
        pc = va + 4 * i
        if word & 0x9f000000 == 0x90000000:  # adrp
            imm = _s(((word >> 5) & 0x7ffff) << 2 | ((word >> 29) & 3), 21)
            pages[word & 31] = (pc & ~0xfff) + (imm << 12)
            continue
        base = (word >> 5) & 31
        if word & 0xff800000 == 0x91000000 and base in pages:  # add imm
            target = pages[base] + (((word >> 10) & 0xfff) << (12 if word & 0x400000 else 0))
            adds[word & 31] = target
            refs.append(target)
            pages.pop(base, None)
            continue
        if word & 0x3b000000 == 0x39000000 and base in pages:  # ldr/str uoff
            size = 4 if word & 0x04800000 == 0x04800000 else word >> 30
            target = pages[base] + (((word >> 10) & 0xfff) << size)
            if word & 0x00400000 == 0:
                stores.append((pc, target))
            else:
                loads[word & 31] = target
                refs.append(target)
            pages.pop(base, None)
            continue
        if word & 0x7f800000 in (0x52800000, 0xd2800000):  # movz 32/64
            reg = word & 31
            movs[reg] = ((word >> 5) & 0xffff) << (((word >> 21) & 3) * 16)
            continue
        if word & 0x7f800000 in (0x72800000, 0xf2800000):  # movk 32/64
            reg = word & 31
            shift = ((word >> 21) & 3) * 16
            movs[reg] = (movs.get(reg, 0) & ~(0xffff << shift)) \
                | (((word >> 5) & 0xffff) << shift)
            continue
        if word & 0x7f800000 in (0x32000000, 0xb2000000):  # orr #imm (mov alias)
            reg = word & 31
            try:
                movs[reg] = bitmask_imm((word >> 22) & 1, (word >> 10) & 0x3f,
                                        (word >> 16) & 0x3f,
                                        64 if word & 0x80000000 else 32)
            except SystemExit:
                movs.pop(reg, None)
            continue
        if word & 0x7c000000 == 0x14000000:
            if word >> 31:
                target = pc + _s(word & 0x3ffffff, 26) * 4
                calls.append((pc, target))
                call_args.append((target, {r: arg(r) for r in range(8)}))
            continue
        if word & 0xfffffc1f == 0xd65f0000:
            break
        if word >> 12 == 0xd5032 or word >> 16 == 0xd503:
            continue  # hint/nop/pac/bti: no register effect
        # mov rd, rm (orr register): carry the source register's value.
        if word & 0xffe0fc00 in (0xaa0003e0, 0x2a0003e0):
            src = (word >> 16) & 31
            val = arg(src)
            reg = word & 31
            if val is not None:
                movs[reg] = val
            else:
                movs.pop(reg, None)
            continue
        if word & 0xff800000 == 0x91000000 or word & 0x3b000000 == 0x39000000:
            continue  # add/ldr/str with an unknown base: value is opaque, keep movs
        pages.clear()
        movs.clear()
    return {'adds': adds, 'loads': loads, 'movs': movs, 'calls': calls,
            'call_args': call_args, 'stores': stores, 'refs': refs}


# ---------------------------------------------------------------------------
# Kernel collection access.
# ---------------------------------------------------------------------------


class Collection:
    def __init__(self, path):
        kc, _ = import_xnuprelink()
        self.path = Path(path)
        self.data = self.path.read_bytes()
        self.image = kc.Image(self.data)
        self.members = {}
        for name, (_, fileoff) in self.image.entries.items():
            if name.endswith('.symbolicated'):
                continue
            self.members[name] = kc.Image(self.data, fileoff)
        self.sha256 = hashlib.sha256(self.data).hexdigest()

    def member(self, bundle):
        return self.members.get(bundle)

    def exec_ranges(self, member):
        return [(s['va'], s['va'] + s['size']) for s in member.segments
                if s['name'] in ('__TEXT_EXEC', '__TEXT') and s['size']]

    def function_starts(self, member):
        return member.function_starts()

    def containing_function(self, member, va):
        starts = self.function_starts(member)
        i = bisect.bisect_right(starts, va) - 1
        return starts[i] if i >= 0 else None

    def find_cstring(self, member, text):
        needle = text.encode() + b'\0'
        hits = []
        for segment in member.segments:
            blob = self.data[segment['fileoff']:segment['fileoff'] + segment['filesize']]
            at = blob.find(needle)
            while at != -1:
                if at == 0 or not (65 <= blob[at - 1] <= 90 or 97 <= blob[at - 1] <= 122
                                   or 48 <= blob[at - 1] <= 57 or blob[at - 1] == 95):
                    hits.append(segment['va'] + at)
                at = blob.find(needle, at + 1)
        return hits

    def functions_referencing(self, member, targets):
        """Function starts in member whose code references any target VA."""
        targets = set(targets)
        found = []
        for lo, hi in self.exec_ranges(member):
            try:
                code = member.read(lo, hi - lo)
            except ValueError:
                # Trim an unbacked tail (e.g. __TEXT padding in the kernel).
                try:
                    end = min(hi, max((s['va'] + s['filesize'] for s in member.segments
                                       if s['va'] < hi and s['filesize']),
                                      default=lo))
                    code = member.read(lo, end - lo) if end > lo else b''
                except ValueError:
                    continue
                if not code:
                    continue
            starts = member.function_starts()
            # Scan raw code for adrp+add/ldr refs to targets, then attribute
            # to the containing function start.
            pages = {}
            for off in range(0, len(code) - 3, 4):
                word, = struct.unpack_from('<I', code, off)
                pc = lo + off
                if word & 0x9f000000 == 0x90000000:
                    imm = _s(((word >> 5) & 0x7ffff) << 2 | ((word >> 29) & 3), 21)
                    pages[word & 31] = (pc & ~0xfff) + (imm << 12)
                    continue
                base = (word >> 5) & 31
                hit = None
                if word & 0xff800000 == 0x91000000 and base in pages:
                    hit = pages[base] + (((word >> 10) & 0xfff) << (12 if word & 0x400000 else 0))
                    pages.pop(word & 31, None)
                elif word & 0x3b000000 == 0x39000000 and base in pages:
                    size = 4 if word & 0x04800000 == 0x04800000 else word >> 30
                    hit = pages[base] + (((word >> 10) & 0xfff) << size)
                    pages.pop(word & 31, None)
                else:
                    pages.clear()
                    continue
                if hit in targets:
                    i = bisect.bisect_right(starts, pc) - 1
                    if i >= 0 and starts[i] not in found:
                        found.append(starts[i])
        return found


def fixups(collection):
    """Chained fixups live on the collection image, not per-member."""
    try:
        return collection.image.fixups()
    except (ValueError, struct.error):
        return {}


def vtable_entries(collection, address_point):
    """Per-entry {target, auth, key, diversity, address_diversity} from chained
    fixups starting at the address point (vtable + 0x10)."""
    fx = fixups(collection)
    entries = []
    site = address_point
    while site in fx:
        target, auth, key, div, adiv = fx[site]
        if not auth:
            break
        entries.append({'target': target, 'auth': auth, 'key': key,
                        'diversity': div, 'address_diversity': adiv})
        site += 8
    return entries


# ---------------------------------------------------------------------------
# Original-class derivation (verified recipe):
#  - functions referencing the class-name cstring
#  - the metaclass constructor loads gMetaClass (adrp+add) as x0, the cstring
#    as x1, the size in w3, calls OSMetaClass::OSMetaClass
#  - getMetaClass returns gMetaClass with no calls
#  - instance constructors store the signed vtable address point (a fixup site)
# ---------------------------------------------------------------------------


def _exec_set(collection, member):
    out = set()
    for lo, hi in collection.exec_ranges(member):
        out.add(lo)
        out.add(hi)
    return out


def _in_exec(collection, member, va):
    for lo, hi in collection.exec_ranges(member):
        if lo <= va < hi:
            return True
    return False


def _writable_data(collection, member, va):
    for s in member.segments:
        if s['va'] <= va < s['va'] + s['size'] and s['name'] not in (
                '__TEXT', '__TEXT_EXEC', '__DATA_CONST', '__LINKEDIT'):
            return True
    return False


def derive_class(collection, bundle, classname):
    member = collection.member(bundle)
    require(member is not None, f'{classname}: fileset {bundle} absent from collection')
    cstrings = collection.find_cstring(member, classname)
    require(cstrings, f'{classname}: class-name cstring not found in {bundle}')
    refs = collection.functions_referencing(member, cstrings)
    require(refs, f'{classname}: no code references the class-name cstring')
    facts = {start: decode_function(member, start) for start in refs}
    fix = fixups(collection)

    # Instance vtable candidates: address points (ref or ref+0x10) that are
    # auth fixup sites chaining >= BASE_SLOTS entries. A neighbouring class
    # shares the cstring family, so each candidate must prove itself.
    candidates = {}
    for start, fn in facts.items():
        for t in fn['refs']:
            for ap in (t, t + 0x10):
                if ap in fix and fix[ap][1]:
                    n = 0
                    while ap + 8 * n in fix and fix[ap + 8 * n][1]:
                        n += 1
                    if n:
                        candidates[ap - 0x10] = n
    big = {v: n for v, n in candidates.items() if n >= BASE_SLOTS}
    require(big, f'{classname}: no vtable with >= {BASE_SLOTS} entries among cstring refs')

    # Candidate identity comes from slot 7: the getMetaClass function returns
    # the actual gMetaClass. The metaclass is then proven by a registration
    # call in the cstring family naming this classname with a real size.
    proven = []
    for vt in sorted(big, key=big.get, reverse=True):
        ap = vt + 0x10
        entries = vtable_entries(collection, ap)
        getter = entries[7]['target'] if len(entries) > 7 else None
        meta_address = None
        if getter:
            gfn = decode_function(member, getter, 32)
            data_refs = [t for t in gfn['refs']
                         if _writable_data(collection, member, t)]
            if gfn['adds'].get(0) and _writable_data(
                    collection, member, gfn['adds'][0]):
                meta_address = gfn['adds'][0]
            elif data_refs:
                meta_address = data_refs[0]
        # Registration proof levels for this candidate's metaclass:
        #   3: a call presets x0 = metaclass, x1 = classname, w3 = size
        #   2: a call passes x1 = classname, w3 = size in a function that
        #      also references the metaclass
        #   1: the metaclass is referenced by the cstring family at all
        #   0: reject — the metaclass belongs to a neighbouring class
        meta_ctor, size, proof = None, None, 0
        for start, fn in facts.items():
            for _, args in fn['call_args']:
                if args.get(1) in cstrings and (args.get(3) or 0) > 0x20:
                    if args.get(0) == meta_address:
                        meta_ctor, size, proof = start, args[3], 3
                    elif proof < 3 and meta_address in fn['refs']:
                        meta_ctor, size, proof = start, args[3], 2
                    elif proof == 0:
                        size = args[3]
            if meta_address is not None and meta_address in fn['refs']:
                proof = max(proof, 1)
        if meta_address is None or proof == 0:
            continue
        proven.append({'vtable': vt, 'entries': entries,
                       'metaclass': meta_address, 'meta_ctor': meta_ctor,
                       'instance_size': size, 'get_meta_class': getter,
                       'proof': proof})
    require(proven, f'{classname}: every vtable candidate was rejected as ambiguous')
    best_proof = max(p['proof'] for p in proven)
    best_set = [p for p in proven if p['proof'] == best_proof]
    require(len(best_set) == 1,
            f'{classname}: {len(best_set)} equally-proven candidates '
            f'(metaclasses {[hex(p["metaclass"]) for p in best_set]})')
    best = best_set[0]
    require(best['metaclass'] is not None,
            f'{classname}: slot-7 getter resolves no metaclass; ABI pending')
    require(best['instance_size'],
            f'{classname}: no registration call proves the instance size')
    vtable, entries = best['vtable'], best['entries']
    meta_address = best['metaclass']

    # Instance constructors: functions in the metaclass object's pointer
    # window whose code references the vtable address point.
    ap = vtable + 0x10
    ctors = []
    for off in range(0, 0x200, 8):
        site = meta_address + off
        if site in fix and fix[site][1]:
            target = fix[site][0]
            if _in_exec(collection, member, target):
                fn = decode_function(member, target, 64)
                if ap in fn['refs'] or vtable in fn['refs']:
                    ctors.append(target)
    reviewed = RECORDS / 'constructor-bindings.json'
    if reviewed.is_file():
        review = json.loads(reviewed.read_text())
        record = review.get('classes', {}).get(classname)
        if record:
            require(review['kernelcache_sha256'] == collection.sha256,
                    f'{classname}: constructor collection identity changed')
            require(record['status'] == 'verified' and record['bundle'] == bundle
                    and int(record['vtable'], 0) == vtable
                    and record['instance_size'] == best['instance_size'],
                    f'{classname}: constructor class identity changed')
            address = int(record['address'], 0)
            body = bytes.fromhex(record['bytes'])
            require(address in member.function_starts()
                    and member.read(address, len(body)) == body
                    and hashlib.sha256(body).hexdigest() == record['sha256'],
                    f'{classname}: reviewed constructor instructions changed')
            ctors = [address]
    return {'class': classname, 'bundle': bundle, 'vtable': vtable,
            'slots': len(entries), 'entries': entries,
            'metaclass': meta_address, 'metaclass_ctor': best['meta_ctor'],
            'instance_size': best['instance_size'],
            'get_meta_class': best['get_meta_class'],
            'constructors': ctors,
            'destructors': [entries[i]['target'] for i in (0, 1)]}


# ---------------------------------------------------------------------------
# Cross-cache function shape matching: bind an old profile address to the
# same original function in the 24B119 member via relocation-masked shapes.
# ---------------------------------------------------------------------------


def function_shape(kc, member, start, limit=4096):
    code = member.read(start, min(limit, max(s['va'] + s['size'] - start
                     for s in member.segments if s['va'] <= start)))
    _, _, shape = kc.scan(code, start)
    return shape


def match_shape(kc, member, shape):
    """Unique function start in member whose shape equals `shape`."""
    hits = []
    for lo, hi in collection_exec(member):
        code = member.read(lo, hi - lo)
        for start in member.function_starts():
            if not lo <= start < hi:
                continue
            end = start + min(4096, hi - start)
            _, _, candidate = kc.scan(member.read(start, end - start), start)
            if candidate == shape:
                hits.append(start)
    return hits


def collection_exec(member):
    return [(s['va'], s['va'] + s['size']) for s in member.segments
            if s['name'] in ('__TEXT_EXEC', '__TEXT') and s['size']]


def transfer_binding(kc, old_collection, old_member_name, old_va, new_collection,
                     new_member_name):
    """Resolve old_va's function in the old member, shape-match it in the new
    member, return the new function start or None."""
    old_member = old_collection.member(old_member_name)
    new_member = new_collection.member(new_member_name)
    if old_member is None or new_member is None:
        return None
    start = old_collection.containing_function(old_member, old_va)
    if start is None:
        return None
    shape = function_shape(kc, old_member, start)
    hits = match_shape(kc, new_member, shape)
    if len(hits) == 1:
        return hits[0]
    return None


# ---------------------------------------------------------------------------
# Per-class semantic specifications. `transfer` semantics resolve by matching
# the prior profile slot diversity inside the 24B119 vtable; `realname`
# semantics hash the real method's mangle; `caller` semantics carry the
# diversity original typed call sites authenticate with.
# ---------------------------------------------------------------------------

PRIOR_WATCHDOG_DIVERSITIES = {  # 22H384 own slots 168..171 (reviewed)
    'enable': 0xe2f7, 'disable': 0x59b4, 'pet': 0x7d00, 'properties': 0xb78a}
PRIOR_USB_SLOTS = {  # usb-xdci-22G100.json macros -> prior slot index
    'XDCI_KEEP_CONFIGURATIONS': 179, 'XDCI_CABLE_CONNECT': 204,
    'XDCI_CABLE_DISCONNECT': 205, 'XDCI_TRANSPORT_ACTIVE': 260,
    'XDCI_POWER_ON': 261, 'XDCI_POWER_OFF': 262, 'XDCI_PARAMETERS': 263,
    'XDCI_CONTROLLER_RESET': 268, 'XDCI_PERIODIC_USAGE': 270,
    'XDCI_PERIODIC_SET': 271}
PMGR_REAL_NAMES = {  # ApplePMGR methods named in the staged source
    # label: (real method name, param-mangle, returns, params)
    'enableCPUCore64': ('enableCPUCore', 'jm', 'void', 'unsigned, uint64_t'),
    'enableCPUCore': ('enableCPUCore', 'j', 'void', 'unsigned'),
    'disableCPUCore': ('disableCPUCore', 'j', 'void', 'unsigned'),
    'enableCPUCluster': ('enableCPUCluster', 'j', 'void', 'unsigned'),
    'disableCPUCluster': ('disableCPUCluster', 'j', 'void', 'unsigned'),
    'initCPUIdle': ('initCPUIdle', 'P19ml_processor_info_t', 'void',
                    'ml_processor_info_t *'),
    'enterCPUIdle': ('enterCPUIdle', 'Py', 'void', 'UInt64 *'),
    'exitCPUIdle': ('exitCPUIdle', 'Py', 'void', 'UInt64 *'),
    'updateCPUIdle': ('updateCPUIdle', 'Py', 'void', 'UInt64 *'),
    'readAon': ('readAon', 'v', 'uint32_t', ''),
    'writeAon': ('writeAon', 'y', 'void', 'uint64_t')}
HID_REAL_NAMES = {  # IOHIDDevice methods the sources override
    # semantic: (param-mangle, returns, params, const)
    'newTransportString': ('v', 'OSString *', '', True),
    'newProductString': ('v', 'OSString *', '', True),
    'newReportDescriptor': ('PP17IOMemoryDescriptor', 'IOReturn',
                            'IOMemoryDescriptor **', True),
    'handleReport': (('P17IOMemoryDescriptor15IOHIDReportTypem',
                      'P17IOMemoryDescriptor15IOHIDReportTypej',
                      'P17IOMemoryDescriptorii'),
                     'IOReturn',
                     'IOMemoryDescriptor *, IOHIDReportType, IOOptionBits', False)}


def prior_slots(profile_path):
    d = json.loads(Path(profile_path).read_text())
    return d


def build_semantics(classname, collection, derived):
    """Resolve the semantic slot map for `classname` against its 24B119 vtable.

    Returns {semantic_or_macro: {'slot': n, 'diversity': '0x..', 'returns':..,
    'parameters':..}} plus a 'names' map {slot: emitted C++ decl info}.
    """
    entries = derived['entries']
    out = {}
    div_to_slot = {}
    for i, e in enumerate(entries):
        div_to_slot.setdefault(e['diversity'], []).append(i)

    def by_diversity(want, evidence):
        hits = div_to_slot.get(want, [])
        require(len(hits) == 1,
                f'{classname}: diversity {want:#x} matches {len(hits)} slots ({evidence})')
        return hits[0]

    if classname == 'IOWatchdog':
        for semantic, want in PRIOR_WATCHDOG_DIVERSITIES.items():
            out[semantic] = {'slot': by_diversity(want, f'watchdog {semantic}'),
                             'returns': 'void', 'parameters': ''}
    elif classname == 'AppleUSBXDCI':
        prior = prior_slots(ROOT / 'profiles/usb-xdci-22G100.json')
        for macro, old_slot in PRIOR_USB_SLOTS.items():
            want = int(prior['slots'][old_slot][1], 16)
            method = prior['methods'].get(str(old_slot), {})
            out[macro] = {'slot': by_diversity(want, f'usb {macro}'),
                          'returns': method.get('returns', 'void'),
                          'parameters': method.get('parameters', ''),
                          'signature': 'Ej' if macro == 'XDCI_PERIODIC_SET' else 'Ev'}
    elif classname == 'IOHIDDevice':
        for real, spec in HID_REAL_NAMES.items():
            signatures, returns, params, const = spec
            if isinstance(signatures, str):
                signatures = (signatures,)
            for signature in signatures:
                want = discriminator(mangle('IOHIDDevice', real,
                                            'E' + signature, const))
                hits = div_to_slot.get(want, [])
                if hits:
                    out[real] = {'slot': hits[0], 'returns': returns,
                                 'parameters': params, 'const': const,
                                 'real_name': real,
                                 'signature': 'E' + signature}
                    break
    elif classname == 'IOMobileFramebufferLegacy':
        prior = prior_slots(ROOT / 'profiles/legacy-display-22H384.json')
        for semantic, old_slot in prior['methods'].items():
            if isinstance(old_slot, dict):
                old_slot = old_slot.get('slot')
            want = int(prior['slots'][old_slot][1], 16)
            hits = div_to_slot.get(want, [])
            caller = prior.get('caller_diversity', {}).get(str(old_slot))
            if not hits and caller and int(caller['diversity'], 16) == want:
                hits = div_to_slot.get(int(caller['vtable_diversity'], 16), [])
            if hits:
                out[semantic] = {'slot': hits[0], 'prior_slot': old_slot,
                                 'signature': prior['names'][semantic][1]}
        for slot_s, caller in prior.get('caller_diversity', {}).items():
            # The prior key is the OLD slot index; the new slot is found via the
            # slot's stored vtable diversity, while callers authenticate the
            # different declared-method disc.
            stored = int(caller.get('vtable_diversity', '0'), 16)
            hits = div_to_slot.get(stored, [])
            if hits:
                out[f'caller:{slot_s}'] = {'slot': hits[0],
                                           'caller_diversity':
                                               int(caller['diversity'], 16)}
    elif classname == 'ApplePMGR':
        for label, (real, sig, returns, params) in PMGR_REAL_NAMES.items():
            want = discriminator(mangle('ApplePMGR', real, 'E' + sig))
            hits = div_to_slot.get(want, [])
            if hits:
                out[label] = {'slot': hits[0], 'real_name': real,
                              'returns': returns, 'parameters': params}
    return out


# ---------------------------------------------------------------------------
# Bound-class registry. Each entry produces one Original*ABI.h.
# ---------------------------------------------------------------------------

BOUND_CLASSES = {
    'IOWatchdog': {
        'bundle': 'com.apple.driver.AppleARMWatchdogTimer',
        'base': 'IOService', 'header': 'OriginalWatchdogABI.h',
        'aliases': {  # source name -> semantic key
            'enable3234': 'enable', 'disable13331': 'disable',
            'pet65562': 'pet', 'properties34074': 'properties'},
        'overrides': [
            'void stop(IOService *) override;',
            'IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *,'
            ' IOUserClient **) override;'],
        'extra_bindings': {
            '_marble_original_watchdog_properties': '_ZN10IOWatchdog15properties34074Ev',
            '_marble_original_watchdog_initialize': {
                'address': 0xfffffff00834d930, 'bytes': 1204,
                'sha256': '6fa7940728582fd95d340406acf95e013d22804a6b384872cae0714d66af1d12'}},
        'defines': False},
    'IOHIDDevice': {
        'bundle': 'com.apple.iokit.IOHIDFamily',
        'base': 'IOService', 'header': 'OriginalHidDeviceABI.h'},
    'IOAESAccelerator': {
        'bundle': 'com.apple.iokit.IOCryptoAcceleratorFamily',
        'base': 'IOService', 'header': 'OriginalAESABI.h'},
    'IOMobileFramebufferLegacy': {
        'bundle': 'com.apple.iokit.IOMobileGraphicsFamily',
        'base': 'IOService', 'header': 'OriginalLegacyFramebufferABI.h',
        'template': 'legacy'},
    'IOMobileFramebuffer': {
        'bundle': 'com.apple.iokit.IOMobileGraphicsFamily',
        'base': 'IOService', 'header': 'OriginalMobileFramebufferABI.h',
        'alias_prefix': 'unclassified'},
    'AppleUSBXDCI': {
        'bundle': 'com.apple.driver.AppleUSBXDCI',
        'base': 'IOService', 'header': 'OriginalUsbXdciABI.h'},
    'IOAcceleratorES': {
        'bundle': 'com.apple.iokit.IOGPUFamily',
        'base': 'IOService', 'header': 'OriginalAcceleratorABI.h'},
    'IOGPU': {
        'bundle': 'com.apple.iokit.IOGPUFamily',
        'base': 'IOService', 'header': 'OriginalGPUProviderABI.h',
        'alias_prefix': 'abiSlot'},
    'ApplePMGR': {
        'bundle': 'com.apple.driver.ApplePMGR',
        'base': 'IOService', 'header': 'OriginalPMGRABI.h',
        'alias_prefix': 'opaque', 'guard_inc': 'GuardedPMGRSlots.inc',
        'guard_from': 177, 'extra_includes': ['IOKit/IOPMGR.h']},
    'IOService': {
        'bundle': 'com.apple.kernel',
        'base': 'OSMetaClassBase', 'header': 'OriginalServiceABI.h',
        'no_header': True,
        # Native XNU provides IOService's symbols; the profile exists as
        # base-slot evidence and never produces import bindings.
        'native': True},
}


LEGACY_DISPLAY_OFFSETS = {  # verified on the 24B119 IOMobileGraphics member:
    # getDisplayDimensions reads self+7152/+7156; the vsync client queue base
    # 6760+5*16 lands on 6840; remaining offsets follow the same-generation
    # 24A446 layout the offsets were cross-checked against.
    'current': 6720, 'head': 6728, 'tail': 6736, 'vsync_clients': 6840,
    'width': 7152, 'height': 7156, 'default_surface': 7000,
    'surface_root': 6976}
LEGACY_REQUEST_OFFSETS = {  # same-generation request layout
    'rect0': 17, 'rect1': 81, 'masks': 984, 'options': 996, 'dims': 1008,
    'surfaces': 2944, 'next': 3384, 'prev': 3392}
LEGACY_PROTOTYPES = {
    'isMainDisplay': 'bool isMainDisplay()',
    'getDotPitch': 'IOReturn getDotPitch(uint32_t *)',
    'supportedFrameInfo': ('IOReturn supportedFrameInfo(uint32_t,'
                           'uint32_t *,char *,uint32_t *)=0'),
    'getDisplayDimensions': ('void getDisplayDimensions(uint32_t *,'
                             'uint32_t *)'),
    'startHardware': 'bool startHardware()',
    'kickSwapQueue': 'void kickSwapQueue()',
    'completeCurrentSwap': 'void completeCurrentSwap()',
    'validateSwap': 'bool validateSwap(const OriginalLegacySwapRequest *)',
    'getRefreshAttributes': 'bool getRefreshAttributes(uint64_t *,bool)',
    'collectCompletionReports': ('IOReturn collectCompletionReports(uint32_t,'
                                 'void *,uint32_t *,uint32_t)=0'),
    'applyStateRequest': ('int applyStateRequest('
                          'OriginalLegacyStateRequest *,bool)'),
    'cancelSwapsAfterTimeout': 'bool cancelSwapsAfterTimeout()',
    'create_default_fb_surface': 'void create_default_fb_surface()'}


def emit_legacy_header(spec, derived, semantics, out_dir):
    """The IOMobileFramebufferLegacy header: opaque storage plus the real
    prototypes the owned display backend overrides, the swap-queue accessors
    the source drives, and the default-surface factory hook for iOS27's
    backboardd path."""
    entries = derived['entries']
    pure = {i for i, e in enumerate(entries) if e['pure']}
    D, R = LEGACY_DISPLAY_OFFSETS, LEGACY_REQUEST_OFFSETS
    caller_map = {v['slot']: v['caller_diversity'] for k, v in semantics.items()
                  if k.startswith('caller:')}
    prototype_slots = {}
    prototype_signatures = {}
    for name, v in semantics.items():
        if name.startswith('caller:') or 'slot' not in v:
            continue
        if name in LEGACY_PROTOTYPES:
            prototype_slots[v['slot']] = name
            prototype_signatures[v['slot']] = v['signature']
    leaves = sorted(pure - set(prototype_slots))
    renames, decls = {}, []
    for i in range(BASE_SLOTS, len(entries)):
        if i in prototype_slots:
            semantic = prototype_slots[i]
            diversity = caller_map.get(i, entries[i]['diversity'])
            name = find_disc_name('IOMobileFramebufferLegacy', semantic, i,
                                  diversity, prototype_signatures[i])
            require(name, f'legacy slot {i}: method discriminator name absent')
            renames[semantic] = name
            decls.append(f' virtual {LEGACY_PROTOTYPES[semantic]};')
            continue
        diversity = caller_map.get(i, entries[i]['diversity'])
        name = find_disc_name('IOMobileFramebufferLegacy', 'opaque', i, diversity)
        require(name, f'legacy slot {i}: no name for diversity {diversity:#x}')
        renames[f'opaque{i}'] = name
        if i in pure:
            decls.append(f' virtual __attribute__((noreturn)) void opaque{i}()=0;')
        else:
            decls.append(' virtual __attribute__((deprecated("Unclassified ABI:'
                         f' never call from source"))) void opaque{i}();')
    leaf_list = ' '.join(f'LEAF({i})' for i in leaves)
    rename_definitions = ''.join(f'#define {o} {n}\n'
                                 for o, n in sorted(renames.items()))
    header = f'''// Generated 24B119 arm64e ABI for the original IOMobileFramebufferLegacy.
// Own slots start at {BASE_SLOTS}; opaque names carry the diversity original
// callers authenticate with. Private fields stay opaque.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
extern "C" OSObject *native_legacy_default_surface_create(OSObject *,uint32_t,uint32_t,IOService *);
struct OriginalLegacySwapRequest;
struct OriginalLegacyStateRequest {{uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];}};
static constexpr unsigned kLegacyVsyncClients={D['vsync_clients']};
static constexpr unsigned kRequestRect0={R['rect0']},kRequestRect1={R['rect1']},kRequestMasks={R['masks']},kRequestOptions={R['options']},kRequestDims={R['dims']},kRequestSurfaces={R['surfaces']};
#define NATIVE_LEGACY_LEAVES(LEAF) {leaf_list}
#define NATIVE_LEGACY_DEFAULT_SURFACE 1
#define NATIVE_LEGACY_CANCEL_SWAPS 1
static constexpr unsigned kLegacyDefaultSurface={D['default_surface']};
{rename_definitions}class IOMobileFramebufferLegacy : public IOService {{
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[{derived['instance_size']}-sizeof(IOService)];
public:
 bool installDefaultSurface(uint32_t width,uint32_t height) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  OSObject *root=nullptr,*surface=nullptr;
  memcpy(&surface,self+kLegacyDefaultSurface,sizeof(surface));
  if(surface)return true;
  memcpy(&root,self+{D['surface_root']},sizeof(root));
  if(!root)return false;
  surface=native_legacy_default_surface_create(root,width,height,getProvider());
  if(!surface)return false;
  // Transfer the factory reference to the family; its stop path releases this field.
  memcpy(self+kLegacyDefaultSurface,&surface,sizeof(surface));
  return true;
 }}
 void installBootDimensions(uint32_t width,uint32_t height) {{
  memcpy(reinterpret_cast<unsigned char *>(this)+{D['width']},&width,4);memcpy(reinterpret_cast<unsigned char *>(this)+{D['height']},&height,4);
 }}
 const OriginalLegacySwapRequest *pendingSwapRequest() const {{
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+{D['head']},sizeof(request));return request;
 }}
 // Tail queue: head {D['head']}, tail pointer {D['tail']}, each request's next {R['next']} and previous-link {R['prev']}; the head moves
 // to the current-swap slot {D['current']} for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+{D['current']},8);memcpy(&head,self+{D['head']},8);memcpy(&tail,self+{D['tail']},8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+{R['next']},8);memcpy(&previous,bytes+{R['prev']},8);
  if(previous!=self+{D['head']})return false;
  if(next) {{
   const void *link=self+{D['head']};
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+{R['prev']},&link,8);
  }} else if(tail!=bytes+{R['next']})return false;
  memcpy(self+{D['current']},&request,8);
  if(next) {{memcpy(self+{D['head']},&next,8);}}
  else {{const void *empty=nullptr,*emptyTail=self+{D['head']};memcpy(self+{D['head']},&empty,8);memcpy(self+{D['tail']},&emptyTail,8);}}
  return true;
 }}
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
{chr(10).join(decls)}
}};
static_assert(sizeof(IOMobileFramebufferLegacy)=={derived['instance_size']},"Actual original instance extent");
'''
    path = out_dir / spec['header']
    path.write_text(header)
    return path, renames


def emit_header(classname, spec, derived, semantics, out_dir):
    if spec.get('template') == 'legacy':
        return emit_legacy_header(spec, derived, semantics, out_dir)
    """Generate Original*ABI.h for `classname` from its derived vtable.

    The class declares only its own slots (everything past the base's vtable):
    redeclaring inherited slots under fresh names would append duplicate
    virtuals and shift every own slot. Inherited methods the sources need
    (qualified calls, explicit overrides) emit real-signature override decls
    from the spec. Own-slot names carry the PAC diversity original callers
    authenticate with, so unoverridden subclasses bind the original impl.
    """
    entries = derived['entries']
    base_slots = spec.get('base_slots', BASE_SLOTS)
    alias_prefix = spec.get('alias_prefix')
    semantic_slots = {v['slot']: (k, v) for k, v in semantics.items()
                      if not k.startswith('caller:') and 'slot' in v}
    caller_map = {v['slot']: v['caller_diversity'] for k, v in semantics.items()
                  if k.startswith('caller:')}
    lines = [f'// Generated 24B119 arm64e ABI for the original {classname}.',
             f'// {len(entries)} slots; own virtuals start at {base_slots}.',
             '// Private fields stay opaque; virtual declaration names carry',
             '// the PAC diversity the original callers authenticate with.',
             '#pragma once', '#include <IOKit/IOService.h>']
    for inc in spec.get('extra_includes', []):
        lines.append(f'#include <{inc}>')
    lines += [f'class {classname} : public {spec["base"]} {{',
              f' OSDeclareAbstractStructors({classname})',
              f' unsigned char originalPrivate[{derived["instance_size"]}-sizeof({spec["base"]})];',
              'public:']
    lines += [f' {decl}' for decl in spec.get('overrides', ())]
    defines = {}
    for slot in range(base_slots, len(entries)):
        entry = entries[slot]
        diversity = caller_map.get(slot, entry['diversity'])
        semantic = semantic_slots.get(slot)
        if semantic and semantic[1].get('real_name'):
            info = semantic[1]
            name = info['real_name']
            const = bool(info.get('const'))
            sig = info.get('signature', 'Ev')
            want = discriminator(mangle(classname, name, sig, const))
            require(want == diversity,
                    f'{classname} slot {slot}: real name {name} disc {want:#x}'
                    f' != expected {diversity:#x}')
            ret = info.get('returns', 'void')
            params = info.get('parameters', '')
            lines.append(f" virtual {ret} {name}({params}){' const' if const else ''};")
            continue
        if semantic and not semantic[0].startswith('caller:'):
            label = semantic[0]
        elif alias_prefix:
            label = alias_prefix
        else:
            label = 'opaque'
        signature = semantic[1].get('signature', 'Ev') if semantic else 'Ev'
        name = find_disc_name(classname, label, slot, diversity, signature)
        require(name, f'{classname} slot {slot}: no name under 4M for diversity {diversity:#x}')
        if semantic:
            info = semantic[1]
            ret = info.get('returns', 'void')
            params = info.get('parameters', '')
            decl = f' virtual {ret} {name}({params});'
            source_name = next((a for a, sem in spec.get('aliases', {}).items()
                                if sem == semantic[0]), semantic[0])
            if source_name != name:
                defines[source_name] = name
        else:
            decl = f' virtual void {name}();'
            if alias_prefix:
                defines[f'{alias_prefix}{slot}'] = name
        lines.append(decl)
    lines.append('};')
    if classname == 'IOAcceleratorES':
        lines.append('#define NATIVE_ACCELERATOR_BASE IOAcceleratorES')
    for alias, name in sorted(defines.items()):
        lines.append(f'#define {alias} {name}')
    lines.append(f'static_assert(sizeof({classname}) == '
                 f'{derived["instance_size"]}, "24B119 {classname} instance size");')
    path = out_dir / spec['header']
    path.write_text('\n'.join(lines) + '\n')
    # Guarded slot overrides for probe subclasses that panic on unimplemented
    # original operations (ShengPlatformPower's GuardedPMGRSlots.inc).
    if spec.get('guard_inc'):
        semantic = {v['slot'] for k, v in semantics.items()
                    if not k.startswith('caller:') and 'slot' in v}
        guards = ['// Generated unavailable() guards for unimplemented slots.']
        for slot in range(spec.get('guard_from', 0), len(entries)):
            if slot not in semantic:
                guards.append(f' void {alias_prefix}{slot}() override '
                              f'{{ unavailable({slot}); }}')
        (out_dir / spec['guard_inc']).write_text('\n'.join(guards) + '\n')
    return path, defines


# ---------------------------------------------------------------------------
# Derived ABI profiles written under profiles/abi/ios27-24B119/.
# ---------------------------------------------------------------------------


def write_profile(classname, spec, derived, semantics, out_dir=ABI_PROFILES):
    profile = {'class': classname, 'bundle': derived['bundle'],
               'build': TARGET['build'], 'xnu': TARGET['xnu'],
               'kernelcache_sha256': derived.get('sha256'),
               'vtable': f"0x{derived['vtable']:x}",
               'metaclass': f"0x{derived['metaclass']:x}" if derived['metaclass'] else None,
               'metaclass_ctor': f"0x{derived['metaclass_ctor']:x}",
               'instance_size': derived['instance_size'],
               'slots': [[f"0x{e['target']:x}", f"0x{e['diversity']:x}",
                          e['key'], e['address_diversity']] for e in derived['entries']],
               'constructors': [f'0x{a:x}' for a in derived['constructors']],
               'destructors': [f'0x{a:x}' for a in derived['destructors']],
               'get_meta_class': (f"0x{derived['get_meta_class']:x}"
                                  if derived['get_meta_class'] else None),
               'semantics': {k: v for k, v in semantics.items()}}
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f'{classname.lower()}.json'
    with tempfile.NamedTemporaryFile(mode='w', dir=out_dir,
                                     prefix=path.name + '.', delete=False) as stream:
        stream.write(json.dumps(profile, indent=2) + '\n')
        temporary = Path(stream.name)
    temporary.replace(path)
    return path


# ---------------------------------------------------------------------------
# Compile against the tree's compile records.
# ---------------------------------------------------------------------------


def compile_record(tree, suffix, source=None):
    name = ('osfmk/DEVELOPMENT/startup.o.json' if source is not None and source.name=='MarbleCpuPowerProfile.c'
            else C_RECORD if suffix == '.c' else CXX_RECORD)
    release_name = name.replace('/DEVELOPMENT/', '/RELEASE/')
    release = (Path(tree) / 'build/xnu/ios-release-obj/RELEASE_ARM64_BCM2837'
               / release_name)
    if release.is_file():
        return json.loads(release.read_text().strip().rstrip(','))
    record = Path(tree) / OBJ_ROOT / name
    require(record.exists(), f'compile record missing: {record}')
    return json.loads(record.read_text().strip().rstrip(','))


def compile_source(record, source, obj, extra_include=None, warn_error=True,
                   extra_args=()):
    command, i = [], 0
    arguments = record['arguments']
    while i < len(arguments):
        arg = arguments[i]
        if arg == record['file']:
            command.append(str(source))
        elif arg == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif arg == '-Xclang' and i + 1 < len(arguments) and arguments[i + 1].startswith('-W'):
            i += 1
        elif not arg.startswith('-W'):
            command.append(str(arg))
        i += 1
    # Driver ABI audits inspect machine relocations and authenticated slots.
    # Keep RELEASE configuration while emitting inspectable Mach-O objects.
    command += ['-fno-lto', '-DMARBLE_IOS18=1', '-DMARBLE_IOS18_GPU=1',
                '-DMARBLE_IOS27=1', '-DMARBLE_IOS27_GPU=1']
    command += ['-Wall', '-Wextra', '-Wno-unused-parameter',
                '-Wno-incompatible-sysroot', '-Wno-c99-designator',
                '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier',
                '-Wno-deprecated-volatile',
                '-Wno-deprecated-anon-enum-enum-conversion',
                '-Wno-unneeded-internal-declaration',
                '-Wno-unused-const-variable',
                '-Wno-unused-function',
                '-Wno-macro-redefined',
                '-Wno-missing-field-initializers',
                '-Wno-implicit-function-declaration',
                '-Wno-error=deprecated-declarations']
    if warn_error:
        command.append('-Werror')
    for inc in extra_include or []:
        command += ['-I', str(inc)]
    # Private headers not staged under EXPORT_HDRS (kern/ast.h and friends).
    seen = set()
    for arg in arguments:
        arg = str(arg)
        if 'xnu-work/' not in arg:
            continue
        work = Path(arg[:arg.index('xnu-work/') + len('xnu-work/')]
                    + arg.split('xnu-work/', 1)[1].split('/')[0])
        if work in seen or not work.is_dir():
            continue
        seen.add(work)
        for sub in ('osfmk', 'bsd', 'iokit', 'libkern', 'pexpert', 'libsa',
                    'EXTERNAL_HEADERS'):
            if (work / sub).is_dir():
                command += ['-I', str(work / sub)]
    command += [str(a) for a in extra_args]
    obj.parent.mkdir(parents=True, exist_ok=True)
    obj.with_suffix('.command.json').write_text(json.dumps({
        'directory': record['directory'], 'arguments': command,
        'source': str(source),
        'source_sha256': hashlib.sha256(Path(source).read_bytes()).hexdigest(),
    }, indent=2) + '\n')
    subprocess.check_output([str(v) for v in command], cwd=record['directory'],
                            timeout=300, stderr=subprocess.STDOUT)


# ---------------------------------------------------------------------------
# Mach-O object inspection and binding.
# ---------------------------------------------------------------------------


def macho_obj(blob, at=0):
    """Same shape as native_prelink.macho but accepts arm64e (subtype 2) and
    arm64 object files."""
    magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', blob, at)
    if magic != 0xfeedfacf or cpu != 0x100000c or subtype & 0xffffff not in (0, 2):
        raise ValueError('Only little-endian ARM64/arm64e is supported')
    end = at + 32 + size
    offset = at + 32
    commands, segments = [], []
    for _ in range(count):
        command, length = struct.unpack_from('<II', blob, offset)
        commands.append((command, offset, length))
        if command == 0x19:
            name, va, size_v, fileoff, filesize, maxprot, initprot, nsects, flags = \
                struct.unpack_from('<16s4Q4I', blob, offset + 8)
            seg = {'name': name.rstrip(b'\0').decode(), 'va': va, 'size': size_v,
                   'fileoff': fileoff, 'filesize': filesize,
                   'command_offset': offset, 'maxprot': maxprot,
                   'initprot': initprot, 'sections': []}
            for index in range(nsects):
                sa = offset + 72 + index * 80
                sname, _, address, extent, section_offset = \
                    struct.unpack_from('<16s16sQQI', blob, sa)
                seg['sections'].append(
                    {'name': sname.rstrip(b'\0').decode(), 'va': address,
                     'size': extent, 'fileoff': section_offset,
                     'command_offset': sa})
            segments.append(seg)
        offset += length
    return {'kind': kind, 'segments': segments, 'commands': commands}


def object_symbols(blob, info):
    sym = next((at for cmd, at, _ in info['commands'] if cmd == 2), None)
    if sym is None:
        return []
    at, count, strings, _ = struct.unpack_from('<4I', blob, sym + 8)
    out = []
    for i in range(count):
        ix, kind, sect, _, address = struct.unpack_from('<IBBHQ', blob, at + i * 16)
        end = blob.index(b'\0', strings + ix)
        out.append({'name': blob[strings + ix:end].decode(), 'kind': kind,
                    'section': sect, 'address': address})
    return out


def object_auth_relocations(blob, info, symbols):
    """{site_va: {name, key, diversity, address_diversity}} for
    ARM64_RELOC_AUTHENTICATED_POINTER relocations (r_type=11, r_extern=1,
    r_length=3, r_pcrel=0) whose authored content word has the sign bit set."""
    rel = {}
    for segment in info['segments']:
        for section in segment['sections']:
            at, count = struct.unpack_from('<2I', blob, section['command_offset'] + 56)
            if not count:
                continue
            for i in range(count):
                offset, bits = struct.unpack_from('<2I', blob, at + i * 8)
                if not (bits >> 27 & 1) or bits >> 28 != 11 \
                        or bits >> 25 & 3 != 3 or bits >> 24 & 1:
                    continue
                site = section['va'] + offset
                word, = struct.unpack_from('<Q', blob, section['fileoff'] + offset)
                if not word >> 63:
                    continue
                rel[site] = {'name': symbols[bits & 0xffffff]['name'],
                             'key': (word >> 49) & 3,
                             'address_diversity': (word >> 48) & 1,
                             'diversity': (word >> 32) & 0xffff}
    return rel


def owned_vtables(blob, info, symbols, tags):
    """vtable symbol -> ordered entry relocations for owned subclasses."""
    auth = object_auth_relocations(blob, info, symbols)
    defined = {s['name']: s['address'] for s in symbols
               if s['kind'] & 0xe0 == 0 and s['kind'] & 0xe == 0xe}
    tables = {}
    for name, va in defined.items():
        if not name.startswith('__ZTV'):
            continue
        if tags and not any(tag in name for tag in tags):
            continue
        entries = []
        site = va + 16
        while site in auth:
            entries.append((site, auth[site]))
            site += 8
        tables[name] = entries
    return tables


def validate_vtable(entries, slots, caller_map, objname):
    """Each owned vtable entry must carry IA key + address diversity and the
    diversity original callers authenticate with."""
    for i, (site, rel) in enumerate(entries):
        expected = caller_map.get(i, slots[i][1] if isinstance(slots[i], (list, tuple))
                                  else slots[i].get('diversity'))
        require(rel['key'] == 0, f'{objname} slot {i}: non-IA key {rel["key"]}')
        require(rel['address_diversity'] == 1,
                f'{objname} slot {i}: missing address diversity')
        require(rel['diversity'] == expected,
                f'{objname} slot {i}: diversity {rel["diversity"]:#x} != expected {expected:#x}')


# ---------------------------------------------------------------------------
# Binding emission: absolute .set for data symbols, BTI stubs for functions.
# ---------------------------------------------------------------------------


def is_data_symbol(symbol):
    """Mach-O symbols bound as absolute values, not branch stubs: vtables,
    typeinfo and gMetaClass objects are data; methods are code."""
    return symbol.startswith(('__ZTV', '__ZTI', '__ZTS', '__ZTT')) \
        or symbol.endswith('gMetaClassE')


def emit_bindings(bindings, out_dir, name='original-bindings'):
    data_lines, code_lines = [], []
    for symbol, va in sorted(bindings.items()):
        if is_data_symbol(symbol):
            data_lines.append(f'.globl {symbol}\n.set {symbol}, {va:#x}')
        else:
            code_lines += [
                f'\t.globl {symbol}', '\t.p2align 2', f'{symbol}:',
                '\thint #34',  # bti c: land here for indirect original calls
                f'\tmovz x16, #0x{va & 0xffff:x}',
                f'\tmovk x16, #0x{(va >> 16) & 0xffff:x}, lsl #16',
                f'\tmovk x16, #0x{(va >> 32) & 0xffff:x}, lsl #32',
                f'\tmovk x16, #0x{(va >> 48) & 0xffff:x}, lsl #48',
                '\tbr x16']
    emitted = []
    if data_lines:
        p = out_dir / f'{name}-data.S'
        p.write_text('\n'.join(data_lines) + '\n')
        emitted.append(p)
    if code_lines:
        p = out_dir / f'{name}-code.S'
        p.write_text('\n'.join(code_lines) + '\n')
        emitted.append(p)
    return emitted


def assemble(source, obj):
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'],
                                  text=True).strip()
    subprocess.check_call(['xcrun', 'clang', '-target', 'arm64e-apple-ios27.2',
                           '-isysroot', sdk, '-c', str(source), '-o', str(obj)])


OBJCOPY = next((p for p in ('/opt/homebrew/opt/llvm/bin/llvm-objcopy',
                            shutil.which('llvm-objcopy')) if p and Path(p).exists()),
               None)


def late_object(obj, out, source_name):
    """Remove __mod_init_func and globalize the TU's static-init symbol so
    registration ordering is controlled by the parent."""
    init = f'__GLOBAL__sub_I_{source_name}'
    tool = OBJCOPY or 'xcrun'
    args = [tool] + ([] if OBJCOPY else ['llvm-objcopy']) + [
        '--remove-section=__DATA,__mod_init_func',
        f'--globalize-symbol={init}', str(obj), str(out)]
    subprocess.check_call(args, stderr=subprocess.DEVNULL)
    return out


# ---------------------------------------------------------------------------
# Derivation driver: build all bound-class profiles.
# ---------------------------------------------------------------------------


def derive_abi(kernelcache, out_dir=ABI_PROFILES):
    """Derive every bound original class from the 24B119 collection."""
    collection = Collection(kernelcache)
    report = {}
    derived_all = {}
    for classname, spec in BOUND_CLASSES.items():
        try:
            derived = derive_class(collection, spec['bundle'], classname)
            derived['sha256'] = collection.sha256
            semantics = build_semantics(classname, collection, derived)
            derived_all[classname] = (spec, derived, semantics)
            report[classname] = {'status': 'derived', 'slots': derived['slots'],
                                 'vtable': f"0x{derived['vtable']:x}",
                                 'semantics': len(semantics)}
        except SystemExit as e:
            report[classname] = {'status': 'pending', 'evidence': str(e)}
    # __cxa_pure_virtual: the single most common function target across the
    # derived vtables' entries (every abstract slot points at it).
    frequency = {}
    for _, derived, _ in derived_all.values():
        for e in derived['entries']:
            frequency[e['target']] = frequency.get(e['target'], 0) + 1
    if frequency:
        derived_all['__cxa_pure_virtual'] = max(frequency, key=frequency.get)
        pure_target = derived_all['__cxa_pure_virtual']
        for item in derived_all.values():
            if isinstance(item, tuple):
                for e in item[1]['entries']:
                    e['pure'] = e['target'] == pure_target
    for classname, item in list(derived_all.items()):
        if isinstance(item, tuple):
            spec, derived, semantics = item
            write_profile(classname, spec, derived, semantics, out_dir)
    return report, derived_all


# ---------------------------------------------------------------------------
# Source manifests and family builds.
# ---------------------------------------------------------------------------


def find_source(name, tree):
    for base in (Path(tree) / 'kits/marble', ROOT / 'package'):
        cand = base / name
        if cand.exists():
            return cand
    drivers = Path(tree) / 'drivers'
    if drivers.is_dir():
        for comp in sorted(drivers.iterdir()):
            cand = comp / name
            if cand.exists():
                return cand
    return None


BOOT_SOURCES = [
    'MarbleBootWatchdog.cpp', 'MarblePmgr.cpp', 'MarbleRtc.cpp', 'MarbleCpu.cpp', 'MarbleCpuPowerProfile.c',
    'MarbleSystemOS.c', 'MarbleMetal.c', 'MarbleUiPolicy.c', 'MarbleSetupPolicy.c',
    'MarbleAppPublication.c', 'RootWatchdogPolicy.c', 'MarbleDisplay.cpp',
    'MarbleScanout.cpp', 'MarbleNativePanel.cpp', 'MarblePanel.cpp',
    'MarbleBacklight.cpp', 'MarbleButtons.cpp', 'MarbleTouch.cpp',
    'MarbleHid18.cpp', 'NativeHomeInput.cpp', 'NativeResident.cpp',
    'RuntimeUsb.cpp', 'MarbleUsb.cpp',
    'KeyStoreEvents.cpp', 'MarbleBattery.cpp', 'MarbleBatteryProbe.cpp',
    'MarbleCharging.cpp', 'MarbleAdsp.cpp', 'MarbleAudio.cpp',
    'MarbleHaptics.cpp', 'MarbleHapticsRuntime.cpp',
    'MarbleRuntime18.cpp',
    'MarbleQrtr.c', 'QcomGlink.c', 'QcomMdt.c', 'QcomQmp.c', 'QcomQrtr.c',
    'QcomRpmh.c', 'QcomSmem.c', 'QcomSmp2p.c', 'GeniI2c.c', 'MarbleBattmgr.c',
    'MarbleWifi.cpp', 'MarbleWifiNet.cpp', 'MarbleWifiRuntime.cpp',
    'MarbleWifiFirmware.c',
]
STORAGE_SOURCES = ['MarbleRootDisk.cpp', 'MarbleUfs.cpp', 'MarbleVirtualStorage.cpp']
GPU_SOURCES = ['MarbleGpu.cpp', 'MarbleGmu.cpp', 'MarbleAdreno.cpp',
               'MarbleZap.cpp']

# Explicit component sources, mirroring the ios18 family manifests.
BOOT_COMPONENT_SOURCES = [
    'drivers/SoftwareAES/NativeAES.cpp',
    'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp',
    'drivers/GopFramebuffer/NativeLegacyDisplay.cpp',
    'drivers/QcomGeniSpi/GeniSpi.c',
    'kits/marble/audio/ShengGpr.c',
    'kits/marble/audio/ShengAudioReach.c',
]
# MarbleUfs owns the SM7475 transport and includes the merged ShengUfs
# RootQueue, RootCommands and RootLayout implementations, as in iOS18.
STORAGE_COMPONENT_SOURCES = []
GPU_COMPONENT_SOURCES = [
    'drivers/GopFramebuffer/NativeGOPPresenter.cpp',
    # drivers/Adreno740, matching the ios18 kernel GPU list plus the 27 Pool
    # and the staged accelerator TU. The sheng/generic service paths
    # (NativeAdrenoService, NativeAdrenoSmmuService, NativeAdrenoGmuService,
    # probes and tests) are intentionally absent.
] + [f'drivers/Adreno740/NativeAdreno{name}.cpp' for name in
     ('Memory', 'Cpu', 'SmmuMemory', 'SmmuContext', 'Vm', 'GpuContext',
      'Ring', 'CpInit', 'Pwrup', 'Preempt', 'Submit', 'UserClient',
      'Publish', 'Timer', 'KernelTime', 'Pool', 'Accelerator')]
GPU_KIT_PATTERNS = [('kits/ArmSmmu4K', 'ArmSmmu4K*.c')]
GPU_KIT_SOURCES = ['kits/AdrenoPM4/AdrenoPM4.c',
                   'kits/AdrenoFirmware/AdrenoFirmware.c',
                   'kits/AdrenoFirmware/AdrenoHfi.c',
                   'kits/AdrenoFirmware/AdrenoHfiVotes.c']
# Sources staged into the family generated dir so the 24B119 ABI headers win
# over stale same-directory Original*ABI.h copies.
ABI_STAGED_SOURCES = {'NativeAdrenoAccelerator.cpp'}

FAMILIES = {'marble-boot': BOOT_SOURCES, 'marble-storage': STORAGE_SOURCES,
            'marble-gpu': GPU_SOURCES}
FAMILY_COMPONENTS = {'marble-boot': BOOT_COMPONENT_SOURCES,
                     'marble-storage': STORAGE_COMPONENT_SOURCES,
                     'marble-gpu': GPU_COMPONENT_SOURCES}
FAMILY_INCLUDE_DIRS = {
    'marble-boot': ['drivers/SoftwareAES', 'drivers/AppleKeyStoreState',
                    'drivers/GopFramebuffer', 'drivers/QcomGeniSpi',
                    'drivers/NativeGeniSpi', 'drivers/Ath12kWifi',
                    'drivers/Ath12kWifi/include',
                    'drivers/ShengUfs', 'drivers/NativeUfs', 'drivers/RamBlock'],
    'marble-storage': ['drivers/ShengUfs', 'drivers/RamBlock'],
    'marble-gpu': ['drivers/Adreno740', 'drivers/AdrenoMetal',
                   'drivers/GopFramebuffer', 'kits/ArmSmmu4K',
                   'kits/AdrenoPM4', 'kits/AdrenoFirmware'],
}
FAMILY_ARCHIVE = {'marble-boot': 'libMarbleBoot.a',
                  'marble-storage': 'libMarbleStorage.a',
                  'marble-gpu': 'libMarbleGpu.a'}
# Prebuilt component object directories folded into archives.
WIFI_OBJECTS = (WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119/wifi/objects')
FAMILY_PREBUILT = {'marble-boot': [WIFI_OBJECTS], 'marble-storage': [], 'marble-gpu': []}
# Compiled subclass vtables audited against their qualified 24B119 profile:
# object stem -> [(profile class, vtable symbol)].
FAMILY_VTABLE_AUDIT = {
    'marble-boot': {
        'MarbleBootWatchdog': [('iowatchdog', '__ZTV14MarbleWatchdog')],
        'MarbleUsb': [('appleusbxdci', '__ZTV12QcomDwc3Xdci')],
        'NativeAES': [('ioaesaccelerator', '__ZTV9NativeAES')],
        'NativeHomeInput': [('iohiddevice', '__ZTV15NativeHomeInput'),
                            ('iohiddevice', '__ZTV22NativeMarbleTouchInput')],
        'NativeLegacyDisplay': [('iomobileframebufferlegacy',
                                 '__ZTV19NativeLegacyDisplay')]},
    'marble-gpu': {
        'NativeAdrenoAccelerator': [('ioacceleratores', '__ZTV14AGXAccelerator')]},
    'marble-storage': {}}
# Owned adapter sources compiled into each family.
FAMILY_ADAPTERS = {'marble-boot': [], 'marble-storage': [],
                   'marble-gpu': []}
# Per-source extra compiler arguments (forced includes, defines).
SOURCE_FLAGS = {
    'NativeUfs.cpp': ['-include', str(ROOT / 'package/ios27/UfsTicksShim.h')],
    # iOS27 keeps the Darwin26 AES ABI: getAlgorithmName lives on
    # IOCryptoAccelerator (PAC 0x6497), not on IOAESAccelerator itself.
    'NativeAES.cpp': ['-DNATIVE_AES_DARWIN26=1'],
    # iOS27 renders through backboardd's default surface, not macOS
    # CoreDisplay/WindowServer; the IOAV helpers are an optional adapter.
    'NativeLegacyDisplay.cpp': ['-DNATIVE_LEGACY_MACOS=0',
                                '-DNATIVE_LEGACY_DEFAULT_SURFACE=1']}

# Original-class single inheritance, for qualified-call import fallback:
# `__ZN<cls>M<p>` imports whose own-name diversity hits no slot retry as
# `__ZN<ancestor>M<p>` — the root declaration the slot authenticates with.
CLASS_ANCESTRY = {'IOWatchdog': ('IOService',), 'IOHIDDevice': ('IOService',),
                  'IOAESAccelerator': ('IOService',),
                  'IOMobileFramebufferLegacy': ('IOService',),
                  'IOMobileFramebuffer': ('IOService',),
                  'AppleUSBXDCI': ('IOService',),
                  'IOAcceleratorES': ('IOService',), 'IOGPU': ('IOService',),
                  'ApplePMGR': ('IOService',),
                  'IOService': ('IORegistryEntry',),
                  'IORegistryEntry': ('OSObject',),
                  'OSObject': ('OSMetaClassBase',)}


def class_ancestors(classname):
    for ancestor in CLASS_ANCESTRY.get(classname, ()):
        yield ancestor
        yield from class_ancestors(ancestor)


def component_sources(tree, family):
    """Explicit component source list plus curated glob patterns."""
    out = []
    for rel in FAMILY_COMPONENTS[family]:
        path = Path(tree) / rel
        out.append(path)
    if family == 'marble-gpu':
        for directory, pattern in GPU_KIT_PATTERNS:
            root = Path(tree) / directory
            if root.is_dir():
                out += [p for p in sorted(root.glob(pattern))
                        if not p.name.startswith('test_')]
        for rel in GPU_KIT_SOURCES:
            path = Path(tree) / rel
            out.append(path)
    return list(dict.fromkeys(p.resolve() for p in out))


def stage_abi_source(source, generated):
    """Copy a TU whose include line reaches a stale same-directory
    Original*ABI.h into the generated dir so the 24B119 header wins."""
    text = Path(source).read_text()
    if 'ABI.h' not in text:
        return source
    generated.mkdir(parents=True, exist_ok=True)
    out = generated / source.name
    new = re.sub(r'#\s*include\s+["<]([^">]*/)?(Original\w*ABI\.h)[">]',
                 r'#include "\2"', text)
    out.write_text(new)
    return out


def stage_board_source(source, generated):
    if source.name not in ('MarbleAdsp.cpp', 'NativeResident.cpp'):
        return source
    text = source.read_text()
    if source.name == 'MarbleAdsp.cpp':
        marker = 'extern const uint8_t marble_adsp_firmware[];'
        require(text.count(marker) == 1, 'ADSP firmware pointer declaration changed')
        text = text.replace(marker, 'extern const uint8_t *marble_adsp_firmware;')
    else:
        # Match the retained firmware handoff used by build_ios18_arm64e.
        text = text.replace('  native_root_block_export_data();\n', '')
        start = text.index('  uint64_t start, now, frequency;')
        end = text.index('  // The resident firmware resumes', start)
        text = text[:start] + text[end:]
    generated.mkdir(parents=True, exist_ok=True)
    target = generated / source.name
    target.write_text(text)
    return target


def board_firmware(family, out):
    root = ROOT.parent / 'UniXNU/.cache/firmware/marble'
    entries = []
    if family == 'marble-boot':
        path = root / 'adsp2.mbn'
        require(path.is_file(), f'ADSP firmware missing: {path}')
        assembly = ('.section __DATA_CONST,__const\n.p2align 3\n'
                    '.globl _marble_adsp_firmware_size\n_marble_adsp_firmware_size:\n'
                    f'.quad {path.stat().st_size}\n')
        entries.append({'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    elif family == 'marble-gpu':
        assembly = '.section __DATA_CONST,__const\n'
        for name, filename, expected in [
            ('gmu', 'gmu_gen70000.bin', 'b2d3f0e2a98fd6259ab9ffef16df63265542e6cc9a970d53d476e823d432042a'),
            ('sqe', 'a730_sqe.fw', 'e67d1829f57fc8326d806234b932dc3b78e28a9940df34c70752f17d5f38b7aa'),
            ('zap', 'a730_zap.mbn', 'f03e732d07046c01250998d7f1ee2a2fae3e3ae28872320cdd38334e60e9a10e')]:
            path = root / filename
            require(path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == expected,
                    f'Marble GPU firmware identity differs: {path}')
            label = f'_marble_{name}_firmware'
            assembly += (f'.p2align 12\n.globl {label}\n{label}:\n.incbin "{path}"\n'
                         f'{label}_end:\n.p2align 3\n.globl {label}_size\n{label}_size:\n'
                         f'.quad {label}_end-{label}\n')
            entries.append({'path': str(path), 'sha256': expected})
    else:
        return [], []
    source = out / 'firmware.S'
    source.write_text(assembly)
    obj = out / 'obj/firmware.o'
    obj.parent.mkdir(parents=True, exist_ok=True)
    assemble(source, obj)
    return [obj], entries


def runtime_sources(tree):
    """Version-qualified runtime policy sources staged into the tree."""
    out = []
    for pattern_dir in (Path(tree) / 'kits/marble/ios27-runtime',
                        ROOT / 'package/ios27-runtime'):
        if pattern_dir.is_dir():
            out += sorted(pattern_dir.glob('*.c'))
            out += sorted(pattern_dir.glob('*.cpp'))
    return list(dict.fromkeys(p.resolve() for p in out))


def ioav_sources():
    """The butter-tanker IOAV adapter, when delivered."""
    pattern_dir = ROOT / 'package/ios27-ioav'
    if not pattern_dir.is_dir():
        return []
    return sorted(pattern_dir.glob('*.cpp')) + sorted(pattern_dir.glob('*.c'))


def has_mod_init(blob, info):
    for segment in info['segments']:
        for section in segment['sections']:
            if section['name'] == '__mod_init_func':
                return True
    return False


STORAGE_MARKER = 'OSDefineMetaClassAndAbstractStructors(IOBlockStorageDevice,IOService)'
STORAGE_REPLACE = '''
extern "C" const OSMetaClass *native_source_storage_parent(void);
OSDefineMetaClassWithInit(IOBlockStorageDevice, IOService, )
IOBlockStorageDevice::MetaClass::MetaClass()
    : OSMetaClass("UniXNUBlockStorageDevice", native_source_storage_parent(), sizeof(IOBlockStorageDevice)) {}
OSDefineAbstractStructors(IOBlockStorageDevice, IOService)
OSDefineOperatorMethods(IOBlockStorageDevice)
'''


def storage_reference(tree):
    """The IOStorageFamily reference directory for this tree."""
    for cand in (Path(tree).parent / 'references/IOStorageFamily',
                 Path(tree) / 'references/IOStorageFamily',
                 DEFAULT_TREE.parent / 'references/IOStorageFamily'):
        if (cand / 'IOBlockStorageDevice.cpp').exists():
            return cand
    return None


def stage_storage(tree, include, generated):
    """Stage IOKit/storage headers and the metaclass-patched base source."""
    ref = storage_reference(tree)
    require(ref is not None, 'IOStorageFamily reference source absent')
    storage = include / 'IOKit/storage'
    storage.mkdir(parents=True, exist_ok=True)
    for header in ref.glob('*.h'):
        shutil.copyfile(header, storage / header.name)
    sdk = Path(subprocess.check_output(
        ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    shutil.copyfile(sdk / 'usr/include/TargetConditionals.h',
                    include / 'TargetConditionals.h')
    original = (ref / 'IOBlockStorageDevice.cpp').read_text()
    require(original.count(STORAGE_MARKER) == 1,
            'original storage base registration macro changed')
    generated.mkdir(parents=True, exist_ok=True)
    base = generated / 'IOBlockStorageDevice.cpp'
    base.write_text(original.replace(STORAGE_MARKER, STORAGE_REPLACE))
    return base


def binding_name(classname):
    return f'__ZN{len(classname)}{classname}'


KERNEL_FRAMEWORK_HEADERS = ('/Applications/Xcode.app/Contents/Developer/'
                            'Platforms/MacOSX.platform/Developer/SDKs/'
                            'MacOSX.sdk/System/Library/Frameworks/'
                            'Kernel.framework/Versions/A/Headers')


def stage_hid(include):
    """Stage the real IOKit/hid + HIDDriverKit headers the arm64e sources take
    under __has_feature(ptrauth_calls), from the Kernel framework."""
    root = Path(KERNEL_FRAMEWORK_HEADERS)
    require(root.is_dir(), f'Kernel framework headers missing: {root}')
    sdk = Path(subprocess.check_output(
        ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    include.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(sdk / 'usr/include/TargetConditionals.h',
                    include / 'TargetConditionals.h')
    for rel in ('IOKit/hid', 'IOKit/hidsystem', 'IOKit/graphics', 'HIDDriverKit'):
        src = root / rel
        if not src.is_dir():
            continue
        dst = include / rel
        dst.mkdir(parents=True, exist_ok=True)
        for header in src.glob('*.h'):
            shutil.copyfile(header, dst / header.name)
    reference = DEFAULT_TREE.parent / 'references/IOHIDFamily'
    require(reference.is_dir(), f'Pinned IOHIDFamily source missing: {reference}')
    for source, relative in ((reference / 'IOHIDFamily', 'IOKit/hid'),
                             (reference / 'IOHIDSystem/IOKit/hidsystem',
                              'IOKit/hidsystem')):
        destination = include / relative
        destination.mkdir(parents=True, exist_ok=True)
        for header in source.glob('*.h'):
            shutil.copyfile(header, destination / header.name)


def qualify_source_class(obj, classname, bundle, collection):
    """Contract check: the compiled source class's authenticated vtable must
    equal an original collection table slot-for-slot, and the original
    registration must prove the class's instance size.

    Returns a report; exactly one matching original table is required."""
    blob = Path(obj).read_bytes()
    info = macho_obj(blob)
    symbols = object_symbols(blob, info)
    ztv = f'__ZTV{len(classname)}{classname}'
    entries = owned_vtables(blob, info, symbols, [classname]).get(ztv, [])
    require(entries, f'{classname}: no authenticated vtable in {obj}')
    sig = [(e['key'], e['address_diversity'], e['diversity']) for _, e in entries]
    fx = fixups(collection)
    member = collection.member(bundle)
    require(member is not None, f'{classname}: original member {bundle} absent')
    const = next((s for s in member.segments if s['name'] == '__DATA_CONST'), None)
    require(const, f'{classname}: {bundle} has no __DATA_CONST')
    matches = []
    for site in sorted(fx):
        if not const['va'] <= site < const['va'] + const['size']:
            continue
        # Exact table match: signature equal and no authenticated continuation.
        tail = fx.get(site + 8 * len(sig))
        if all(site + 8 * i in fx and fx[site + 8 * i][1]
               and (fx[site + 8 * i][2], fx[site + 8 * i][4], fx[site + 8 * i][3]) == s
               for i, s in enumerate(sig)) and not (tail and tail[1]):
            matches.append(site)
    # Signature-equal subclasses (a child overriding methods keeps the root
    # declaration's diversity) make len(matches) ambiguous; the correct table
    # is the one whose slot-7 metaclass is registered under `classname`.
    cstrings = collection.find_cstring(member, classname)
    refs = collection.functions_referencing(member, cstrings)
    facts = {start: decode_function(member, start) for start in refs}
    proven = {}
    for site in matches:
        vtable = vtable_entries(collection, site)
        getter = vtable[7]['target'] if len(vtable) > 7 else None
        meta = decode_function(member, getter, 32)['adds'].get(0) if getter else None
        size = None
        named = False
        for fn in facts.values():
            for _, args in fn['call_args']:
                if args.get(1) in cstrings and (args.get(3) or 0) > 0x20:
                    if args.get(0) == meta:
                        named, size = True, args[3]
        proven[site] = {'metaclass': meta, 'named': named, 'size': size,
                        'vtable': vtable}
    named = [s for s in matches if proven[s]['named']]
    if len(named) == 1:
        site = named[0]
    else:
        require(len(matches) == 1,
                f'{classname}: {len(matches)} original tables match the source '
                f'signature, {len(named)} name-registered')
        site = matches[0]
    pick = proven[site]
    table = site - 0x10  # address point -> vtable base
    vtable = pick['vtable']
    metaclass, size = pick['metaclass'], pick['size']
    if size is None:
        for fn in facts.values():
            for _, args in fn['call_args']:
                if args.get(1) in cstrings and (args.get(3) or 0) > 0x20:
                    size = args[3]
    return {'class': classname, 'bundle': bundle,
            'source_vtable_slots': len(sig),
            'original_table': f'0x{table:x}',
            'original_slots': len(vtable),
            'original_metaclass': f'0x{metaclass:x}' if metaclass else None,
            'original_instance_size': size,
            'instance_size_verified': size is not None,
            'status': 'verified' if metaclass and size else 'pending'}


def compiled_size(source, record, out, extra_include):
    """Compile a probe returning sizeof(<source class>) as a data symbol."""
    probe = out / 'sizeof-probe.cpp'
    probe.write_text(f'#include <IOKit/storage/IOBlockStorageDevice.h>\n'
                     f'extern "C" __attribute__((used)) const uint64_t '
                     f'unixnu_source_class_size = sizeof(IOBlockStorageDevice);\n')
    obj = out / 'sizeof-probe.o'
    compile_source(record, probe, obj, extra_include)
    blob = obj.read_bytes()
    info = macho_obj(blob)
    for s in object_symbols(blob, info):
        if s['name'] == '_unixnu_source_class_size':
            value, = struct.unpack_from('<Q', blob, _section_offset(info, s))
            return value
    raise SystemExit('sizeof probe symbol absent')


def _section_offset(info, symbol):
    sect = symbol['section']
    n = 0
    for segment in info['segments']:
        for section in segment['sections']:
            n += 1
            if n == sect:
                return section['fileoff'] + symbol['address'] - section['va']
    raise SystemExit('sizeof probe section absent')


def resolve_object_bindings(obj, derived_all, collection):
    """Undefined original imports of one object -> {symbol: collection VA}.

    - __ZN<cls>C2E.. / C1E..          -> instance constructor
    - __ZN<cls>D2Ev / D1Ev            -> nondeleting destructor (slot 0)
    - __ZN<cls>D0Ev                   -> deleting destructor (slot 1)
    - __ZN<cls>10gMetaClassE          -> metaclass object
    - __ZTV<cls>                      -> vtable base (__ZTV address)
    - __ZN<cls><method>E..            -> vtable slot target by diversity, or
                                       the own-name target when the symbol is
                                       a non-virtual original function.
    """
    _, np = import_xnuprelink()
    blob = Path(obj).read_bytes()
    info = macho_obj(blob)
    symbols = object_symbols(blob, info)
    undefined = {s['name'] for s in symbols if s['kind'] & 0x0f == 1}
    bindings, pending = {}, []
    # Slot bindings by diversity: for each bound class, map disc -> slot target.
    for classname, item in derived_all.items():
        if not isinstance(item, tuple) or not isinstance(item[1], dict):
            continue
        spec, derived, semantics = item
        if spec.get('native'):
            continue
        tag = binding_name(classname)
        div_map = {}
        # A private declaration belongs to this class. Its 16-bit PAC value
        # can collide with an inherited IOService method, so retain ownership.
        entries=derived['entries']
        for i in [*range(BASE_SLOTS,len(entries)),*range(min(BASE_SLOTS,len(entries)))]:
            div_map.setdefault(entries[i]['diversity'],i)
        for symbol in sorted(undefined):
            const_tag = f'__ZNK{len(classname)}{classname}'
            if not symbol.startswith((tag, const_tag)):
                continue
            const = symbol.startswith(const_tag)
            rest = symbol[len(const_tag if const else tag):]
            if rest.startswith(('C1E', 'C2E')):
                if derived['constructors']:
                    bindings[symbol] = derived['constructors'][0]
                else:
                    pending.append({'symbol': symbol, 'class': classname,
                                    'evidence': 'protected instance constructor requires target derivation'})
            elif rest == 'D1Ev' or rest == 'D2Ev':
                bindings[symbol] = derived['destructors'][0]
            elif rest == 'D0Ev':
                bindings[symbol] = derived['destructors'][1]
            elif rest == '10gMetaClassE' and derived['metaclass']:
                bindings[symbol] = derived['metaclass']
            elif rest.startswith('C') or rest.startswith('D'):
                pass
            else:
                disc = discriminator(symbol[1:])  # strip leading _
                opaque=re.match(r'\d+opaque(\d+)_\d+E',rest)
                slot=int(opaque[1]) if opaque else div_map.get(disc)
                if opaque and (slot>=len(entries) or entries[slot]['diversity']!=disc):
                    pending.append({'symbol':symbol,'class':classname,
                                    'evidence':'generated slot identity or PAC value changed'})
                    continue
                if slot is not None:
                    bindings[symbol] = derived['entries'][slot]['target']
                    continue
                # Qualified calls to an unoverridden method keep the subclass
                # name in the symbol but the root declaration's diversity in
                # the vtable: retry with each ancestor substituted.
                matched = False
                for ancestor in class_ancestors(classname):
                    # The enclosing class is substitution S_ in its parameter
                    # type: IOService::stop(IOService *) is ...4stopEPS_.
                    canonical = re.sub(r'(P[KVR]*)' + str(len(ancestor)) + ancestor,
                                       r'\1S_', rest, count=1)
                    alt = f'_ZN{"K" if const else ""}{len(ancestor)}{ancestor}{canonical}'
                    slot = div_map.get(discriminator(alt))
                    if slot is not None:
                        bindings[symbol] = derived['entries'][slot]['target']
                        matched = True
                        break
                if not matched:
                    pending.append({'symbol': symbol, 'class': classname,
                                    'evidence': 'no vtable slot carries its diversity'})
    # Analyst-named helper imports: root-decl mangle -> slot target.
    for classname, item in derived_all.items():
        if not isinstance(item, tuple):
            continue
        spec, derived, semantics = item
        if spec.get('native'):
            continue
        div_map = {}
        for i, e in enumerate(derived['entries']):
            div_map.setdefault(e['diversity'], i)
        for symbol, root_mangle in spec.get('extra_bindings', {}).items():
            if symbol not in undefined or symbol in bindings:
                continue
            if isinstance(root_mangle, dict):
                address = root_mangle['address']
                member = collection.member(spec['bundle'])
                code = member.read(address, root_mangle['bytes'])
                require(hashlib.sha256(code).hexdigest() == root_mangle['sha256'],
                        'Original lifecycle helper digest differs: ' + symbol)
                bindings[symbol] = address
                continue
            slot = div_map.get(discriminator(root_mangle))
            if slot is not None:
                bindings[symbol] = derived['entries'][slot]['target']
            else:
                pending.append({'symbol': symbol, 'class': classname,
                                'evidence': f'root {root_mangle} has no slot'})
    # gMetaClass and __ZTV absolute symbols.
    for classname, item in derived_all.items():
        if not isinstance(item, tuple):
            continue
        spec, derived, semantics = item
        if spec.get('native'):
            continue
        meta_sym = f'__ZN{len(classname)}{classname}10gMetaClassE'
        if meta_sym in undefined and derived['metaclass']:
            bindings[meta_sym] = derived['metaclass']
        ztv = f'__ZTV{len(classname)}{classname}'
        if ztv in undefined:
            bindings[ztv] = derived['vtable']
    # vtable fixup validation for owned subclasses.
    auth = object_auth_relocations(blob, info, symbols)
    tables = owned_vtables(blob, info, symbols, None)
    return bindings, pending, auth, tables, undefined


def build_family(family, tree, kernelcache, out_root, collection=None,
                 strict=True):
    """Compile, bind and archive one driver family.

    Fails (report + no archive) on a pending bound-class ABI, a missing
    source, a compile failure or an unresolved original import; a partial
    archive is never produced.
    """
    tree, kernelcache = Path(tree).resolve(), Path(kernelcache).resolve()
    out = Path(out_root).resolve() / family
    include = out / 'include'
    include.mkdir(parents=True, exist_ok=True)
    archive = tree / 'build' / family / FAMILY_ARCHIVE[family]
    report = {'family': family, 'archive': str(archive), 'sources': [],
              'objects': [], 'missing': [], 'failed': [], 'pending': [],
              'prebuilt': [], 'bound': {}, 'status': 'incomplete'}
    report['build_source_revision'] = json.loads(
        (RECORDS / 'build-source.json').read_text())['revision']
    report['source_hashes'] = {}
    collection = collection or Collection(kernelcache)

    # 1. Derive bound-class ABI; a pending class blocks the family.
    _, derived_all = derive_abi(kernelcache)
    pending_abi = [c for c, i in derived_all.items()
                   if not (isinstance(i, tuple) and isinstance(i[1], dict))]
    # derive_abi only keeps derived classes in derived_all; re-derive the
    # pending list from the report dict it also returns via write_profile.
    for classname, item in derived_all.items():
        if isinstance(item, tuple) and isinstance(item[1], dict) \
                and not item[0].get('no_header'):
            emit_header(classname, item[0], item[1], item[2], include)
    missing_classes = [c for c in BOUND_CLASSES if c not in derived_all]
    if missing_classes:
        report['pending'] += [{'class': c, 'evidence': 'ABI pending'}
                              for c in missing_classes]

    # 2. Resolve and compile the complete source list.
    generated = out / 'generated'
    generated_sources = []
    if family == 'marble-boot':
        stage_hid(include)
    if family == 'marble-storage':
        try:
            generated_sources.append(stage_storage(tree, include, generated))
        except SystemExit as e:
            report['pending'].append({'stage': 'storage-base', 'evidence': str(e)})
    sources = []
    for name in FAMILIES[family]:
        found = find_source(name, tree)
        if found is None:
            report['missing'].append(name)
        else:
            sources.append(stage_board_source(found, generated))
    if family == 'marble-boot':
        sources += runtime_sources(tree)
        helper = ROOT / 'package/ios27-display/NativeLegacyDisplayHelpers.cpp'
        if helper.is_file():
            subprocess.check_call([sys.executable, str(helper.parent / 'build.py'),
                                   '--tree', str(tree), '--kernelcache', str(kernelcache),
                                   '--out', str(out / 'display-helper-review')],
                                  stdout=subprocess.DEVNULL)
            report['display_helpers'] = str(out / 'display-helper-review/build-report.json')
            sources.append(helper)
        else:
            report['missing'].append(str(helper))
        ioav = ioav_sources()
        sources += ioav
        if not ioav:
            report['pending'].append({
                'adapter': 'package/ios27-ioav',
                'evidence': 'optional IOAV timing/color/hotplug helper adapter '
                           'not yet delivered; the iOS default-surface path '
                           'does not depend on it'})
    for name in FAMILY_ADAPTERS[family]:
        adapter = ROOT / 'package/ios27' / name
        if adapter.exists():
            sources.append(adapter)
        else:
            report['missing'].append(f'package/ios27/{name}')
    for source in component_sources(tree, family):
        if not source.is_file():
            report['missing'].append(str(source))
            continue
        if source.name in ABI_STAGED_SOURCES:
            sources.append(stage_abi_source(source, generated))
        elif 'ABI.h' in source.read_text():
            # Only rewrite when the same-directory header is a stale
            # previous-generation ABI; parent-authored headers stay.
            text = source.read_text()
            stale = False
            for match in re.finditer(
                    r'#\s*include\s+["<]([^">]*Original\w*ABI\.h)[">]', text):
                local = (source.parent / match.group(1)).resolve()
                if local.is_file() and re.search(
                        r'22H384|iOS1[78]|iOS 17|iOS 18', local.read_text()):
                    stale = True
            sources.append(stage_abi_source(source, generated) if stale
                           else source)
        else:
            sources.append(source)
    sources += generated_sources
    source_flags = dict(SOURCE_FLAGS)
    objects = []
    extra_includes = [include, ROOT / 'package', ROOT / 'package/Protocol',
                      tree / 'build' / family, tree / 'kits/marble',
                      tree / 'kits/marble/audio', generated]
    for rel in FAMILY_INCLUDE_DIRS[family]:
        comp_dir = tree / rel
        if comp_dir.is_dir():
            extra_includes.append(comp_dir)
    for source in sources:
        if source.suffix in ('.inc', '.m'):
            continue
        obj = out / 'obj' / f'{source.stem}.o'
        try:
            compile_source(compile_record(tree, source.suffix, source), source, obj,
                           extra_includes + [source.parent],
                           warn_error=generated not in source.parents,
                           extra_args=source_flags.get(source.name, ()))
            _, np = import_xnuprelink()
            blob = obj.read_bytes()
            if has_mod_init(blob, macho_obj(blob)):
                late = obj.with_suffix('.late.o')
                late_object(obj, late, source.name)
                objects.append(late)
            else:
                objects.append(obj)
            report['sources'].append(str(source))
            report['source_hashes'][str(source)] = hashlib.sha256(source.read_bytes()).hexdigest()
        except subprocess.CalledProcessError as e:
            compile_log = out / 'obj' / f'{source.stem}.compile.log'
            compile_log.write_bytes(e.output)
            report['failed'].append({'source': str(source),
                                     'log': str(compile_log),
                                     'error': e.output.decode(errors='replace')})

    # 2b. Storage source-class ABI contract: the rebuilt IOBlockStorageDevice
    # must produce an authenticated vtable matching exactly one original
    # 24B119 table slot-for-slot, and sizeof() must equal the original
    # registration's instance size.
    if family == 'marble-storage':
        base_obj = out / 'obj' / 'IOBlockStorageDevice.o'
        if base_obj.exists():
            try:
                contract = qualify_source_class(
                    base_obj, 'IOBlockStorageDevice',
                    'com.apple.iokit.IOStorageFamily', collection)
                size = compiled_size(generated / 'IOBlockStorageDevice.cpp',
                                     compile_record(tree, '.cpp'), out,
                                     extra_includes)
                contract['source_sizeof'] = size
                contract['instance_size_verified'] = (
                    contract['original_instance_size'] is not None
                    and contract['original_instance_size'] == size)
                if not contract['instance_size_verified']:
                    contract['status'] = 'pending'
                report['storage_abi'] = contract
                (out / 'storage-abi.json').write_text(
                    json.dumps(contract, indent=2) + '\n')
                if contract['status'] != 'verified':
                    original_size = contract['original_instance_size']
                    evidence = (f'original registration size 0x{original_size:x} '
                                f'!= compiled sizeof {size:#x}' if original_size
                                else 'original registration size unproven')
                    report['pending'].append(
                        {'contract': 'IOBlockStorageDevice',
                         'evidence': evidence})
            except SystemExit as e:
                report['pending'].append({'contract': 'IOBlockStorageDevice',
                                          'evidence': str(e)})
        elif generated_sources:
            report['pending'].append({'contract': 'IOBlockStorageDevice',
                                      'evidence': 'base object not compiled'})

    # 2c. Audited subclass vtables: every compiled original-class subclass must
    # carry the exact 24B119 authenticated signature, slot-for-slot, through the
    # independent checker (records/ios27-24B119/compiled-vtable-audit.json).
    audit_json = RECORDS / 'compiled-vtable-audit.json'
    audit_results = {}
    if audit_json.is_file():
        audit_results = {c['table']: c for c in
                         json.loads(audit_json.read_text()).get('classes', [])}
    checker = ROOT / 'scripts/check_ios27_vtable.py'
    for stem, audits in FAMILY_VTABLE_AUDIT.get(family, {}).items():
        obj = out / 'obj' / f'{stem}.o'
        for profile_class, table in audits:
            profile = ABI_PROFILES / f'{profile_class}.json'
            if not obj.is_file():
                report['pending'].append({'vtable_audit': table,
                                          'evidence': f'{stem}.o not compiled'})
                continue
            out_path = out / f'vtable-audit-{table.lstrip("_")}.json'
            try:
                subprocess.check_output(
                    [sys.executable, checker, '--object', obj,
                     '--profile', profile, '--table', table,
                     '--output', out_path],
                    stderr=subprocess.STDOUT, timeout=120)
                result = json.loads(out_path.read_text())
            except (subprocess.CalledProcessError, SystemExit):
                result = (json.loads(out_path.read_text())
                          if out_path.is_file() else
                          {'status': 'pending', 'differences': [
                              {'kind': 'checker', 'evidence': 'audit failed'}]})
            audit_results[table] = result
            if result.get('status') != 'verified':
                report['pending'].append({
                    'vtable_audit': table, 'object': str(obj),
                    'differences': result.get('differences', [])[:8]})
    if audit_results:
        audit_json.parent.mkdir(parents=True, exist_ok=True)
        audit_json.write_text(json.dumps(
            {'classes': sorted(audit_results.values(),
                               key=lambda c: c.get('table', ''))}, indent=2) + '\n')

    # 3. Fold in prebuilt component objects (e.g. the ath11k wifi backend).
    firmware_objects, report['firmware'] = board_firmware(family, out)
    objects += firmware_objects
    for directory in FAMILY_PREBUILT.get(family, []):
        if directory.is_dir():
            prebuilt = sorted(directory.glob('*.o'))
            objects += prebuilt
            report['prebuilt'] += [str(p) for p in prebuilt]
        else:
            report['pending'].append({'prebuilt': str(directory),
                                      'evidence': 'component objects not built'})

    # 4. Resolve original imports across every object.
    bindings, pending = {}, []
    for obj in objects:
        b, p, _, _, _ = resolve_object_bindings(obj, derived_all, collection)
        bindings.update(b)
        pending += p
    report['pending'] += pending

    # 5. Fail rather than archive a subset.
    blockers = report['missing'] + report['failed'] + report['pending']
    if blockers and strict:
        report['status'] = 'pending'
        report['bound'] = {k: hex(v) for k, v in bindings.items()}
        (out / 'build-report.json').parent.mkdir(parents=True, exist_ok=True)
        (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
        return report

    # 6. Emit and assemble original bindings; archive everything.
    binding_objects = []
    if bindings:
        for asm in emit_bindings(bindings, out):
            bo = Path(asm).with_suffix('.o')
            assemble(asm, bo)
            binding_objects.append(bo)
    report['bound'] = {k: hex(v) for k, v in bindings.items()}
    archive.parent.mkdir(parents=True, exist_ok=True)
    archive.unlink(missing_ok=True)
    if objects or binding_objects:
        subprocess.check_call(['xcrun', 'ar', 'rcs', str(archive)]
                              + [str(o) for o in objects + binding_objects])
    report['objects'] = len(objects)
    report['archive_sha256'] = (hashlib.sha256(archive.read_bytes()).hexdigest()
                              if archive.exists() else None)
    report['status'] = 'built' if archive.exists() else 'empty'
    (out / 'build-report.json').parent.mkdir(parents=True, exist_ok=True)
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def build_all(tree, kernelcache, output=None):
    out = Path(output) if output else SCRATCH
    return {f: build_family(f, tree, kernelcache, out)
            for f in ('marble-boot', 'marble-storage', 'marble-gpu')}


def check(tree, kernelcache, output=None):
    tree, kernelcache = Path(tree), Path(kernelcache)
    problems = []
    if not kernelcache.exists():
        problems.append(f'missing kernelcache {kernelcache}')
    for rel in (f'{OBJ_ROOT}/{CXX_RECORD}', f'{OBJ_ROOT}/{C_RECORD}'):
        if not (tree / rel).exists():
            problems.append(f'missing compile record {rel}')
    derived_report = None
    if kernelcache.exists():
        derived_report, _ = derive_abi(kernelcache)
        for name, info in derived_report.items():
            if info.get('status') == 'pending':
                problems.append(f'{name}: {info["evidence"]}')
    return {'ok': not problems, 'problems': problems, 'derived': derived_report}


def write_records(report, kernelcache):
    RECORDS.mkdir(parents=True, exist_ok=True)
    sha = hashlib.sha256(Path(kernelcache).read_bytes()).hexdigest()
    existing = RECORDS / 'drivers.json'
    if existing.is_file():
        previous = json.loads(existing.read_text())
        if previous.get('kernelcache_sha256') == sha:
            report = {**previous.get('families', {}), **report}
    doc = {'target': TARGET, 'kernelcache_sha256': sha, 'families': report}
    (RECORDS / 'drivers.json').write_text(json.dumps(doc, indent=2) + '\n')
    lines = ['# marble iOS 27.2 (24B119) driver ABI build', '',
             f'- kernelcache sha256 `{sha}`',
             f"- arch `{TARGET['arch']}`, xnu `{TARGET['xnu']}`, "
             f"device `{TARGET['device']}`", '', '## Families', '']
    for family, info in report.items():
        lines += [f'### {family}', f"- archive `{info.get('archive')}`",
                  f"- objects {info.get('objects', 0)}",
                  f"- sources {len(info.get('sources', []))}",
                  f"- sha256 `{info.get('archive_sha256')}`", '']
        for item in info.get('failed', []):
            lines.append(f"- FAILED `{item['source']}`")
        lines.append('')
    (RECORDS / 'drivers.md').write_text('\n'.join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('derive', 'build', 'check'),
                        nargs='?', default='build')
    parser.add_argument('--tree', default=str(DEFAULT_TREE))
    parser.add_argument('--kernelcache', default=str(DEFAULT_KERNELCACHE))
    parser.add_argument('--output', default=str(SCRATCH))
    parser.add_argument('--family', choices=tuple(FAMILIES))
    args = parser.parse_args()
    if args.command == 'derive':
        report, _ = derive_abi(args.kernelcache)
        print(json.dumps(report, indent=2))
    elif args.command == 'check':
        result = check(args.tree, args.kernelcache, args.output)
        print(json.dumps({'ok': result['ok'], 'problems': result['problems']},
                         indent=2))
        sys.exit(0 if result['ok'] else 1)
    else:
        if args.family:
            report = {args.family: build_family(args.family, args.tree,
                                                args.kernelcache, args.output)}
        else:
            report = build_all(args.tree, args.kernelcache, args.output)
        write_records(report, args.kernelcache)
        print(json.dumps({k: {'status': v.get('status'),
                             'archive': v.get('archive') if v.get('status') == 'built' else None}
                          for k, v in report.items()},
                         indent=2))
        sys.exit(0 if all(v.get('status') == 'built' for v in report.values()) else 1)


if __name__ == '__main__':
    main()
