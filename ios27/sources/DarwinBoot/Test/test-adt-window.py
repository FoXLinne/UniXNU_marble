#!/usr/bin/env python3
"""Compile the production device tree parser and check the window properties of /chosen."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile

driver = Path(__file__).resolve().parents[1] / 'DarwinBootPkg/Driver'
source = (driver / 'DarwinBoot.c').read_text()
parser = source[source.index('STATIC\nBOOLEAN\nAdtNode ('):source.index('STATIC\nBOOLEAN\nValidTrustCaches')]
preamble = '''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "WindowSize.h"
#define DARWIN_INPUT_COUNT 2
#define DARWIN_INPUT_RECORD_SIZE 32
typedef uint8_t UINT8; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef int BOOLEAN; typedef char CHAR8;
#define STATIC static
#define CONST const
#define TRUE 1
#define FALSE 0
#define MAX_UINTN SIZE_MAX
#define ALIGN_VALUE(a,b) (((a)+(b)-1)&~((b)-1))
#define AsciiStrCmp strcmp
#define CompareMem memcmp
static const void *ScanMem8(const void *p, size_t n, int v) { return memchr(p, v, n); }
static BOOLEAN IsZeroBuffer(const void *p, size_t n) { const UINT8 *b = p; while (n--) { if (*b++) return FALSE; } return TRUE; }
static UINT32 U32(const void *p) { UINT32 v; memcpy(&v, p, 4); return v; }
static UINT64 U64(const void *p) { UINT64 v; memcpy(&v, p, 8); return v; }
'''
entry = '''
int parse(const UINT8 *tree, UINTN size, UINT64 *window, UINTN *excluded, UINTN *capacity, UINTN *inputs) {
  UINTN at = 0, ramdisk = MAX_UINTN, trust = MAX_UINTN, addon = MAX_UINTN, policy = MAX_UINTN;
  BOOLEAN packed = FALSE;
  *window = 0; *excluded = MAX_UINTN; *capacity = 0; *inputs = MAX_UINTN;
  return AdtNode(tree, size, &at, 0, 3, &ramdisk, &trust, &addon, &policy, window, &packed, excluded, capacity, inputs) && at == size;
}
'''


def node(name, properties=(), children=()):
    props = [('name', name.encode() + b'\0'), *properties]
    data = struct.pack('<II', len(props), len(children))
    for key, value in props:
        data += struct.pack('<32sI', key.encode(), len(value)) + value + bytes(-len(value) % 4)
    return data + b''.join(children)


with tempfile.TemporaryDirectory(prefix='darwinboot-adt-') as temporary:
    work = Path(temporary)
    (work / 'parser.c').write_text(preamble + parser + entry)
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all', '-shared', '-fPIC',
                    '-I', str(driver), str(work / 'parser.c'), '-o', str(work / 'parser.dylib')], check=True)
    lib = ctypes.CDLL(str(work / 'parser.dylib'))
    lib.parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint64),
                          ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]

    def parse_all(tree):
        window, excluded, capacity, inputs = ctypes.c_uint64(), ctypes.c_size_t(), ctypes.c_size_t(), ctypes.c_size_t()
        ok = lib.parse(tree, len(tree), ctypes.byref(window), ctypes.byref(excluded), ctypes.byref(capacity), ctypes.byref(inputs))
        return bool(ok), window.value, excluded.value, capacity.value, inputs.value

    def parse(tree):
        return parse_all(tree)[:4]

    none = ctypes.c_size_t(-1).value
    size = ('darwinboot-memory-size', struct.pack('<Q', 0x180000000))

    def chosen(*properties):
        return node('device-tree', children=[node('chosen', properties, [node('memory-map')])])

    # A tree without the table keeps the single-run window.
    assert parse(chosen(size)) == (True, 0x180000000, none, 0)
    assert parse(chosen()) == (True, 0, none, 0)
    for slots in (1, 2, 64, 300):
        tree = chosen(size, ('darwinboot-memory-excluded', bytes(16 * slots)))
        ok, window, offset, capacity = parse(tree)
        assert (ok, window, capacity) == (True, 0x180000000, slots)
        # The offset is where the loader writes: the property's value, inside the tree.
        assert tree[offset - 36:offset - 36 + 27] == b'darwinboot-memory-excluded\0'
        assert struct.unpack_from('<I', tree, offset - 4)[0] == 16 * slots and offset + 16 * slots <= len(tree)
    # The table works without a size request (the default window), in either property order.
    assert parse(chosen(('darwinboot-memory-excluded', bytes(32))))[::3] == (True, 2)
    assert parse(chosen(('darwinboot-memory-excluded', bytes(32)), size))[::3] == (True, 2)
    refused = {
        'no pair': bytes(0), 'half a pair': bytes(8), 'a pair and a half': bytes(24), 'odd length': bytes(17),
        'stale contents': bytes(16) + struct.pack('<QQ', 0x8BB300000, 0x4D00000),
        'stale last byte': bytes(31) + b'\x01',
    }
    for name, value in refused.items():
        assert not parse(chosen(size, ('darwinboot-memory-excluded', value)))[0], name
    table = ('darwinboot-memory-excluded', bytes(32))
    assert not parse(chosen(size, table, table))[0], 'duplicate'
    assert not parse(node('device-tree', [table], [node('chosen', [size])]))[0], 'table in the root node'
    assert not parse(node('device-tree', children=[node('chosen', [size], [node('memory-map', [table])])]))[0], 'table in memory-map'
    assert not parse(node('device-tree', children=[node('chosen', [size, table]), node('chosen', [table])]))[0], 'second chosen node'

    # The record of the loader's inputs: two empty 32-byte records in /chosen, with or without the table.
    record = ('darwinboot-loader-inputs', bytes(64))
    assert parse_all(chosen(size))[4] == none
    for tree in (chosen(record), chosen(size, table, record), chosen(record, size, table)):
        ok, _, _, _, offset = parse_all(tree)
        assert ok and tree[offset - 36:offset - 36 + 25] == b'darwinboot-loader-inputs\0'
        assert struct.unpack_from('<I', tree, offset - 4)[0] == 64 and offset + 64 <= len(tree)
    assert parse_all(chosen(size, table, record))[:4] == parse(chosen(size, table)), 'the record changes no other result'
    refused_records = {
        'one record': bytes(32), 'three records': bytes(96), 'empty': bytes(0), 'odd length': bytes(63),
        'stale contents': bytes(8) + struct.pack('<Q', 0xC800000) + bytes(48), 'stale last byte': bytes(63) + b'\x01',
    }
    for name, value in refused_records.items():
        assert not parse_all(chosen(size, ('darwinboot-loader-inputs', value)))[0], name
    assert not parse_all(chosen(size, record, record))[0], 'duplicate record'
    assert not parse_all(node('device-tree', [record], [node('chosen', [size])]))[0], 'record in the root node'
    assert not parse_all(node('device-tree', children=[node('chosen', [size], [node('memory-map', [record])])]))[0], 'record in memory-map'
    print(f'PASS: window size, excluded-range table and loader input record of /chosen, '
          f'{len(refused) + 4 + len(refused_records) + 3} refusal cases')
