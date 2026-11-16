#!/usr/bin/env python3
"""Exercise the loader's table of free memory outside the window (MemoryWindow.h DarwinOutsideRanges) on synthetic maps."""
import ctypes
from pathlib import Path
import random
import struct
import subprocess
import tempfile

header = Path(__file__).resolve().parents[1] / 'DarwinBootPkg/Driver/MemoryWindow.h'
RESERVED, LOADER_CODE, LOADER_DATA, BS_CODE, BS_DATA, RT_CODE, RT_DATA, FREE = range(8)
STRIDE = 48
MIB = 1 << 20
ALIGN, MINIMUM = 64 << 10, MIB


def pack(descriptors):
    return b''.join(struct.pack('<I4xQQQQ8x', kind, start, 0, size >> 12, 0) for kind, start, size in descriptors)


def reference(descriptors, base, size, capacity):
    runs = []
    for start, end in sorted((s, s + n) for kind, s, n in descriptors if kind == FREE and n):
        if runs and start <= runs[-1][1]:
            runs[-1][1] = max(runs[-1][1], end)
        else:
            runs.append([start, end])
    pieces = []
    for start, end in runs:
        for a, b in ((start, min(end, base)), (max(start, base + size), end)):
            a, b = (a + ALIGN - 1) & ~(ALIGN - 1), b & ~(ALIGN - 1)
            if a < b and b - a >= MINIMUM:
                pieces.append((a, b - a))
    kept = []
    for piece in pieces:  # the loader keeps the largest, an equal one does not replace an earlier one
        if len(kept) < capacity:
            kept.append(piece)
        else:
            least = min(range(capacity), key=lambda i: kept[i][1])
            if piece[1] > kept[least][1]:
                kept[least] = piece
    return sorted(kept)


with tempfile.TemporaryDirectory(prefix='darwinboot-outside-') as temporary:
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
size_t outside(const void *map, size_t size, size_t stride, UINT64 base, UINT64 window, UINT8 *table, size_t capacity, UINT64 *bytes) {
  return DarwinOutsideRanges(map, size, stride, base, window, 0x10000, 0x100000, table, capacity, bytes);
}
UINT64 unused_start(const void *m, size_t s, size_t t) { UINT64 u; return DarwinWindowStart(m, s, t, 0x200000, 0, 1, 1, MAX_UINT64, 0, &u) + DarwinWindowUsable(m, s, t, 0, 0) + DARWIN_WINDOW_RESERVE(0); }
size_t unused_exclusions(const void *m, size_t s, size_t t, UINT8 *table, UINT64 *e) { return DarwinWindowExclusions(m, s, t, 0, 0, table, 0, e); }
''')
    library = work / 'outside.dylib'
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    '-shared', '-fPIC', '-I', str(header.parent), str(work / 'test.c'), '-o', str(library)], check=True)
    lib = ctypes.CDLL(str(library))
    u64, size_t = ctypes.c_uint64, ctypes.c_size_t
    lib.outside.restype = size_t
    lib.outside.argtypes = [ctypes.c_char_p, size_t, size_t, u64, u64, ctypes.c_char_p, size_t, ctypes.POINTER(u64)]

    def run(descriptors, base, size, capacity):
        table = ctypes.create_string_buffer(b'\xff' * (16 * capacity + 1), 16 * capacity + 1)
        total = u64(0)
        blob = pack(descriptors)
        count = lib.outside(blob, len(blob), STRIDE, base, size, ctypes.cast(ctypes.byref(table, 1), ctypes.c_char_p), capacity, ctypes.byref(total))
        pairs = [struct.unpack_from('<QQ', table.raw, 1 + 16 * i) for i in range(capacity)]
        assert all(pair == (0, 0) for pair in pairs[count:]), 'unused pairs are zeroed'
        assert total.value == sum(length for _, length in pairs[:count])
        return pairs[:count]

    # sheng: the low bank's free pieces, the window in bank 1, firmware memory in between.
    sheng = [(FREE, 0xBAB00000, 0x0D1C0000), (FREE, 0xC7CC0000, 0x05040000), (RESERVED, 0xCCD00000, 0x280000),
             (FREE, 0xCCF80000, 0x0B080000), (BS_DATA, 0xD8000000, 0xE71D000), (FREE, 0xE671D000, 0x190E3000),
             (FREE, 0x880000000, 0x3B300000), (FREE, 0x8C0000000, 0x130000000), (BS_DATA, 0x9F0000000, 0x10000000)]
    got = run(sheng, 0x880004000, 0x180000000, 16)
    assert got == [(0xBAB00000, 0x12200000), (0xCCF80000, 0x0B080000), (0xE6720000, 0x190E0000)], [(hex(a), hex(b)) for a, b in got]
    assert run(sheng, 0x880004000, 0x180000000, 2) == [(0xBAB00000, 0x12200000), (0xE6720000, 0x190E0000)]
    # Free memory inside the window is never listed; the part of a run below the window is.
    assert run([(FREE, 0x1000000, 0x4000000)], 0x2000000, 0x1000000, 4) == [(0x1000000, 0x1000000), (0x3000000, 0x2000000)]
    assert run([(LOADER_DATA, 0x1000000, 0x4000000), (BS_DATA, 0x8000000, 0x4000000)], 0x40000000, 0x1000000, 4) == []

    random.seed(20261111)
    for _ in range(600):
        descriptors, cursor = [], random.randrange(0, 64) * 0x1000
        for _ in range(random.randrange(1, 24)):
            cursor += random.choice((0, 0, 0x1000, 0x40000, 3 * MIB)) * random.randrange(0, 3)
            length = random.choice((0x1000, 0x10000, 0x80000, MIB, 5 * MIB, 37 * MIB)) * random.randrange(1, 4)
            descriptors.append((random.choice((FREE, FREE, FREE, LOADER_DATA, BS_DATA, RESERVED)), cursor, length))
            cursor += length
        random.shuffle(descriptors)
        base = random.randrange(0, cursor + MIB, 0x1000)
        size = random.randrange(0x1000, cursor + MIB, 0x1000)
        capacity = random.randrange(1, 9)
        assert run(descriptors, base, size, capacity) == reference(descriptors, base, size, capacity), (descriptors, base, size, capacity)
print('PASS: free memory outside the window: sheng layout, clipping at the window, table overflow keeps the largest, 600 random maps')
