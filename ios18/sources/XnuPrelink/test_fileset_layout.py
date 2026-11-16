"""Reject malformed linker records and verify independently relocated references."""
import struct
from types import SimpleNamespace
import unittest

from fileset_layout import split_references, encode_uleb, relocate_native, relocate_original_stubs, Layout


def records(groups):
    result = bytearray(b'\x7f') + encode_uleb(len(groups))
    for source, target, targets in groups:
        for value in (source, target, len(targets)):
            result.extend(encode_uleb(value))
        for delta, kinds in targets:
            for value in (delta, len(kinds)):
                result.extend(encode_uleb(value))
            for kind, deltas in kinds:
                for value in (kind, len(deltas), *deltas):
                    result.extend(encode_uleb(value))
    return bytes(result)


def image_fixture(blob):
    return SimpleNamespace(data=struct.pack('<4I', 0x1e, 16, 16, len(blob))+blob,
                           command=lambda _: (0, 16), sections=[{}, {}])


class SplitSegmentTests(unittest.TestCase):
    def original_stub(self, target=0x8000, moved=0x20000, branch_target=None):
        stub = 0x1000
        actual = target if branch_target is None else branch_target
        blob = struct.pack('<2I', 0xd503245f, 0x14000000 | (((actual-stub-4)//4) & 0x3ffffff))
        native = SimpleNamespace(symbols=lambda: {stub: ['_constructor']},
                                 absolute_symbols=lambda: {'_constructor.original': target,
                                                           '_metaclass': 0x9000},
                                 read=lambda a, n: blob)
        return SimpleNamespace(native=native, cache=SimpleNamespace(base=0),
                               retained_code=[(0x8000, 0x9000, 'kept.kext')],
                               translate=lambda a: moved + a - stub)

    def test_absolute_original_branch_keeps_target_when_native_text_moves(self):
        from kc_image import signed
        for moved in (0x4000, 0x20000):
            output = bytearray(0x21000)
            records = relocate_original_stubs(output, self.original_stub(moved=moved))
            word, = struct.unpack_from('<I', output, moved + 4)
            self.assertEqual(moved + 4 + signed(word & 0x3ffffff, 26) * 4, 0x8000)
            self.assertEqual(records, [dict(symbol='_constructor', source=hex(moved+4),
                                            target='0x8000', owner='kept.kext')])

    def test_original_branch_rejects_stale_compensation_or_omitted_component(self):
        with self.assertRaisesRegex(ValueError, 'not retained executable'):
            relocate_original_stubs(bytearray(0x21000), self.original_stub(target=0x7000))
        layout = self.original_stub()
        layout.retained_code = []
        with self.assertRaisesRegex(ValueError, 'not retained executable'):
            relocate_original_stubs(bytearray(0x21000), layout)

    def test_original_branch_checks_declared_target_and_branch_range(self):
        with self.assertRaisesRegex(ValueError, 'disagrees'):
            relocate_original_stubs(bytearray(0x21000), self.original_stub(branch_target=0x8004))
        with self.assertRaisesRegex(ValueError, 'out of range'):
            relocate_original_stubs(bytearray(), self.original_stub(moved=0x9000000))

    def test_offsets_accumulate_per_target_but_reset_per_kind(self):
        blob = records([(1, 2, [(8, [(5, [4, 12]), (6, [8, 12])]),
                               (16, [(5, [32])])])])
        self.assertEqual(list(split_references(image_fixture(blob))),
                         [(5, 1, 4, 2, 8), (5, 1, 16, 2, 8),
                          (6, 1, 8, 2, 8), (6, 1, 20, 2, 8), (5, 1, 32, 2, 24)])

    def test_truncation_overflow_invalid_kind_and_section_are_rejected(self):
        valid = records([(1, 2, [(0, [(7, [0])])])])
        for blob in (valid[:-1], b'\x7f'+b'\xff'*10, b'\x01', valid+b'\x01',
                     records([(1, 3, [])]), records([(1, 2, [(0, [(14, [0])])])])):
            with self.subTest(blob=blob), self.assertRaises(ValueError):
                list(split_references(image_fixture(blob)))

    def test_section_ordinal_distinguishes_empty_marker_from_data(self):
        layout = Layout.__new__(Layout)
        layout.native = SimpleNamespace(base=0x10000, segments=[
            dict(name='__LAST', va=0x20000, size=0),
            dict(name='__DATA', va=0x20000, size=0x4000)], sections=[
            dict(segment='__LAST', va=0x20000, size=0),
            dict(segment='__DATA', va=0x20000, size=0x100)])
        layout.placements = {'__LAST': 0x80000, '__DATA': 0xa0000}
        self.assertEqual(layout.section_address(1, 0, True), 0x80000)
        self.assertEqual(layout.section_address(2, 0, True), 0xa0000)
        self.assertEqual(layout.translate(0x20000), 0xa0000)

    def test_native_branch_relocation_checks_old_target_and_signed_range(self):
        source, target = 0x10000, 0x20000
        blob = records([(1, 2, [(0, [(7, [0])])])])
        native = image_fixture(blob)
        native.sections = [dict(va=source), dict(va=target)]
        native.fixups = lambda: {}
        word = 0x94000000 | ((target-source)//4)
        native.read = lambda a, n: struct.pack('<I', word)
        layout = SimpleNamespace(native=native, cache=SimpleNamespace(base=0),
                                 section_address=lambda i, o, new=False:
                                 ([source, target] if not new else [0x100, 0x80])[i-1]+o)
        output = bytearray(0x200)
        pointers, counts = relocate_native(output, layout)
        self.assertEqual(struct.unpack_from('<I', output, 0x100)[0], 0x97ffffe0)
        self.assertEqual(counts['kind_7'], 1)
        self.assertEqual(pointers, {})
        word ^= 1
        with self.assertRaisesRegex(ValueError, 'disagrees'):
            relocate_native(output, layout)



class TextPlacementTests(unittest.TestCase):
    """Native __TEXT is placed by its sections and mapped as whole 4 KiB pages below the next retained segment."""
    def layout(self, section_end, next_kext):
        layout = object.__new__(Layout)
        layout.native = SimpleNamespace(
            segments=[{'name': '__TEXT', 'va': 0x7004000, 'size': 0x1dc000, 'fileoff': 0, 'filesize': 0x1dc000},
                      {'name': '__DATA', 'va': 0x9000000, 'size': 0x8000, 'fileoff': 0x200000, 'filesize': 0x8000}],
            sections=[{'segment': '__TEXT', 'name': '__const', 'va': 0x7005a40, 'size': 0x100},
                      {'segment': '__TEXT', 'name': '__eh_frame', 'va': 0x7004000 + section_end - 0x10, 'size': 0x10},
                      {'segment': '__DATA', 'name': '__data', 'va': 0x9000000, 'size': 0x10}])
        layout.roots = {'__PRELINK_TEXT': {'va': 0x100000, 'size': 0x400000}}
        layout.occupied = [(0x100000, 0x104000), (0x104000 + next_kext, 0x300000)]
        layout.placements, layout.mapped = {}, {}
        return layout

    def place(self, layout):
        used = layout.used('__TEXT')
        layout.reserve('__TEXT', layout.hole('__PRELINK_TEXT', used))
        room = layout.room('__PRELINK_TEXT', layout.placements['__TEXT'])
        layout.mapped = {'__TEXT': min(layout.segment('__TEXT')['size'], room // 0x1000 * 0x1000)}
        return used, room

    def test_sections_fit_where_the_padded_segment_does_not(self):
        layout = self.layout(section_end=0x1d80a8, next_kext=0x1db750)
        used, room = self.place(layout)
        self.assertEqual((used, room), (0x1d80a8, 0x1db750))
        self.assertEqual(layout.placements['__TEXT'], 0x104000)
        self.assertEqual(layout.span('__TEXT'), 0x1db000)          # whole pages, below the kext
        self.assertEqual(layout.end('__TEXT'), 0x104000 + 0x1db000)
        self.assertEqual(layout.span('__DATA'), 0x8000)            # other segments keep their size
        self.assertEqual(layout.used('__DATA'), 0x10)

    def test_a_segment_with_room_keeps_its_linked_size(self):
        layout = self.layout(section_end=0x1d80a8, next_kext=0x1e0000)
        self.place(layout)
        self.assertEqual(layout.span('__TEXT'), 0x1dc000)

    def test_sections_past_the_free_run_are_refused(self):
        layout = self.layout(section_end=0x1db800, next_kext=0x1db750)
        layout.roots['__PRELINK_TEXT']['size'] = 0x200000   # nothing free after the retained kext
        with self.assertRaisesRegex(ValueError, 'No room'):
            layout.hole('__PRELINK_TEXT', layout.used('__TEXT'))

if __name__ == '__main__':
    unittest.main()
