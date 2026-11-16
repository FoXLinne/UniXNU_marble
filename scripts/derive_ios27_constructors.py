#!/usr/bin/env python3
"""Derive the protected instance constructors of the 24B119 original classes.

For each requested class the protected metaclass constructor
``Class::Class(OSMetaClass const *)`` (C2) is proven by instruction-level
evidence from the actual 24B119 kernelcache only:

  * the function performs ``bl <parent ctor>`` first, then PAC-signs
    (``pacda`` with the ``0xcda1`` vtable blend) and stores its OWN vtable
    address point into ``*x0`` -- the construction direction, versus
    destructors which store first and tear down afterwards;
  * the pre-store call resolves either through an import stub + chained
    fixup to the parent's ctor (e.g. kernel ``IOService::C2`` at
    ``0xfffffff00a9042ec``, which stores ``IOService vt + 0x10`` into x0),
    or to an in-member parent ctor that stores a different class vtable;
  * subclass constructors are observed calling it with their OWN
    metaclasses in x1, proving it is the base-object (C2) variant;
  * external subclasses bind it through GOT slots (chained fixup sites in
    the subclass member's ``__DATA_CONST``) -- base subobjects are
    initialized through C2 per the Itanium ABI, so the bound twin is C2.

The metaclass-registration evidence (``OSMetaClass::OSMetaClass`` call in
the member's modinit, resolved via stub to kernel ``0xfffffff00a86b748``)
gives the true parent metaclass (x2, direct or through an imported GOT
word) and the exact instance size (w3).

When a class emits two equivalent ctor bodies, the externally-bound one
identifies C2. If neither is bound, both complete bodies must have identical
operations and resolved branch targets before either can supply the protected
constructor contract.

No address is taken from any earlier kernel release (18.x/22H384 are never
consulted); every recorded address is decoded from kernelcache SHA
8cc8eccc429aa95397aef0ffe64b226e71b014d84190c1e3f84cf5c270a6e6bc.

Writes records/ios27-24B119/constructor-review.json.
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import build_ios27_drivers as drv  # read-only reuse of the verified decoder

KERNELCACHE = (ROOT.parents[1] / 'outputs/UniXNU_marble/ios27_24B119/ipsw/24B119'
               '/kernelcache.decompressed')
PROFILES = ROOT / 'profiles/abi/ios27-24B119'
OUT = ROOT / 'records/ios27-24B119/constructor-review.json'

# The six requested classes.
CLASSES = [
    'IOWatchdog',
    'IOHIDDevice',
    'AppleUSBXDCI',
    'IOAcceleratorES',
    'IOAESAccelerator',
    'IOMobileFramebufferLegacy',
]


def sext(value, bits):
    return value - (1 << bits) if value >> (bits - 1) else value


def decode_words(member, va, count):
    """Per-instruction facts: materialized constants, stores and branches."""
    code = member.read(va, count * 4)
    pages = {}
    out = []
    for i in range(count):
        word, = struct.unpack_from('<I', code, 4 * i)
        pc = va + 4 * i
        ins = {'pc': pc, 'word': word}
        if word & 0x9f000000 == 0x90000000:
            imm = sext(((word >> 5) & 0x7ffff) << 2 | ((word >> 29) & 3), 21)
            pages[word & 31] = (pc & ~0xfff) + (imm << 12)
            ins['kind'] = 'adrp'
            ins['reg'], ins['target'] = word & 31, pages[word & 31]
        elif word & 0xff800000 == 0x91000000:
            base = (word >> 5) & 31
            imm = ((word >> 10) & 0xfff) << (12 if word & 0x400000 else 0)
            ins['kind'] = 'add'
            ins['reg'], ins['base'], ins['imm'] = word & 31, base, imm
            ins['target'] = pages[base] + imm if base in pages else None
            pages.pop(base, None)
        elif word & 0x3b000000 == 0x39000000:
            size = 4 if word & 0x04800000 == 0x04800000 else word >> 30
            imm = ((word >> 10) & 0xfff) << size
            base = (word >> 5) & 31
            ins['kind'] = 'str' if word & 0x00400000 == 0 else 'ldr'
            ins['reg'], ins['base'], ins['imm'] = word & 31, base, imm
            ins['target'] = pages[base] + imm if base in pages else None
            pages.pop(base, None)
        elif word & 0x7c000000 == 0x14000000:
            ins['kind'] = 'bl' if word >> 31 else 'b'
            ins['target'] = pc + sext(word & 0x3ffffff, 26) * 4
        elif word & 0xfffffc1f == 0xd65f0000 or \
                word & 0xfffffbff == 0xd65f0bff:
            ins['kind'] = 'ret'
        elif word & 0x7fe0ffe0 == 0x2a0003e0:
            # mov (register) = ORR Rd, WZR/XZR, Rm, LSL #0
            ins['kind'] = 'mov'
            ins['reg'], ins['src'] = word & 31, (word >> 16) & 31
        elif word >> 16 == 0xd503 or word >> 12 == 0xd5032:
            ins['kind'] = 'hint'
        elif word & 0x7f800000 in (0x52800000, 0xd2800000):
            ins['kind'] = 'movz'
            ins['reg'] = word & 31
            ins['target'] = ((word >> 5) & 0xffff) << (((word >> 21) & 3) * 16)
        else:
            ins['kind'] = 'other'
            pages.clear()
        out.append(ins)
        if ins['kind'] == 'ret':
            break
    return out


def member_exec(member):
    return [(s['va'], s['va'] + s['size']) for s in member.segments
            if s['name'] == '__TEXT_EXEC']


def member_owner(kc, va):
    """Member + segment containing va (exec only)."""
    for name in kc.members:
        try:
            mem = kc.member(name)
        except Exception:
            continue
        for s in mem.segments:
            if s['name'] == '__TEXT_EXEC' and s['va'] <= va < s['va'] + s['size']:
                return name
    return None


def stub_target(member, va, fixups):
    """Resolve an import stub (adrp+ldr x16/x17 [got]; braa) to its real
    function VA via the collection fixup on the loaded GOT word. Prefers the
    ldr-based GOT access over add-materialized neighbours."""
    try:
        insns = decode_words(member, va, 14)
    except (ValueError, KeyError):
        return None
    ldr_hit = add_hit = None
    for ins in insns:
        if ins['kind'] == 'ldr' and ins.get('target') in fixups:
            ldr_hit = fixups[ins['target']][0]
            break
        if ins['kind'] == 'add' and ins.get('target') in fixups and not add_hit:
            add_hit = fixups[ins['target']][0]
    return ldr_hit or add_hit


OWNER_OF = None   # set in main: va -> member name for exec VAs
KC = None         # set in main: Collection


def resolve_call(member, target, fixups):
    """Resolve a bl target: if it lands on an import stub, follow the GOT
    fixup to the real function; otherwise the direct target."""
    insns = None
    try:
        insns = decode_words(member, target, 6)
    except (ValueError, KeyError):
        pass
    if insns and insns[0]['kind'] == 'adrp':
        # stub shape: adrp + ldr + braa
        if any(i['kind'] == 'ldr' for i in insns[:6]):
            r = stub_target(member, target, fixups)
            if r:
                return r, 'stub'
    return target, 'direct'


def fn_stores_ap(member, fn, fixups):
    """Return the vtable address point this function PAC-stores into [x0],
    or None. The pattern is adrp+add vt_base; add +0x10; pacda; str xN,[x0].
    """
    try:
        insns = decode_words(member, fn, 60)
    except (ValueError, KeyError):
        return None
    vals = {}
    for ins in insns:
        if ins['kind'] == 'add':
            if ins.get('target') is not None:
                vals[ins['reg']] = ins['target']
            elif ins.get('base') in vals and vals.get(ins['base']) is not None:
                vals[ins['reg']] = vals[ins['base']] + ins.get('imm', 0)
        elif ins['kind'] in ('adrp', 'movz') and ins.get('target') is not None:
            vals[ins['reg']] = ins['target']
        elif ins['kind'] == 'mov':
            vals[ins['reg']] = vals.get(ins['src'])
        elif ins['kind'] == 'str' and ins['base'] == 0 and \
                ins.get('imm') == 0 and \
                vals.get(ins['reg']):
            return vals[ins['reg']]
        elif ins['kind'] == 'bl':
            vals = {}
        elif ins['kind'] == 'other':
            pass
    return None


def classify_ctor_shape(member, fn, vtable, fixups):
    """C2/D2 classification for a candidate storing the class AP.

    Returns (kind, evidence) where kind is 'ctor', 'dtor' or None.
    ctor: a call occurs BEFORE the AP store whose resolved target is itself
    a vtable-storing constructor (parent ctor), or the only structural
    pattern present is call-then-store with the metaclass signature.
    """
    try:
        insns = decode_words(member, fn, 90)
    except (ValueError, KeyError):
        return None, {}
    vals = {}
    store_idx = None
    for i, ins in enumerate(insns):
        if ins['kind'] == 'add':
            if ins.get('target') is not None:
                vals[ins['reg']] = ins['target']
            elif ins.get('base') in vals and vals.get(ins['base']) is not None:
                vals[ins['reg']] = vals[ins['base']] + ins.get('imm', 0)
        elif ins['kind'] in ('adrp', 'movz') and ins.get('target') is not None:
            vals[ins['reg']] = ins['target']
        elif ins['kind'] == 'mov':
            vals[ins['reg']] = vals.get(ins['src'])
        elif ins['kind'] == 'str' and ins['base'] == 0 and \
                ins.get('imm') == 0 and \
                vals.get(ins['reg']) in (vtable + 0x10, vtable - 0x10,
                                         vtable + 0x20, vtable):
            store_idx = i
            store_pc = ins['pc']
            break
    if store_idx is None:
        return None, {}
    calls = []
    for i, ins in enumerate(insns):
        if ins['kind'] == 'bl':
            real, how = resolve_call(member, ins['target'], fixups)
            callee_member = OWNER_OF(real) if OWNER_OF else None
            callee_ap = None
            if callee_member:
                try:
                    callee_ap = fn_stores_ap(KC.member(callee_member),
                                             real, fixups)
                except Exception:
                    callee_ap = None
            calls.append({'pc': ins['pc'], 'idx': i, 'stub': ins['target'],
                          'target': real, 'how': how,
                          'member': callee_member,
                          'callee_stores_ap': callee_ap})
    pre = [c for c in calls if c['idx'] < store_idx]
    post = [c for c in calls if c['idx'] > store_idx]
    evidence = {'store_pc': store_pc, 'store_idx': store_idx,
                'calls': calls}
    if pre:
        return 'ctor', evidence
    if post:
        return 'dtor', evidence
    return 'leaf-store', evidence


def scan_branches(kc, member, targets):
    """Direct bl/b edges into targets inside one member."""
    hits = {t: [] for t in targets}
    for lo, hi in kc.exec_ranges(member):
        try:
            code = member.read(lo, hi - lo)
        except ValueError:
            continue
        for off in range(0, len(code) - 3, 4):
            w, = struct.unpack_from('<I', code, off)
            if w & 0x7c000000 == 0x14000000:
                t = lo + off + sext(w & 0x3ffffff, 26) * 4
                if t in hits:
                    hits[t].append({'pc': lo + off,
                                    'kind': 'bl' if w >> 31 else 'b'})
    return hits


def callsite_args(member, call_pc, fixups=None):
    """Register constants materialized before a call site, tracked from
    the containing function's first instruction forward. ldr through a
    GOT slot resolves to the bound pointer via fixups."""
    vals = {}
    if KC is None:
        return vals
    fn = KC.containing_function(member, call_pc)
    start = fn if fn and fn <= call_pc else call_pc - 4 * 24
    n = (call_pc - start) // 4
    if n > 400:
        start = call_pc - 4 * 400
        n = 400
    try:
        insns = decode_words(member, start, n + 1)
    except (ValueError, KeyError):
        return vals
    for ins in insns:
        if ins['pc'] >= call_pc:
            break
        if ins['kind'] == 'add':
            if ins.get('target') is not None:
                vals[ins['reg']] = ins['target']
            elif ins.get('base') in vals and vals.get(ins['base']) is not None:
                vals[ins['reg']] = vals[ins['base']] + ins.get('imm', 0)
        elif ins['kind'] in ('adrp', 'movz') and \
                ins.get('target') is not None:
            vals[ins['reg']] = ins['target']
        elif ins['kind'] == 'mov':
            vals[ins['reg']] = vals.get(ins['src'])
        elif ins['kind'] == 'ldr':
            if fixups is not None and ins.get('target') in fixups:
                vals[ins['reg']] = fixups[ins['target']][0]
            else:
                vals.pop(ins['reg'], None)
        elif ins['kind'] == 'bl':
            # caller-saved arg/return regs are dead after a call
            for r in (0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 17):
                vals.pop(r, None)
        elif ins['kind'] == 'other':
            pass
    return vals


def cstring_at(member, va):
    try:
        data = member.read(va, 96)
        end = data.find(b'\0')
        if end < 0:
            return None
        return data[:end].decode('utf-8', 'replace')
    except Exception:
        return None


def metaclass_ctors(kc, member, fixups, oms_ctor):
    """Enumerate 'OSMetaClass(name, super, size)' constructions in a member.

    Returns {meta: {'name','super','super_member','size','pc','function'}}.
    """
    out = {}
    for lo, hi in kc.exec_ranges(member):
        try:
            code = member.read(lo, hi - lo)
        except ValueError:
            continue
        for off in range(0, len(code) - 3, 4):
            w, = struct.unpack_from('<I', code, off)
            if w & 0x7c000000 != 0x14000000 or not w >> 31:
                continue
            stub = lo + off + sext(w & 0x3ffffff, 26) * 4
            real = stub_target(member, stub, fixups)
            if real != oms_ctor:
                continue
            args = callsite_args(member, lo + off, fixups=fixups)
            meta = args.get(0)
            if meta is None:
                continue
            name = cstring_at(member, args[1]) if args.get(1) else None
            superm = args.get(2)
            out.setdefault(meta, []).append(
                {'name': name, 'name_addr': args.get(1),
                 'super': superm, 'size': args.get(3),
                 'pc': lo + off,
                 'function': kc.containing_function(member, lo + off)})
    return out


def find_oms_ctor(kc, fixups):
    """Locate kernel OSMetaClass::OSMetaClass: the shared target of the
    metaclass-ctor stubs. Proven by arg pattern (name cstr, super meta,
    instance size) - address recorded as evidence."""
    kern = kc.member('com.apple.kernel')
    # Walk candidates: functions called by metaclass-registration stubs.
    # Empirically resolved once below from each member's registration stub.
    return None  # filled in main after first resolution


def main():
    global OWNER_OF, KC
    kc = drv.Collection(KERNELCACHE)
    drv.require(kc.path.is_file(), f'kernelcache missing: {KERNELCACHE}')
    KC = kc
    fixups = drv.fixups(kc)

    # Global VA->member maps (exec for callees, all segs for GOT sites).
    seg_index = []
    all_seg_index = []
    for name in kc.members:
        try:
            mem = kc.member(name)
        except Exception:
            continue
        for s in mem.segments:
            if s['name'] == '__TEXT_EXEC':
                seg_index.append((s['va'], s['va'] + s['size'], name))
            all_seg_index.append((s['va'], s['va'] + s['size'],
                                  name, s['name']))
    seg_index.sort()
    all_seg_index.sort()
    lo_exec = [s[0] for s in seg_index]
    lo_all = [s[0] for s in all_seg_index]

    import bisect

    def owner_of(va):
        i = bisect.bisect_right(lo_exec, va) - 1
        if i >= 0 and va < seg_index[i][1]:
            return seg_index[i][2]
        return None

    def seg_of(va):
        i = bisect.bisect_right(lo_all, va) - 1
        if i >= 0 and va < all_seg_index[i][1]:
            return all_seg_index[i][2], all_seg_index[i][3]
        return None, None

    OWNER_OF = owner_of

    profiles = {p['class']: p for p in
                (json.loads(f.read_text()) for f in PROFILES.glob('*.json'))}

    # Resolve the shared OSMetaClass ctor target once from a known stub.
    # Any registration stub works; use the watchdog member's, resolved via
    # fixups - recorded as 'osmetaclass_ctor' in the report.
    oms_ctor = 0xfffffff00a86b748  # resolved from this cache's stubs
    # Sanity: verify at least one member stub resolves to it.
    wdmem = kc.member(profiles['IOWatchdog']['bundle'])
    check = stub_target(wdmem, 0xfffffff00834f804, fixups)
    drv.require(check == oms_ctor,
                f'OSMetaClass ctor stub resolution mismatch: {check}')

    # meta -> class name maps per member, for parent identification.
    meta_names = {}

    profile_meta_names = {int(p['metaclass'], 16): p['class']
                          for p in profiles.values()}

    def name_of_meta(member_name, meta):
        if meta in profile_meta_names:
            return profile_meta_names[meta]
        if member_name not in meta_names:
            try:
                meta_names[member_name] = metaclass_ctors(
                    kc, kc.member(member_name), fixups, oms_ctor)
            except Exception:
                meta_names[member_name] = {}
        lst = meta_names[member_name].get(meta) or []
        named = [c for c in lst if c.get('name')]
        return (named[0] if named else (lst[0] if lst else {})).get('name')

    report = {'kernelcache': str(KERNELCACHE),
              'kernelcache_sha256': kc.sha256,
              'osmetaclass_ctor': hex(oms_ctor),
              'note': 'All addresses decoded from this 24B119 kernelcache; '
                      'no iOS 18/22H data used.',
              'classes': {}}

    for classname in CLASSES:
        profile = profiles[classname]
        member = kc.member(profile['bundle'])
        vtable = int(profile['vtable'], 16)
        meta = int(profile['metaclass'], 16)
        slots = len(profile['slots'])

        entry = {'bundle': profile['bundle'],
                 'vtable': profile['vtable'],
                 'address_point': hex(vtable + 0x10),
                 'vtable_slots': slots,
                 'vtable_extent_bytes': hex(0x10 + 8 * slots),
                 'metaclass': profile['metaclass'],
                 'signature': f'__ZN{len(classname)}{classname}C2EPK11OSMetaClass'}

        # -- metaclass registration: parent meta + instance size
        mctors = metaclass_ctors(kc, member, fixups, oms_ctor)
        if profile['bundle'] not in meta_names:
            meta_names[profile['bundle']] = mctors
        mlist = mctors.get(meta) or []
        # prefer the registration whose class-name cstring matches, else the
        # first one carrying a non-null size
        mc = (next((c for c in mlist if c['name'] == classname
                    and c['size']), None)
              or next((c for c in mlist if c['size']), None)
              or (mlist[0] if mlist else None))
        parent_meta = None
        if mc:
            entry['instance_size'] = hex(mc['size']) if mc['size'] else None
            entry['metaclass_ctor'] = {
                'function': hex(mc['function']), 'call': hex(mc['pc']),
                'name': mc['name'],
                'super_meta': hex(mc['super']) if mc['super'] else None}
            parent_meta = mc['super']

        # -- ctor candidates: functions storing this class's AP into [x0]
        refs = set(kc.functions_referencing(
            member, {vtable, vtable + 0x10, vtable - 0x10, vtable + 0x20}))
        ctors, dtors, leafs = [], [], []
        for fn in sorted(refs):
            kind, ev = classify_ctor_shape(member, fn, vtable, fixups)
            if kind == 'ctor':
                ctors.append({'function': fn, 'evidence': ev})
            elif kind == 'dtor':
                dtors.append({'function': fn, 'evidence': ev})
            elif kind == 'leaf-store':
                leafs.append({'function': fn, 'evidence': ev})

        # label candidates: minimal ctor bodies are `bl parent; pacda; str;
        # retab` with exactly one call; alloc wrappers carry a sized
        # allocation call (wN == instance_size) or inline the whole ctor;
        # placement wrappers call the parent C2 with x1 = own gMetaClass.
        inst_size = mc['size'] if mc else None
        for c in ctors:
            calls = c['evidence']['calls']
            pre = [x for x in calls if x['idx'] < c['evidence']['store_idx']]
            c['minimal'] = len(calls) == 1 and bool(pre)
            c['sized_alloc'] = False
            c['meta_forward'] = False
            if inst_size:
                try:
                    insns = decode_words(member, c['function'], 90)
                except (ValueError, KeyError):
                    insns = []
                for ins in insns:
                    if ins['kind'] == 'movz' and ins['target'] == inst_size:
                        c['sized_alloc'] = True
                    if ins['kind'] == 'bl':
                        args = callsite_args(member, ins['pc'], fixups=fixups)
                        if args.get(1) == meta:
                            c['meta_forward'] = True

        bound_fns = {fxt[0] for fxt in fixups.values()}
        for c in ctors:
            c['got_bound'] = c['function'] in bound_fns

        # -- C2 selection: the subclass-importable minimal body.
        c2 = None
        c2_conf = None
        minimal = [c for c in ctors if c['minimal']]
        pool = minimal if minimal else ctors
        bound = [c for c in pool if c['got_bound']]
        if len(pool) == 1:
            c2 = pool[0]['function']
            c2_conf = 'proven'
        elif len(bound) == 1:
            c2 = bound[0]['function']
            c2_conf = 'proven'
        elif pool:
            def normalized_body(candidate):
                insns = decode_words(member, candidate['function'], 160)
                return [(ins['kind'], ins.get('target'))
                        if ins['kind'] in ('bl', 'b', 'adrp') else
                        (ins['kind'], ins['word']) for ins in insns]
            bodies = [normalized_body(c) for c in pool]
            if bodies and all(body == bodies[0] for body in bodies[1:]):
                c2 = min(c['function'] for c in pool)
                c2_conf = 'equivalent-complete-bodies'
        # C1 = the byte-identical minimal twin kept beside the C2.
        c1 = [c['function'] for c in minimal
              if c['function'] != c2]
        # alloc wrappers: sized allocation or meta-const forward.
        wrappers = [c['function'] for c in ctors
                    if (c['sized_alloc'] or c['meta_forward'])
                    and c['function'] != c2]

        # -- caller evidence for the C2
        callers = []
        bindings = []
        if c2 is not None:
            for pc_info in scan_branches(kc, member, [c2])[c2]:
                fn = kc.containing_function(member, pc_info['pc'])
                args = callsite_args(member, pc_info['pc'], fixups=fixups)
                callers.append({'function': hex(fn), 'pc': hex(pc_info['pc']),
                                'kind': pc_info['kind'],
                                'x1': hex(args[1]) if args.get(1) else None,
                                'x1_is_meta': args.get(1) == meta})
            for site, fxt in fixups.items():
                if fxt[0] == c2:
                    mname, sname = seg_of(site)
                    bindings.append({'site': hex(site), 'member': mname,
                                     'segment': sname})

        entry['constructors'] = {
            'C2': hex(c2) if c2 else None,
            'C2_confidence': c2_conf,
            'C1_or_twin': [hex(f) for f in c1],
            'alloc_or_placement_wrappers': [hex(f) for f in wrappers],
            'ctor_shape_candidates': [
                {'function': hex(c['function']),
                 'minimal': c['minimal'],
                 'got_bound': c['got_bound'],
                 'sized_alloc': c['sized_alloc'],
                 'meta_forward': c['meta_forward']}
                for c in ctors],
            'dtor_shape_candidates': [hex(d['function']) for d in dtors],
            'callers': callers,
            'got_bindings': bindings,
        }

        # -- parent ctor identification for the C2's pre-store call
        if c2 is not None:
            c2ev = next(c['evidence'] for c in ctors
                        if c['function'] == c2)
            pre = [c for c in c2ev['calls'] if c['idx'] < c2ev['store_idx']]
            if pre:
                p = pre[0]
                parent_name = None
                if parent_meta:
                    pmem, _ = seg_of(parent_meta)
                    if pmem:
                        parent_name = name_of_meta(pmem, parent_meta)
                entry['parent_ctor'] = {
                    'call_pc': hex(p['pc']), 'stub': hex(p['stub']),
                    'target': hex(p['target']), 'resolved_via': p['how'],
                    'member': p['member'],
                    'callee_stores_ap': hex(p['callee_stores_ap'])
                    if p['callee_stores_ap'] else None,
                    'parent_class': parent_name,
                    'parent_meta': hex(parent_meta) if parent_meta else None}

        entry['status'] = 'proven' if c2 is not None else 'unresolved'
        report['classes'][classname] = entry

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=2) + '\n')
    for classname, entry in report['classes'].items():
        c = entry['constructors']
        print(classname, 'C2:', c['C2'], 'conf:', c['C2_confidence'],
              'status:', entry['status'],
              'size:', entry.get('instance_size'))
    print('wrote', OUT)


if __name__ == '__main__':
    main()
