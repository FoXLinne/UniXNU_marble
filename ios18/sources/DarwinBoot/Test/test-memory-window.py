#!/usr/bin/env python3
"""Exercise the loader's window selection and excluded-range table (MemoryWindow.h) on synthetic UEFI memory maps."""
import ctypes
from pathlib import Path
import random
import struct
import subprocess
import tempfile

header = Path(__file__).resolve().parents[1] / 'DarwinBootPkg/Driver/MemoryWindow.h'
RESERVED, LOADER_CODE, LOADER_DATA, BS_CODE, BS_DATA, RT_CODE, RT_DATA, FREE = range(8)
ACPI_NVS, MMIO = 10, 11
USABLE = {FREE, LOADER_CODE, LOADER_DATA}
STRIDE = 48  # Firmware descriptors are larger than the structure; the loader must step by the reported size.
MIB, GIB = 1 << 20, 1 << 30
BLOCK = 2 * MIB
MAX = (1 << 64) - 1


def pack(descriptors):
    return b''.join(struct.pack('<I4xQQQQ8x', kind, start, 0, size >> 12, 0) for kind, start, size in descriptors)


def merged(ranges):
    out = []
    for start, end in sorted(ranges):
        if out and start <= out[-1][1]:
            out[-1][1] = max(out[-1][1], end)
        else:
            out.append([start, end])
    return out


def usable_ranges(descriptors, kinds=USABLE):
    return merged([start, start + size] for kind, start, size in descriptors if kind in kinds and size)


def reference_exclusions(descriptors, base, size):
    """The complement of the usable memory inside the window, as (base, size) pairs."""
    out, cursor = [], base
    for start, end in usable_ranges(descriptors):
        start, end = max(start, base), min(end, base + size)
        if start >= end:
            continue
        if start > cursor:
            out.append((cursor, start - cursor))
        cursor = max(cursor, end)
    if cursor < base + size:
        out.append((cursor, base + size - cursor))
    return out


def reference_start(descriptors, offset, head, size):
    best = None
    free = usable_ranges(descriptors, {FREE})
    for kind, start, length in descriptors:
        if kind != FREE or not length:
            continue
        candidate = ((start + BLOCK - 1) & ~(BLOCK - 1)) + offset
        if candidate >= start + length or not any(a <= candidate and candidate + head <= b for a, b in free):
            continue
        usable = size - sum(length for _, length in reference_exclusions(descriptors, candidate, size))
        if best is None or (usable, -candidate) > (best[1], -best[0]):
            best = (candidate, usable)
    return best or (0, 0)


with tempfile.TemporaryDirectory(prefix='darwinboot-window-') as temporary:
    work = Path(temporary)
    (work / 'test.c').write_text('''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t UINT8; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef int BOOLEAN;
#define STATIC static
#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0
#define MAX_UINT64 UINT64_MAX
#define EFI_PAGE_SIZE 4096
enum { EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode, EfiBootServicesData,
       EfiRuntimeServicesCode, EfiRuntimeServicesData, EfiConventionalMemory };
typedef struct { UINT32 Type; UINT64 PhysicalStart; UINT64 VirtualStart; UINT64 NumberOfPages; UINT64 Attribute; } EFI_MEMORY_DESCRIPTOR;
static UINT64 ReadUnaligned64(const UINT64 *p) { UINT64 v; memcpy(&v, p, 8); return v; }
static void WriteUnaligned64(UINT64 *p, UINT64 v) { memcpy(p, &v, 8); }
#include "MemoryWindow.h"
UINT64 start(const void *map, size_t size, size_t stride, UINT64 offset, UINT64 head, UINT64 window,
             UINT64 skip_usable, UINT64 skip_address, UINT64 *usable) {
  return DarwinWindowStart(map, size, stride, 0x200000, offset, head, window, skip_usable, skip_address, usable);
}
size_t exclusions(const void *map, size_t size, size_t stride, UINT64 base, UINT64 window, UINT8 *table,
                  size_t capacity, UINT64 *excluded) {
  return DarwinWindowExclusions(map, size, stride, base, window, table, capacity, excluded);
}
UINT64 run_end(const void *map, size_t size, size_t stride, int free_only, UINT64 address, UINT64 limit) {
  return DarwinRunEnd(map, size, stride, free_only, address, limit);
}
UINT64 reserve(UINT64 window) { return DARWIN_WINDOW_RESERVE(window); }
size_t outside(const void *map, size_t size, size_t stride, UINT8 *table, UINT64 *bytes) {  /* tested in test-memory-outside.py */
  return DarwinOutsideRanges(map, size, stride, 0, 0, 0x10000, 0x100000, table, 0, bytes);
}
''')
    library = work / 'window.dylib'
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    '-shared', '-fPIC', '-I', str(header.parent), str(work / 'test.c'), '-o', str(library)], check=True)
    lib = ctypes.CDLL(str(library))
    u64, size_t = ctypes.c_uint64, ctypes.c_size_t
    lib.start.restype = u64
    lib.start.argtypes = [ctypes.c_char_p, size_t, size_t, u64, u64, u64, u64, u64, ctypes.POINTER(u64)]
    lib.exclusions.restype = size_t
    lib.exclusions.argtypes = [ctypes.c_char_p, size_t, size_t, u64, u64, ctypes.c_char_p, size_t, ctypes.POINTER(u64)]
    lib.run_end.restype = u64
    lib.run_end.argtypes = [ctypes.c_char_p, size_t, size_t, ctypes.c_int, u64, u64]
    lib.reserve.restype = u64
    lib.reserve.argtypes = [u64]

    def start(descriptors, offset, head, size, skip=(MAX, 0)):
        """(address, usable bytes); skip is the (usable bytes, address) of the start to rank after."""
        blob, usable = pack(descriptors), u64()
        address = lib.start(blob, len(blob), STRIDE, offset, head, size, skip[0], skip[1], ctypes.byref(usable))
        return address, usable.value

    def exclusions(descriptors, base, size, capacity):
        blob, excluded = pack(descriptors), u64()
        # One byte in front: the table sits unaligned, as a device tree property does.
        table = ctypes.create_string_buffer(b'\xee' * (16 * capacity + 2), 16 * capacity + 2)
        pointer = ctypes.cast(ctypes.addressof(table) + 1, ctypes.c_char_p)
        count = lib.exclusions(blob, len(blob), STRIDE, base, size, pointer, capacity, ctypes.byref(excluded))
        raw = table.raw
        assert raw[0] == 0xee and raw[-1] == 0xee, 'table overrun'
        pairs = [struct.unpack_from('<QQ', raw, 1 + 16 * index) for index in range(capacity)]
        assert all(pair == (0, 0) for pair in pairs[count:]), 'unused slots must be zero'
        assert excluded.value == sum(length for _, length in pairs[:count])
        return pairs[:count]

    def run_end(descriptors, free_only, address, limit):
        blob = pack(descriptors)
        return lib.run_end(blob, len(blob), STRIDE, int(free_only), address, limit)

    assert lib.reserve(0x10000000) == 32 * MIB and lib.reserve(0x180000000) == 96 * MIB

    # The sheng layout: free pieces of the low bank, bank 1 with an inaccessible gap that no descriptor covers,
    # and firmware allocations at the top of the high part. The map is deliberately out of address order.
    sheng = [
        (FREE, 0x8C0000000, 0x130000000),
        (BS_DATA, 0x9F0000000, 0x04000000), (FREE, 0x9F4000000, 0x03000000), (RT_DATA, 0x9F7000000, 0x00100000),
        (FREE, 0x9F7100000, 0x00F00000), (LOADER_DATA, 0x9F8000000, 0x02000000), (RT_CODE, 0x9FA000000, 0x00200000),
        (BS_CODE, 0x9FA200000, 0x05E00000),
        (FREE, 0x880000000, 0x20000000), (FREE, 0x8A0000000, 0x1B300000),
        (FREE, 0xBAB00000, 0x0D1C0000), (RESERVED, 0xB8000000, 0x02B00000), (FREE, 0xC7CC0000, 0x05040000),
        (FREE, 0xCCF80000, 0x0B080000), (FREE, 0xE671D000, 0x190E3000), (MMIO, 0x14680000, 0x2A000),
    ]
    offset, head, window = 0x4000, 0x11000000, 0x180000000
    address, usable = start(sheng, offset, head, window)
    assert address == 0x880004000, hex(address)
    table = exclusions(sheng, address, window, 64)
    assert table == [(0x8BB300000, 0x04D00000), (0x9F0000000, 0x04000000), (0x9F7000000, 0x00100000),
                     (0x9FA000000, 0x06004000)], [(hex(a), hex(b)) for a, b in table]
    assert table == reference_exclusions(sheng, address, window)
    assert usable == window - sum(length for _, length in table) == (0x3B300000 - 0x4000) + 0x130000000 + 0x03000000 + 0x00F00000 + 0x02000000
    assert run_end(sheng, False, address, address + head) == address + head
    # The starts in rank: the second descriptor of the same run, the run behind the gap, the two runs of the
    # low bank that hold the head (its first two pieces are adjacent), then none.
    ranked, skip = [], (usable, address)
    while (found := start(sheng, offset, head, window, skip=skip))[0]:
        assert found[1] <= skip[0]
        ranked.append(found[0])
        skip = (found[1], found[0])
    assert ranked == [0x8A0004000, 0x8C0004000, 0xBAC04000, 0xE6804000], [hex(value) for value in ranked]
    # A head larger than the run in front of the gap moves the window behind the gap.
    assert start(sheng, offset, 0x3C000000, window)[0] == 0x8C0004000
    # Nothing holds the head: no start, and a head larger than the window is refused outright.
    assert start(sheng, offset, 0x140000000, window) == (0, 0)
    assert start(sheng, offset, window + 0x1000, window) == (0, 0)
    # A firmware allocation inside the head is seen by the run check that guards the final map.
    broken = [entry for entry in sheng if entry[1] != 0x880000000] + [
        (FREE, 0x880000000, 0x00800000), (RT_DATA, 0x880800000, 0x1000), (FREE, 0x880801000, 0x1F7FF000)]
    assert run_end(broken, False, address, address + head) == 0x880800000
    # Boot services memory stays excluded; loader memory is usable but is not a free run.
    assert run_end([(BS_DATA, 0x1000, 0x1000)], False, 0x1000, 0x3000) == 0x1000
    assert run_end([(LOADER_DATA, 0x1000, 0x1000)], False, 0x1000, 0x3000) == 0x2000
    assert run_end([(LOADER_DATA, 0x1000, 0x1000)], True, 0x1000, 0x3000) == 0x1000
    # A descriptor whose size leaves the address space is ignored.
    assert run_end([(FREE, 0x1000, MAX & ~0xfff)], False, 0x1000, 0x3000) == 0x1000

    # A table smaller than the number of ranges joins the neighbours with the least usable memory between them.
    for capacity in (3, 2, 1):
        small = exclusions(sheng, address, window, capacity)
        assert len(small) == capacity
        for a, b in zip(small, small[1:]):
            assert a[0] + a[1] < b[0]
        for base, length in table:
            assert any(a <= base and base + length <= a + b for a, b in small), 'an excluded range was left out'
    # 47 MiB lie between the last two ranges and 48 MiB between the two before them: the last two are joined.
    assert exclusions(sheng, address, window, 3) == [(0x8BB300000, 0x04D00000), (0x9F0000000, 0x04000000), (0x9F7000000, 0x09004000)]
    assert exclusions(sheng, address, window, 1) == [(0x8BB300000, 0xA00004000 - 0x8BB300000)]

    # Random maps against the reference model.
    random.seed(20261110)
    for round_ in range(400):
        descriptors, cursor = [], 0x80000000 + random.randrange(0, 64) * 0x1000
        for _ in range(random.randrange(1, 40)):
            cursor += random.choice([0, 0, 0, 0x1000, 0x200000, 0x4000000])
            length = random.choice([0x1000, 0x3000, 0x200000, 0x600000, 0x1000000, 0x8000000])
            descriptors.append((random.choice([FREE, FREE, FREE, LOADER_CODE, LOADER_DATA, BS_CODE, BS_DATA, RT_DATA, RESERVED, ACPI_NVS]),
                                cursor, length))
            cursor += length
        random.shuffle(descriptors)
        size = random.choice([0x10000000, 0x20000000, 0x40000000])
        offset, head = random.choice([0, 0x4000, 0x1ff000]), random.choice([0x1000, 0x200000, 0x2000000])
        got = start(descriptors, offset, head, size)
        assert got == reference_start(descriptors, offset, head, size), (round_, hex(got[0]))
        if not got[0]:
            continue
        expected = reference_exclusions(descriptors, got[0], size)
        assert exclusions(descriptors, got[0], size, 64) == expected, round_
        assert got[1] == size - sum(length for _, length in expected)
        capacity = random.randrange(1, 5)
        small = exclusions(descriptors, got[0], size, capacity)
        assert len(small) == min(capacity, len(expected))
        assert all(got[0] <= base and base + length <= got[0] + size for base, length in small)
        assert all(a[0] + a[1] < b[0] for a, b in zip(small, small[1:]))
        assert all(any(a <= base and base + length <= a + b for a, b in small) for base, length in expected)
        # The head is never given up: joining only ever spans from one excluded range to the next.
        if expected:
            assert small[0][0] == expected[0][0]
    print('PASS: window start by usable memory, excluded-range table, table overflow, 400 random maps against the model')
