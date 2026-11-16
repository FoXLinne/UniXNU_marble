#!/usr/bin/env python3
"""Exercise the loader's input records (LoaderInputs.h): the HOB and device tree readers that locate the payload
RAM disk, the memory type of a range, and the release of verified inputs into the excluded-range table."""
import ctypes
import mmap
from pathlib import Path
import random
import struct
import subprocess
import tempfile

driver = Path(__file__).resolve().parents[1] / 'DarwinBootPkg/Driver'
RESERVED, LOADER_CODE, LOADER_DATA, BS_CODE, BS_DATA, RT_CODE, RT_DATA, FREE = range(8)
ACPI_RECLAIM, ACPI_NVS, MMIO = 9, 10, 11
USABLE = {FREE, LOADER_CODE, LOADER_DATA}
RELEASABLE = USABLE | {BS_DATA, RESERVED}
LOCATED, VERIFIED, INSIDE, USABLE_BIT = 1, 2, 4, 8
NONE = 0xFFFFFFFF
STRIDE = 48
MIB = 1 << 20
MAX = (1 << 64) - 1
GUID = bytes.fromhex('e682eeda9080724ebfb978c13cd4e359')  # DAEE82E6-8090-4E72-BFB9-78C13CD4E359 as stored


def pack(descriptors, stride=STRIDE):
    return b''.join(struct.pack('<I4xQQQQ', kind, start, 0, size >> 12, 0) + bytes(stride - 40) for kind, start, size in descriptors)


def merged(ranges):
    out = []
    for start, end in sorted(ranges):
        if out and start <= out[-1][1]:
            out[-1][1] = max(out[-1][1], end)
        else:
            out.append([start, end])
    return out


def reference_exclusions(usable, base, size):
    out, cursor = [], base
    for start, end in merged(usable):
        start, end = max(start, base), min(end, base + size)
        if start >= end:
            continue
        if start > cursor:
            out.append((cursor, start - cursor))
        cursor = max(cursor, end)
    if cursor < base + size:
        out.append((cursor, base + size - cursor))
    return out


def reference_type(descriptors, start, end):
    """The one type that covers [start, end), found by walking pages."""
    if start >= end:
        return NONE
    kinds = set()
    cursor = start
    while cursor < end:
        owner = [(kind, a + n) for kind, a, n in descriptors if n and a <= cursor < a + n]
        if not owner:
            return NONE
        kinds.add(owner[0][0])
        cursor = owner[0][1]
    return kinds.pop() if len(kinds) == 1 else NONE


def fdt(nodes, version=17, last=16, total_pad=0, magic=0xD00DFEED):
    """nodes: nested (name, [(property, value)], [children]) from the root."""
    strings, offsets, structure = b'', {}, b''

    def name_offset(name):
        nonlocal strings
        if name not in offsets:
            offsets[name] = len(strings)
            strings += name.encode() + b'\0'
        return offsets[name]

    def emit(entry):
        nonlocal structure
        name, properties, children = entry
        structure += struct.pack('>I', 1) + name.encode() + b'\0' + bytes(-(len(name) + 1) % 4)
        for key, value in properties:
            structure += struct.pack('>III', 3, len(value), name_offset(key)) + value + bytes(-len(value) % 4)
        for child in children:
            emit(child)
        structure += struct.pack('>I', 2)

    emit(nodes)
    structure += struct.pack('>I', 9)
    reserve = bytes(16)
    off_struct = 40 + len(reserve)
    off_strings = off_struct + len(structure)
    total = off_strings + len(strings) + total_pad
    header = struct.pack('>10I', magic, total, off_struct, off_strings, 40, version, last, 0, len(strings), len(structure))
    return header + reserve + structure + strings + bytes(total_pad)


def hob_list(address, entries, end=True):
    """entries: (type, payload). The first is the hand-off table, whose end address is filled in."""
    body = b''
    for kind, payload in entries:
        length = 8 + len(payload) + (-len(payload) % 8)
        body += struct.pack('<HHI', kind, length, 0) + payload + bytes(-len(payload) % 8)
    table = bytearray(struct.pack('<HHI', 1, 56, 0) + struct.pack('<II5Q', 9, 0, 0, 0, 0, 0, address + 56 + len(body)))
    return bytes(table) + body + (struct.pack('<HHI', 0xFFFF, 8, 0) if end else b'')


with tempfile.TemporaryDirectory(prefix='darwinboot-inputs-') as temporary:
    work = Path(temporary)
    (work / 'test.c').write_text('''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef int BOOLEAN; typedef char CHAR8;
#define STATIC static
#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0
#define MAX_UINT64 UINT64_MAX
#define EFI_PAGE_SIZE 4096
#define CompareMem memcmp
#define ZeroMem(p, n) memset((p), 0, (n))
enum { EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode, EfiBootServicesData,
       EfiRuntimeServicesCode, EfiRuntimeServicesData, EfiConventionalMemory };
typedef struct { UINT32 Type; UINT64 PhysicalStart; UINT64 VirtualStart; UINT64 NumberOfPages; UINT64 Attribute; } EFI_MEMORY_DESCRIPTOR;
static UINT16 ReadUnaligned16(const UINT16 *p) { UINT16 v; memcpy(&v, p, 2); return v; }
static UINT64 ReadUnaligned64(const UINT64 *p) { UINT64 v; memcpy(&v, p, 8); return v; }
static void WriteUnaligned32(UINT32 *p, UINT32 v) { memcpy(p, &v, 4); }
static void WriteUnaligned64(UINT64 *p, UINT64 v) { memcpy(p, &v, 8); }
#include "LoaderInputs.h"
int hob_value(const UINT8 *hob, size_t limit, const UINT8 *guid, UINT64 *value) { return DarwinHobValue(hob, limit, guid, value); }
size_t hob_size(const UINT8 *hob, UINT64 address, UINT64 limit) { return DarwinHobListSize(hob, address, limit); }
int fdt_initrd(const UINT8 *fdt, UINT64 *start, UINT64 *end) { return DarwinFdtInitrd(fdt, start, end); }
UINT32 range_type(const void *map, size_t size, size_t stride, UINT64 start, UINT64 end) {
  return DarwinRangeType(map, size, stride, start, end);
}
// inputs: the two records as base, size, state triples; out: the records as the device tree property holds them.
size_t describe(void *map, size_t size, size_t capacity, size_t stride, UINT64 base, UINT64 window, int release,
                const UINT64 *inputs, UINT8 *out) {
  DARWIN_LOADER_INPUT records[DARWIN_INPUT_COUNT];
  memset(records, 0xa5, sizeof(records));
  for (int i = 0; i < DARWIN_INPUT_COUNT; i++) {
    records[i].Base = inputs[3 * i]; records[i].Size = inputs[3 * i + 1]; records[i].State = (UINT32)inputs[3 * i + 2];
  }
  DarwinDescribeInputs(map, &size, capacity, stride, base, window, release, records);
  DarwinWriteInputs(out, records);
  return size;
}
size_t exclusions(const void *map, size_t size, size_t stride, UINT64 base, UINT64 window, UINT8 *table,
                  size_t capacity, UINT64 *excluded) {
  return DarwinWindowExclusions(map, size, stride, base, window, table, capacity, excluded);
}
UINT64 run_end(const void *map, size_t size, size_t stride, int free_only, UINT64 address, UINT64 limit) {
  return DarwinRunEnd(map, size, stride, free_only, address, limit);
}
size_t record_size(void) { return sizeof(DARWIN_LOADER_INPUT); }
''')
    library = work / 'inputs.dylib'
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fsanitize=undefined',
                    '-fno-sanitize-recover=all', '-shared', '-fPIC', '-I', str(driver), str(work / 'test.c'), '-o', str(library)], check=True)
    lib = ctypes.CDLL(str(library))
    u64, size_t = ctypes.c_uint64, ctypes.c_size_t
    lib.hob_value.argtypes = [ctypes.c_char_p, size_t, ctypes.c_char_p, ctypes.POINTER(u64)]
    lib.hob_size.restype = size_t
    lib.hob_size.argtypes = [ctypes.c_char_p, u64, u64]
    lib.fdt_initrd.argtypes = [ctypes.c_char_p, ctypes.POINTER(u64), ctypes.POINTER(u64)]
    lib.range_type.restype = ctypes.c_uint32
    lib.range_type.argtypes = [ctypes.c_char_p, size_t, size_t, u64, u64]
    lib.describe.restype = size_t
    lib.describe.argtypes = [ctypes.c_char_p, size_t, size_t, size_t, u64, u64, ctypes.c_int, ctypes.POINTER(u64), ctypes.c_char_p]
    lib.exclusions.restype = size_t
    lib.exclusions.argtypes = [ctypes.c_char_p, size_t, size_t, u64, u64, ctypes.c_char_p, size_t, ctypes.POINTER(u64)]
    lib.run_end.restype = u64
    lib.run_end.argtypes = [ctypes.c_char_p, size_t, size_t, ctypes.c_int, u64, u64]
    lib.record_size.restype = size_t
    assert lib.record_size() == 32

    libc = ctypes.CDLL(None, use_errno=True)
    libc.mprotect.argtypes = [ctypes.c_void_p, size_t, ctypes.c_int]
    guarded = []

    def exact(blob):
        """A copy that ends at an inaccessible page, so a read past the end of the blob stops the test."""
        page = mmap.PAGESIZE
        size = (len(blob) + page - 1) // page * page + page
        region = mmap.mmap(-1, size)
        address = ctypes.addressof(ctypes.c_char.from_buffer(region))
        assert libc.mprotect(address + size - page, page, 0) == 0
        ctypes.memmove(address + size - page - len(blob), blob, len(blob))
        guarded.append(region)
        return ctypes.cast(address + size - page - len(blob), ctypes.c_char_p)

    # --- The entry argument HOB ---
    def hob_value(blob, address=0x9F000000, limit=1 << 24):
        size = lib.hob_size(exact(blob), address, limit)
        value = u64(0xDEAD)
        if not size:
            return None
        assert size <= len(blob)
        return value.value if lib.hob_value(exact(blob), size, GUID, ctypes.byref(value)) else None

    other = bytes(range(16))
    assert hob_value(hob_list(0x9F000000, [(3, bytes(40)), (4, other + struct.pack('<Q', 1)), (4, GUID + struct.pack('<Q', 0xA7C00000))])) == 0xA7C00000
    assert hob_value(hob_list(0x9F000000, [(4, GUID + struct.pack('<QQ', 0x81A00000, 7))])) == 0x81A00000, 'longer data: the first eight bytes'
    assert hob_value(hob_list(0x9F000000, [(3, bytes(40)), (4, other + struct.pack('<Q', 1))])) is None, 'no such HOB'
    assert hob_value(hob_list(0x9F000000, [(4, GUID)])) is None, 'no data'
    assert hob_value(hob_list(0x9F000000, [(0xFFFF, b''), (4, GUID + struct.pack('<Q', 5))])) is None, 'behind the end of the list'
    assert hob_value(hob_list(0x9F000000, [(4, GUID + struct.pack('<Q', 5))]), address=0x9F000100) is None, 'end address before the list'
    assert hob_value(hob_list(0x9F000000, [(4, GUID + struct.pack('<Q', 5))]), limit=64) is None, 'list longer than the limit'
    assert hob_value(b'\x03\x00' + hob_list(0x9F000000, [(4, GUID + struct.pack('<Q', 5))])[2:]) is None, 'no hand-off table in front'
    cut = bytearray(hob_list(0x9F000000, [(4, GUID + struct.pack('<Q', 5))]))
    struct.pack_into('<Q', cut, 48, 0x9F000000 + 56 + 16)  # the list ends inside the HOB
    assert hob_value(bytes(cut)) is None, 'a HOB that reaches behind the end of the list'
    broken = bytearray(hob_list(0x9F000000, [(3, bytes(40)), (4, GUID + struct.pack('<Q', 5))]))
    struct.pack_into('<H', broken, 58, 4)  # a HOB shorter than its header
    assert hob_value(bytes(broken)) is None
    struct.pack_into('<H', broken, 58, 0x4000)  # a HOB that leaves the list
    assert hob_value(bytes(broken)) is None

    # --- The first-stage loader's device tree ---
    def initrd(blob):
        start, end = u64(), u64()
        return (start.value, end.value) if lib.fdt_initrd(exact(blob), ctypes.byref(start), ctypes.byref(end)) else None

    def tree(chosen_properties, **options):
        return fdt(('', [('model', b'board\0')], [('memory', [('reg', bytes(16))], []),
                                                 ('chosen', chosen_properties, [('inner', [('x', bytes(4))], [])]),
                                                 ('soc', [], [('uart', [('status', b'okay\0')], [])])]), **options)

    be32, be64 = (lambda value: struct.pack('>I', value)), (lambda value: struct.pack('>Q', value))
    both = [('bootargs', b'console=tty0\0'), ('linux,initrd-start', be64(0xAB800000)), ('linux,initrd-end', be64(0xB8000000))]
    assert initrd(tree(both)) == (0xAB800000, 0xB8000000)
    assert initrd(tree(list(reversed(both)))) == (0xAB800000, 0xB8000000)
    assert initrd(tree([('linux,initrd-start', be32(0xB6D7A000)), ('linux,initrd-end', be32(0xB7FFF394))])) == (0xB6D7A000, 0xB7FFF394)
    assert initrd(tree([('linux,initrd-start', be32(0x1000)), ('linux,initrd-end', be64(0x8C0000000))])) == (0x1000, 0x8C0000000)
    assert initrd(tree(both, total_pad=4096)) == (0xAB800000, 0xB8000000)
    refused_trees = {
        'no start': tree(both[:1] + both[2:]), 'no end': tree(both[:2]), 'no properties': tree([]),
        'end before start': tree([('linux,initrd-start', be64(0x2000)), ('linux,initrd-end', be64(0x1000))]),
        'empty': tree([('linux,initrd-start', be64(0x2000)), ('linux,initrd-end', be64(0x2000))]),
        'two starts': tree(both + [('linux,initrd-start', be64(1))]),
        'three-byte cell': tree([('linux,initrd-start', b'\0\0\1'), ('linux,initrd-end', be64(0x2000))]),
        'sixteen-byte cell': tree([('linux,initrd-start', bytes(16)), ('linux,initrd-end', be64(0x2000))]),
        'bad magic': tree(both, magic=0xD00DFEEE), 'version 16': tree(both, version=16),
        'not backward compatible': tree(both, last=18),
        'properties in the root': fdt(('', both, [('chosen', [], [])])),
        'properties below chosen': fdt(('', [], [('chosen', [], [('inner', both, [])])])),
        'chosen below another node': fdt(('', [], [('soc', [], [('chosen', both, [])])])),
        'another name': fdt(('', [], [('chosen@0', both, [])])),
    }
    for name, blob in refused_trees.items():
        assert initrd(blob) is None, name
    # Every truncation and every corrupted header word of a good tree is refused or read without leaving the blob
    # (the copy ends at an inaccessible page); a result, when there is one, comes from intact bytes.
    good = tree(both)
    total, off_struct, off_strings = struct.unpack_from('>III', good, 4)
    for cut in range(40, len(good), 7):
        clipped = bytearray(good[:cut])
        struct.pack_into('>I', clipped, 4, cut)
        assert initrd(bytes(clipped)) in (None, (0xAB800000, 0xB8000000)), cut
    for word in range(1, 10):
        for value in (0, 1, 39, 40, 41, len(good) - 1, len(good), 0x7FFFFFFF, 0xFFFFFFFF):
            if word == 1 and value > len(good):
                continue  # the total size is the only bound the reader has for memory it does not own
            damaged = bytearray(good)
            struct.pack_into('>I', damaged, 4 * word, value)
            assert initrd(bytes(damaged)) in (None, (0xAB800000, 0xB8000000)), (word, value)
    assert initrd(struct.pack('>10I', 0xD00DFEED, 0x4000001, 56, 56, 40, 17, 16, 0, 0, 0)) is None, 'larger than the limit'
    random.seed(20261110)
    for round_ in range(300):
        damaged = bytearray(good)
        for _ in range(random.randrange(1, 4)):
            damaged[random.randrange(off_struct, len(good))] = random.choice([0, 1, 2, 3, 4, 9, 0xFF, random.randrange(256)])
        initrd(bytes(damaged))  # must not read outside the blob

    # --- The type of a range ---
    def range_type(descriptors, start, end):
        blob = pack(descriptors)
        return lib.range_type(blob, len(blob), STRIDE, start, end)

    low = [(RESERVED, 0xA8000000, 0x10000000), (RESERVED, 0xB8000000, 0x02B00000), (FREE, 0xBAB00000, 0x0D1C0000),
           (BS_DATA, 0xA7000000, 0x00600000), (RT_DATA, 0xA7601000, 0x1000)]
    assert range_type(low, 0xAB800000, 0xB8000000) == RESERVED
    assert range_type(low, 0xAB800000, 0xB9000000) == RESERVED, 'two descriptors of one type'
    assert range_type(low, 0xB7FFF000, 0xBAB01000) == NONE, 'two types'
    assert range_type(low, 0xA7FFF000, 0xA8001000) == NONE, 'a part no descriptor covers'
    assert range_type(low, 0xA7601000, 0xA7602000) == RT_DATA
    assert range_type(low, 0x1000, 0x2000) == NONE and range_type(low, 0xA8000000, 0xA8000000) == NONE
    assert range_type(low, 0xAB800123, 0xAB800124) == RESERVED, 'a range inside one page'
    assert range_type([(FREE, 0x1000, MAX & ~0xfff)], 0x1000, 0x2000) == NONE, 'a descriptor that leaves the address space'

    # --- Records and release ---
    def describe(descriptors, base, window, release, inputs, slack=2 * STRIDE, stride=STRIDE):
        """Returns the records (base, size, type, state, usable) and the loader's copy of the map."""
        blob = pack(descriptors, stride)
        capacity = len(blob) + slack
        buffer = ctypes.create_string_buffer(blob + b'\xee' * slack, capacity)
        flat = (u64 * 6)(*[value for record in inputs for value in record])
        out = ctypes.create_string_buffer(b'\xee' * 66, 66)
        size = lib.describe(buffer, len(blob), capacity, stride, base, window, int(release), flat,
                            ctypes.cast(ctypes.addressof(out) + 1, ctypes.c_char_p))
        assert out.raw[0] == 0xee and out.raw[65] == 0xee, 'record overrun'
        assert len(blob) <= size <= capacity and (size - len(blob)) % stride == 0
        assert buffer.raw[:len(blob)] == blob, "the firmware's descriptors are never changed"
        assert buffer.raw[size:] == b'\xee' * (capacity - size), 'write behind the copy'
        return [struct.unpack_from('<QQIIQ', out.raw, 1 + 32 * index) for index in range(2)], buffer.raw[:size]

    def table_of(copy, base, window, capacity=64):
        excluded = u64()
        table = ctypes.create_string_buffer(16 * capacity)
        count = lib.exclusions(copy, len(copy), STRIDE, base, window, table, capacity, ctypes.byref(excluded))
        return [struct.unpack_from('<QQ', table.raw, 16 * index) for index in range(count)]

    # The sheng layout (test-memory-window.py) with the payload RAM disk where a first-stage loader of this
    # family puts it, at the top of the reserved load region of the low bank, and the initrd as a pool buffer in
    # boot services data at the top of bank 1.
    sheng = [
        (FREE, 0x8C0000000, 0x130000000),
        (BS_DATA, 0x9F0000000, 0x04000000), (FREE, 0x9F4000000, 0x03000000), (RT_DATA, 0x9F7000000, 0x00100000),
        (FREE, 0x9F7100000, 0x00F00000), (LOADER_DATA, 0x9F8000000, 0x02000000), (RT_CODE, 0x9FA000000, 0x00200000),
        (BS_CODE, 0x9FA200000, 0x05E00000),
        (FREE, 0x880000000, 0x20000000), (FREE, 0x8A0000000, 0x1B300000),
        (FREE, 0xBAB00000, 0x0D1C0000), (RESERVED, 0xA8000000, 0x10000000), (RESERVED, 0xB8000000, 0x02B00000),
        (FREE, 0xC7CC0000, 0x05040000), (FREE, 0xCCF80000, 0x0B080000), (FREE, 0xE671D000, 0x190E3000), (MMIO, 0x14680000, 0x2A000),
    ]
    base, window = 0x880004000, 0x180000000
    before = table_of(pack(sheng), base, window)
    assert before == [(0x8BB300000, 0x04D00000), (0x9F0000000, 0x04000000), (0x9F7000000, 0x00100000), (0x9FA000000, 0x06004000)]
    disk = (0xAB800000, 0x0C800000, LOCATED | VERIFIED)
    buffer_base, buffer_size = 0x9F1234018, 19828880  # a pool buffer: its head lies in front of it in the first page
    pool = (buffer_base, buffer_size, LOCATED | VERIFIED)
    first, last = (buffer_base + 0xfff) & ~0xfff, (buffer_base + buffer_size) & ~0xfff
    records, copy = describe(sheng, base, window, True, [disk, pool])
    # The RAM disk lies outside the window: recorded, nothing released for it.
    assert records[0] == (0xAB800000, 0x0C800000, RESERVED, LOCATED | VERIFIED, 0)
    assert records[1] == (buffer_base, buffer_size, BS_DATA, LOCATED | VERIFIED | INSIDE | USABLE_BIT, last - first)
    assert len(copy) == len(pack(sheng)) + STRIDE, 'one descriptor added'
    assert struct.unpack_from('<I4xQQQQ', copy, len(copy) - STRIDE) == (LOADER_DATA, first, 0, (last - first) >> 12, 0)
    after = table_of(copy, base, window)
    assert after == [(0x8BB300000, 0x04D00000), (0x9F0000000, first - 0x9F0000000), (last, 0x9F4000000 - last),
                     (0x9F7000000, 0x00100000), (0x9FA000000, 0x06004000)]
    assert sum(n for _, n in before) - sum(n for _, n in after) == last - first == 4840 * 4096
    # The partial pages at both ends of the buffer, where the pool's own head and tail lie, stay excluded.
    assert first > buffer_base and last < buffer_base + buffer_size
    # Without the table (a single-run window) the records are written and the copy is unchanged.
    records, copy = describe(sheng, base, window, False, [disk, pool])
    assert records[1] == (buffer_base, buffer_size, BS_DATA, LOCATED | VERIFIED | INSIDE, 0) and copy == pack(sheng)
    # An input that is located but not verified is never released.
    records, copy = describe(sheng, base, window, True, [disk, (buffer_base, buffer_size, LOCATED)])
    assert records[1] == (buffer_base, buffer_size, BS_DATA, LOCATED | INSIDE, 0) and copy == pack(sheng)
    # A state without the located bit, or stray bits, is cleared.
    records, copy = describe(sheng, base, window, True, [(0, 0, 0), (buffer_base, buffer_size, VERIFIED | INSIDE | USABLE_BIT)])
    assert records == [(0, 0, NONE, 0, 0), (buffer_base, buffer_size, NONE, 0, 0)] and copy == pack(sheng)
    # No room in the copy: the input stays excluded and says so.
    for slack in (0, STRIDE - 1):
        records, copy = describe(sheng, base, window, True, [disk, pool], slack=slack)
        assert records[1] == (buffer_base, buffer_size, BS_DATA, LOCATED | VERIFIED | INSIDE, 0) and copy == pack(sheng)
    # Descriptors of exactly the structure's size take the added one as well.
    records, copy = describe([(BS_DATA, 0x9F0000000, 0x04000000)], base, window, True, [disk, pool], stride=40, slack=40)
    assert records[1][3] & USABLE_BIT and len(copy) == 80 and struct.unpack_from('<I', copy, 40)[0] == LOADER_DATA
    # A RAM disk inside the window, in memory the platform reserved for it, is released whole; one that only
    # reaches into the window is released up to the window's edge.
    inside = [(FREE, 0x880000000, 0x20000000), (RESERVED, 0x8A0000000, 0x10000000), (FREE, 0x8B0000000, 0x0B300000),
              (FREE, 0x8C0000000, 0x140000000)]
    records, copy = describe(inside, base, window, True, [(0x8A3800000, 0x0C800000, LOCATED | VERIFIED), (0, 0, 0)])
    assert records[0] == (0x8A3800000, 0x0C800000, RESERVED, LOCATED | VERIFIED | INSIDE | USABLE_BIT, 0x0C800000)
    assert table_of(copy, base, window) == [(0x8A0000000, 0x03800000), (0x8BB300000, 0x04D00000), (0xA00000000, 0x4000)]
    records, copy = describe([(RESERVED, 0x87F000000, 0x02000000), (FREE, 0x881000000, 0x17F000000)], base, window, True,
                             [(0x87F800000, 0x01000000, LOCATED | VERIFIED), (0, 0, 0)])
    assert records[0] == (0x87F800000, 0x01000000, RESERVED, LOCATED | VERIFIED | USABLE_BIT, 0x880800000 - base)
    assert table_of(copy, base, window) == [(0x880800000, 0x00800000), (0xA00000000, 0x4000)]
    # Types with a meaning of their own are never released, verified or not; mixed types neither.
    for kind in (BS_CODE, RT_CODE, RT_DATA, ACPI_RECLAIM, ACPI_NVS, MMIO, 8, 12, 13, 14, 15, 0x80000001):
        records, copy = describe([(kind, 0x8A0000000, 0x10000000)], base, window, True, [(0x8A3800000, 0x0C800000, LOCATED | VERIFIED), (0, 0, 0)])
        assert records[0] == (0x8A3800000, 0x0C800000, kind, LOCATED | VERIFIED | INSIDE, 0) and len(copy) == STRIDE, kind
    records, copy = describe([(RESERVED, 0x8A0000000, 0x08000000), (BS_DATA, 0x8A8000000, 0x08000000)], base, window, True,
                             [(0x8A3800000, 0x0C800000, LOCATED | VERIFIED), (0, 0, 0)])
    assert records[0][2:] == (NONE, LOCATED | VERIFIED | INSIDE, 0) and len(copy) == 2 * STRIDE
    # Memory that is usable anyway needs no descriptor.
    records, copy = describe([(LOADER_DATA, 0x8A0000000, 0x10000000)], base, window, True, [(0x8A3800800, 0x0C000000, LOCATED | VERIFIED), (0, 0, 0)])
    assert records[0][2:] == (LOADER_DATA, LOCATED | VERIFIED | INSIDE | USABLE_BIT, 0x0C000000 - 0x1000) and len(copy) == STRIDE
    # Ranges that are smaller than a page, or that wrap, release nothing.
    for start, length in ((0x9F1234018, 0x800), (0x9F1234800, 0x1000), (MAX - 0x7ff, 0x400), (0x9F1234000, MAX - 0x100000000)):
        records, copy = describe(sheng, base, window, True, [(0, 0, 0), (start, length, LOCATED | VERIFIED)])
        assert not records[1][3] & USABLE_BIT and records[1][4] == 0 and copy == pack(sheng), hex(start)
    # The head check of the final map is not affected by a released input.
    blob = describe(sheng, base, window, True, [disk, pool])[1]
    assert lib.run_end(blob, len(blob), STRIDE, 0, base, base + 0x11000000) == base + 0x11000000

    # Random maps and inputs against a model: the table is the complement of the usable memory, where the whole
    # pages of each verified input of a releasable type are usable too, and no other byte ever becomes usable.
    released = 0
    for round_ in range(1200):
        descriptors, cursor = [], 0x80000000 + random.randrange(0, 64) * 0x1000
        for _ in range(random.randrange(1, 30)):
            cursor += random.choice([0, 0, 0, 0x1000, 0x200000, 0x4000000])
            length = random.choice([0x1000, 0x3000, 0x200000, 0x600000, 0x1000000, 0x8000000])
            descriptors.append((random.choice([FREE, FREE, LOADER_DATA, BS_CODE, BS_DATA, BS_DATA, RT_DATA, RESERVED, RESERVED, ACPI_NVS]), cursor, length))
            cursor += length
        end = cursor
        random.shuffle(descriptors)
        # The window starts near a descriptor more often than not, so that inputs fall inside it and across its edges.
        anchor = random.choice(descriptors)[1] & ~0xfff
        base = max(0x1000, anchor - random.choice([0, 0x1000, 0x800000, 0x10000000])) if random.random() < 0.8 else \
            0x80000000 + random.randrange(0, 0x8000) * 0x1000
        window = random.choice([0x10000000, 0x20000000, 0x40000000])
        inputs = []
        near = [entry for entry in descriptors if entry[1] < base + window and entry[1] + entry[2] > base]
        for _ in range(2):
            kind, start, length = random.choice(near if near and random.random() < 0.8 else descriptors)
            lead = random.choice([0, 0x18, 0x800, 0x1000, 0x1018])
            size = random.choice([0x10, 0x1000, 0x2345, length // 2, length // 2, length - min(lead, length - 1), length, length + 0x1000, 2 * length])
            state = random.choice([LOCATED | VERIFIED, LOCATED | VERIFIED, LOCATED | VERIFIED, LOCATED, 0, VERIFIED])
            inputs.append((start + min(lead, length - 1), max(size, 1), state))
        release = random.random() < 0.8
        records, copy = describe(descriptors, base, window, release, inputs)
        usable = [[a, a + n] for kind, a, n in descriptors if kind in USABLE]
        for (start, size, state), record in zip(inputs, records):
            if not state & LOCATED:
                assert record[2:] == (NONE, 0, 0)
                continue
            kind = reference_type(descriptors, start, start + size)
            lo, hi = max((start + 0xfff) & ~0xfff, base), min((start + size) & ~0xfff, base + window)
            give = release and state & VERIFIED and kind in RELEASABLE and lo < hi
            inside = base <= start and start + size <= base + window
            assert record == (start, size, kind, (state & 3) | (INSIDE if inside else 0) | (USABLE_BIT if give else 0), hi - lo if give else 0), round_
            if give:
                usable.append([lo, hi])
                released += kind not in USABLE
        assert table_of(copy, base, window) == reference_exclusions(usable, base, window), round_
    assert released > 100, released
    print(f'PASS: entry argument HOB, device tree initrd range ({len(refused_trees)} refusals, truncated and damaged trees), '
          f'range types, input records and release into the table, 1200 random maps against the model ({released} releases)')
