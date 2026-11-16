"""Debugger-invoked actual XNU allocation test in an owned, bounded QEMU guest.

This changes guest registers/stack and allocates guest RAM. It is not a
natural driver start, a UFS model, or hardware evidence. No kernel code is
patched; the held candidate is the input. All addresses come from its symbols
or active translation tables. The enclosing runner always terminates QEMU.
"""
import hashlib
import json
import re
import struct
import subprocess
import shlex
import tomllib


def walk4k(read_u64, tcr, ttbr1, mair, va):
    bits = 64 - ((tcr >> 16) & 63)
    if (tcr >> 30) & 3 != 2 or not 25 <= bits <= 48:
        raise RuntimeError('Require active native 4 KiB kernel translation')
    if va >> bits != (1 << (64-bits))-1:
        raise RuntimeError('Require a canonical kernel virtual address')
    table = ttbr1 & 0x0000fffffffff000
    input_va = va & ((1 << bits) - 1)
    chain = []
    for level in range(4 - (bits - 12 + 8) // 9, 4):
        shift = 12 + 9 * (3 - level)
        address = table + ((input_va >> shift) & 511) * 8
        descriptor = read_u64(address)
        chain.append(dict(level=level, descriptor_pa=hex(address), descriptor=hex(descriptor)))
        if not descriptor & 1:
            raise RuntimeError('Allocated page is unmapped')
        if level < 3 and descriptor & 3 == 3:
            table = descriptor & 0x0000fffffffff000
            continue
        if level == 0 or (descriptor & 3) != (3 if level == 3 else 1):
            raise RuntimeError('Invalid hardware leaf')
        pa = (descriptor & 0x0000fffffffff000 & ~((1 << shift)-1)) | (va & ((1 << shift)-1))
        attr = (descriptor >> 2) & 7
        return dict(va=hex(va), pa=hex(pa), level=level, attr_index=attr,
                    mair_byte=(mair >> (8*attr)) & 255, shareability=(descriptor >> 8) & 3,
                    ap=(descriptor >> 6) & 3, accessed=bool(descriptor & 1024), walk=chain)
    raise RuntimeError('No valid leaf')


def exercise(packet, feature, resume, kernel, system_registers, output, esp):
    config = tomllib.loads((esp/'EFI/UniXNU/unixnu.toml').read_text())
    entries = [e for e in config['entry'] if e['id'] == config['boot']['default']]
    if len(entries) != 1:
        raise RuntimeError('Require an unambiguous prepared boot entry')
    boot_args = shlex.split(entries[0].get('args',''))
    if any(a.split('=',1)[0] in ('native-ufs-root','native-ufs-probe') for a in boot_args):
        raise RuntimeError('Allocation-only exercise must not enable UFS startup')
    symbols = subprocess.check_output(['xcrun', 'nm', '--demangle', str(kernel)], text=True)

    def symbol(name):
        found = re.findall(r'^([0-9a-fA-F]+)\s+\S\s+' + re.escape(name) + '$', symbols, re.M)
        if len(found) != 1:
            raise RuntimeError('Missing/ambiguous actual kernel symbol: ' + name)
        return int(found[0], 16)

    functions = {name: symbol(name) for name in (
        'IOBufferMemoryDescriptor::inTaskWithOptions(task*, unsigned int, unsigned long, unsigned long)',
        'IOBufferMemoryDescriptor::getBytesNoCopy()',
        'IOGeneralMemoryDescriptor::getPhysicalSegment(unsigned long long, unsigned long long*, unsigned int)')}
    allocate, get_bytes, get_segment = functions.values()

    def read(address, count):
        data = packet(f'm{address:x},{count:x}')
        if data.startswith('E') or len(data) != count * 2:
            raise RuntimeError(f'Guest memory read failed: {address:x}/{count:x}')
        return bytes.fromhex(data)

    def write(address, data):
        if packet(f'M{address:x},{len(data):x}:' + data.hex()) != 'OK':
            raise RuntimeError('Guest scratch write failed')

    def u64(address):
        return int.from_bytes(read(address, 8), 'little')

    original = packet('g')
    registers = struct.unpack_from('<33Q', bytes.fromhex(original))
    saved_sp, return_pc = registers[31:33]
    scratch = saved_sp - 0x80
    saved_stack = read(scratch, 0x80)
    call_sp = saved_sp - 0x100
    events = []
    report = dict(debugger_invoked=True, natural_driver_start=False, kernel_code_patched=False,
                  allocation_bytes=139264, alignment=4096, options='0x813',
                  functions={k: hex(v) for k, v in functions.items()}, calls=events,
                  source_sha256=hashlib.sha256(open(__file__, 'rb').read()).hexdigest())

    def call(address, *args):
        for index, value in list(enumerate(args)) + [(30, return_pc), (31, call_sp), (32, address)]:
            if packet(f'P{index:x}=' + int(value).to_bytes(8, 'little').hex()) != 'OK':
                raise RuntimeError('Cannot set guest call registers')
        stop, values, interrupted = resume()
        event = dict(function=hex(address), arguments=[hex(v) for v in args],
                     result=hex(values[0]), pc=hex(values[32]), sp=hex(values[31]),
                     interrupted=interrupted)
        events.append(event)
        if interrupted or values[32] != return_pc or values[31] != call_sp:
            raise RuntimeError('Guest allocation API did not return to its bounded caller')
        return values[0]

    regs = {name: int(value, 16) for name, value in system_registers.items()}
    tcr = regs['TCR_EL1']
    bits = 64 - ((tcr >> 16) & 63)
    if (tcr >> 30) & 3 != 2 or not 25 <= bits <= 48 or not regs['SCTLR_EL1'] & 1:
        raise RuntimeError('Require active native 4 KiB kernel translation')
    mair = None
    for include in feature('target.xml').findall('{http://www.w3.org/2001/XInclude}include'):
        for reg in feature(include.attrib['href']).findall('reg'):
            if reg.attrib['name'] == 'MAIR_EL1':
                mair = int.from_bytes(bytes.fromhex(packet(f'p{int(reg.attrib["regnum"]):x}')), 'little')
    if mair is None:
        raise RuntimeError('Cannot inspect active memory attributes')
    report['system_registers'] = dict(system_registers, MAIR_EL1=hex(mair))
    counters = {name: symbol(name) for name in ('_native_ufs_probe_requested', '_native_ufs_mapped')}
    before = {name: int.from_bytes(read(address, 4), 'little') for name, address in counters.items()}
    if any(before.values()):
        raise RuntimeError('Allocation exercise requires UFS probes/MMIO disabled')

    try:
        task = u64(symbol('_kernel_task'))
        descriptor = call(allocate, task, 0x813, 139264, 4096)
        if not descriptor:
            raise RuntimeError('Actual XNU allocation failed')
        va = call(get_bytes, descriptor)
        write(scratch, bytes(8))
        pa = call(get_segment, descriptor, 0, scratch, 0x800)
        contiguous = u64(scratch)
        if not va or not pa or (va | pa) & 4095 or contiguous < 139264:
            raise RuntimeError('Actual XNU DMA span is invalid')
        report.update(descriptor=hex(descriptor), virtual=hex(va), physical=hex(pa), contiguous=contiguous)
        # Every page is queried through the actual API, independently of its
        # initial aggregate contiguous length, then compared with active PTEs.
        segments = []
        for offset in range(0, 139264, 4096):
            actual = call(get_segment, descriptor, offset, scratch, 0x800)
            length = u64(scratch)
            if actual != pa+offset or length < 139264-offset:
                raise RuntimeError('Actual segment walk disagrees with contiguous allocation')
            segments.append(dict(offset=offset, pa=hex(actual), length=length))
        # Exact final byte and out-of-range refusal, with nonzero output sentinel.
        if call(get_segment, descriptor, 139263, scratch, 0x800) != pa+139263 or u64(scratch) != 1:
            raise RuntimeError('Last-byte extent mismatch')
        write(scratch, bytes([0xa5])*8)
        if call(get_segment, descriptor, 139264, scratch, 0x800) or u64(scratch):
            raise RuntimeError('Out-of-range segment accepted')
        old_mode = packet('qqemu.PhyMemMode')
        if old_mode != '0' or packet('Qqemu.PhyMemMode:1') != 'OK':
            raise RuntimeError('Physical page-table inspection unavailable')
        try:
            pages = [walk4k(u64, tcr, regs['TTBR1_EL1'], mair, va+offset) for offset in range(0,139264,4096)]
        finally:
            if packet('Qqemu.PhyMemMode:' + old_mode) != 'OK':
                raise RuntimeError('Cannot restore virtual memory access')
        report.update(segments=segments, pages=pages)
        for offset, page in zip(range(0,139264,4096), pages):
            if int(page['pa'],16) != pa+offset or not page['accessed'] or page['ap'] != 0:
                raise RuntimeError('Active PTE disagrees with actual allocator/segment API')
            # XNU pmap.c's default mapping uses SH_OUTER_MEMORY (2), which
            # includes inner sharing; do not assume the standalone guest's 3.
            if page['mair_byte'] != 0xff or page['shareability'] != 2:
                raise RuntimeError('Expected XNU normal WBWA outer-shareable DMA mapping')
        report.update(passed=True,
                      retained_until_guest_exit=True,
                      scope='Actual XNU allocation and active CPU mapping in TCG; no UFS DMA, SM8550 SMMU or hardware timing')
    except Exception as error:
        report.update(passed=False, error=str(error))
        raise
    finally:
        write(scratch, saved_stack)
        if packet('G' + original) != 'OK' or packet('g') != original:
            raise RuntimeError('Cannot restore original general/vector register state')
        report['registers_restored'] = True
        after = {name: int.from_bytes(read(address,4),'little') for name,address in counters.items()}
        report['ufs_counters_before'] = before
        report['ufs_counters_after'] = after
        if after != before:
            report.update(passed=False, error='Unexpected UFS access during allocation exercise')
        (output/'ufs-allocation.json').write_text(json.dumps(report,indent=2)+'\n')
        if after != before:
            raise RuntimeError('Unexpected UFS access during allocation exercise')
    return report
