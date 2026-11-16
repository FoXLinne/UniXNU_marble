"""Require table identity and the full PAC layout before matching virtual methods."""
from types import SimpleNamespace
import unittest

from fileset_match import Matcher


def side(base, targets, diversities, *, extra=()):
    fixups = {base + 16 + 8 * i: (target, 1, 0, diversity, 1)
              for i, (target, diversity) in enumerate(zip(targets, diversities))}
    for other in extra:
        fixups.update({other + 16 + 8 * i: value for i, value in enumerate(fixups.values())})
    def read(address, length):
        if address in (base, *extra) and length == 16:
            return bytes(16)
        raise ValueError('not backed')
    return SimpleNamespace(fixups=fixups, image=SimpleNamespace(read=read),
                           symbol_names={base: ['__ZTV7Example']},
                           in_exec=lambda address: address in targets,
                           in_mapped=lambda address: base <= address < base + 0x800)


class VtableMatchingTests(unittest.TestCase):
    def test_named_table_matches_slots_with_duplicate_global_discriminators(self):
        matcher = Matcher(side(0x1000, [0x9000, 0x9010], [5, 6]),
                          side(0x2000, [0xa000, 0xa010], [5, 6]))
        self.assertEqual(matcher.anchor_vtables({0x1000: (0x2000, 'refs')}), 2)
        self.assertEqual(matcher.pairs, {0x9000: 0xa000, 0x9010: 0xa010})
        self.assertEqual(matcher.anchor_vtables({0x1000: (0x2000, 'refs')}), 0)

    def test_matching_prefix_does_not_accept_changed_slot_or_extent(self):
        for targets, diversities in (([0xa000, 0xa010], [5, 7]),
                                     ([0xa000, 0xa010, 0xa020], [5, 6, 7])):
            matcher = Matcher(side(0x1000, [0x9000, 0x9010], [5, 6]),
                              side(0x2000, targets, diversities))
            self.assertEqual(matcher.anchor_vtables({0x1000: (0x2000, 'refs')}), 0)
            self.assertFalse(matcher.pairs)

    def test_table_location_requires_two_distinct_methods(self):
        matcher = Matcher(side(0x1000, [0x9000, 0x9010], [5, 6]),
                          side(0x2000, [0xa000, 0xa010], [5, 6]))
        known = {}
        matcher.bind(0x9000, 0xa000, 'shape')
        self.assertEqual(matcher.pair_vtables(known), 0)
        matcher.bind(0x9010, 0xa010, 'shape')
        self.assertEqual(matcher.pair_vtables(known), 1)
        self.assertEqual(known, {0x1000: (0x2000, 'vtable-layout')})

    def test_shared_inherited_methods_do_not_choose_between_identical_tables(self):
        matcher = Matcher(side(0x1000, [0x9000, 0x9010], [5, 6], extra=(0x1100,)),
                          side(0x2000, [0xa000, 0xa010], [5, 6]))
        matcher.bind(0x9000, 0xa000, 'shape')
        matcher.bind(0x9010, 0xa010, 'shape')
        self.assertEqual(matcher.pair_vtables({}), 0)


if __name__ == '__main__':
    unittest.main()

class FunctionMatchingTests(unittest.TestCase):
    def make_side(self, functions, names=None):
        from collections import defaultdict
        return SimpleNamespace(functions=functions, symbol_names=names or {},
                               string_owners={}, named_functions={}, callees=defaultdict(set))

    def test_unique_shape_means_unique_across_all_functions(self):
        fn = {'digest': 'same', 'length': 4}
        matcher = Matcher(self.make_side({1: fn, 2: fn}), self.make_side({10: fn, 20: fn}))
        matcher.bind(1, 10, 'vtable')
        self.assertEqual(matcher.anchor_shapes(), 0)
        self.assertNotIn(2, matcher.pairs)

    def test_cold_string_anchors_parent_and_cannot_bind_nonmatching_body(self):
        a = self.make_side({1: {'digest': 'body', 'length': 50}})
        b = self.make_side({10: {'digest': 'parent', 'length': 45}, 20: {'digest': 'panic', 'length': 8}},
                           {10: ['_example'], 20: ['_example.cold.1']})
        a.string_owners = {'unique panic': {1}}
        b.string_owners = {'unique panic': {20}}
        b.named_functions = {'_example': 10, '_example.cold.1': 20}
        b.callees[10].add(20)
        matcher = Matcher(a, b)
        self.assertFalse(matcher.bind(1, 20, 'calls'))
        self.assertEqual(matcher.anchor_strings(), 1)
        self.assertEqual(matcher.pairs, {1: 10})

    def test_adrp_provenance_ends_at_register_overwrite(self):
        import struct
        from kc_image import scan
        # adrp x8, page; ldr x8, [x8,#16]; ldr x0,[x8,#24].
        code = struct.pack('<3I', 0x90000008, 0xf9400908, 0xf9400d00)
        self.assertEqual(scan(code, 0x10000)[1], [(0x10004, 0x10010, 'ldst')])
        # mov x8,x0 between the page and the load invalidates it as well.
        code = struct.pack('<3I', 0x90000008, 0xaa0003e8, 0xf9400d00)
        self.assertEqual(scan(code, 0x10000)[1], [])

    def test_internal_branch_geometry_is_not_erased_from_function_shape(self):
        import struct
        from kc_image import scan
        first = struct.pack('<3I', 0x14000001, 0xd503201f, 0xd65f03c0)
        second = struct.pack('<3I', 0x14000002, 0xd503201f, 0xd65f03c0)
        self.assertNotEqual(scan(first, 0x10000)[2], scan(second, 0x20000)[2])

    def test_repeated_local_symbol_names_remain_distinct_graph_nodes(self):
        matcher = Matcher(self.make_side({}), self.make_side({}, {10: ['_helper'],20: ['_helper']}))
        self.assertNotEqual(matcher.canon(10),matcher.canon(20))

    def test_reference_transfers_code_identity_to_actual_native_symbol(self):
        original = self.make_side({1: {'digest':'a'}})
        native = self.make_side({20: {'digest':'b'}})
        native.named_functions = {'_named_api':20}
        reference = SimpleNamespace(symbol_names={10:['_named_api']},named_functions={'_named_api':10})
        matcher = Matcher(original,native)
        pairing = {'functions':{'0x1':{'native':'0xa','how':'pac-checked-shape'}}}
        self.assertEqual(matcher.anchor_reference(reference,pairing),1)
        self.assertEqual(matcher.pairs,{1:20})

    def test_reference_rejects_weak_guesses_and_split_aliases(self):
        original,native = self.make_side({1:{}}),self.make_side({20:{},30:{}})
        native.named_functions = {'_api':20,'_alias':30}
        reference = SimpleNamespace(symbol_names={10:['_api','_alias']},
                                    named_functions={'_api':10,'_alias':10})
        for how in ('calls','string','pac-checked-shape'):
            matcher=Matcher(original,native)
            pairing={'functions':{'0x1':{'native':'0xa','how':how}}}
            self.assertEqual(matcher.anchor_reference(reference,pairing),0)

    def test_reference_does_not_transfer_compiler_generated_names(self):
        original,native = self.make_side({1:{}}),self.make_side({20:{}})
        native.named_functions = {'_OUTLINED_FUNCTION_0':20}
        reference = SimpleNamespace(symbol_names={10:['_OUTLINED_FUNCTION_0']},
                                    named_functions={'_OUTLINED_FUNCTION_0':10})
        matcher = Matcher(original,native)
        self.assertEqual(matcher.anchor_reference(reference, {'functions':{
            '0x1':{'native':'0xa','how':'pac-checked-shape'}}}),0)

class MetaclassIdentityTests(unittest.TestCase):
    def make_matcher(self):
        a = side(0x1000,[0x9000,0x9010],[5,6])
        b = side(0x2000,[0xa000,0xa010],[5,6])
        a.functions = {0x9010: {'digest':'getter','refs':[(8,0x3000)]}}
        b.functions = {0xa010: {'digest':'getter','refs':[(8,0x4000)]}}
        b.symbol_names.update({0xa010:['__ZNK7Example12getMetaClassEv'],
                               0x4000:['__ZN7Example10gMetaClassE']})
        matcher = Matcher(a,b)
        matcher.bind(0x9010,0xa010,'reference-shape')
        return matcher

    def test_landing_pad_difference_preserves_getter_and_table_identity(self):
        matcher = self.make_matcher()
        matcher.original.functions[0x9010]['digest'] = 'original-entry'
        matcher.native.functions[0xa010]['digest'] = 'native-entry'
        matcher.original.checked_functions = {0x9010: {
            'digest': 'canonical', 'refs': [(0, 0x3000)], 'calls': [], 'length': 3}}
        matcher.native.checked_functions = {0xa010: {
            'digest': 'canonical', 'refs': [(0, 0x4000)], 'calls': [], 'length': 3}}
        matcher.pairs.clear()
        matcher.reverse.clear()
        matcher.how.clear()
        known = {0x3000: (0x4000, 'refs')}
        self.assertEqual(matcher.pair_metaclass_getters(known), 1)
        self.assertEqual(matcher.pairs[0x9010], 0xa010)
        self.assertEqual(matcher.pair_metaclass_vtables(known), 1)
        self.assertEqual(known[0x1000], (0x2000, 'metaclass-vtable'))

    def test_class_specific_getter_identifies_table_without_generic_method_anchors(self):
        matcher = self.make_matcher()
        known = {0x3000:(0x4000,'refs')}
        self.assertEqual(matcher.pair_metaclass_vtables(known),1)
        self.assertEqual(known[0x1000],(0x2000,'metaclass-vtable'))

    def test_getter_requires_matching_metaobject_and_complete_pac_layout(self):
        matcher = self.make_matcher()
        self.assertEqual(matcher.pair_metaclass_vtables({0x3000:(0x4010,'refs')}),0)
        matcher.native.fixups[0x2010] = (0xa000,1,0,7,1)
        self.assertEqual(matcher.pair_metaclass_vtables({0x3000:(0x4000,'refs')}),0)

    def test_identified_table_corrects_weak_string_identity(self):
        matcher = self.make_matcher()
        matcher.bind(0x9000,0xb000,'string')
        matcher.anchor_vtables({0x1000:(0x2000,'metaclass-vtable')})
        self.assertEqual(matcher.pairs[0x9000],0xa000)
        self.assertNotIn(0xb000,matcher.reverse)
        self.assertEqual(matcher.conflicts[-1]['kind'],'vtable-correction')


class PplFunctionTests(unittest.TestCase):
    def test_ppl_function_is_scanned_as_executable(self):
        import struct
        from fileset_match import Side
        image = SimpleNamespace(
            segments=[{'name': '__PPLTEXT', 'va': 0x1000, 'size': 8, 'filesize': 8}],
            function_starts=lambda: [0x1000],
            read=lambda address, length: struct.pack('<2I', 0xd2800000, 0xd65f03c0)[:length],
            symbols=lambda: {},
            section=lambda address: None)
        result = Side(image, {}, [(0x1000, 0x1008)])
        self.assertTrue(result.in_exec(0x1000))
        self.assertEqual(result.functions[0x1000]['end'], 0x1008)


class NormalizedReferenceTests(unittest.TestCase):
    def matcher(self, left, right):
        from collections import defaultdict
        def side(functions):
            return SimpleNamespace(functions=functions, checked_functions=functions,
                symbol_names={}, image=SimpleNamespace(cstring=lambda address: None))
        return Matcher(side(left), side(right))

    def function(self, refs=(), calls=(), end=0x100):
        return {'digest': 'body', 'length': 4, 'refs': list(refs), 'calls': list(calls), 'end': end}

    def test_normalized_data_operands_disambiguate_shared_bodies(self):
        matcher = self.matcher({1: self.function([(4, 1000)])},
            {10: self.function([(4, 2000)]), 20: self.function([(4, 3000)])})
        self.assertEqual(matcher.pair_reference_shapes({1000: (2000, 'refs')}), 1)
        self.assertEqual(matcher.pairs, {1: 10})

    def test_external_call_identifies_a_tail_wrapper_without_data_operands(self):
        matcher = self.matcher({1: self.function(calls=[(0, 1000)])},
            {10: self.function(calls=[(0, 2000)]), 20: self.function(calls=[(0, 3000)])})
        matcher.bind(1000, 2000, 'reviewed')
        self.assertEqual(matcher.pair_reference_shapes({}), 1)
        self.assertEqual(matcher.pairs[1], 10)

    def test_unresolved_operand_and_duplicate_candidates_remain_unpaired(self):
        matcher = self.matcher({1: self.function([(4, 1000), (8, 1100)])},
            {10: self.function([(4, 2000), (8, 2100)])})
        self.assertEqual(matcher.pair_reference_shapes({1000: (2000, 'refs')}), 0)
        matcher = self.matcher({1: self.function([(4, 1000)])},
            {10: self.function([(4, 2000)]), 20: self.function([(4, 2000)])})
        self.assertEqual(matcher.pair_reference_shapes({1000: (2000, 'refs')}), 0)


class CombinedAnchorTests(unittest.TestCase):
    def test_data_anchor_and_known_call_select_the_joint_user(self):
        from collections import defaultdict
        def side(functions, callees):
            return SimpleNamespace(functions=functions, callees=callees, symbol_names={})
        left = side({1: {'refs': [(0, 100)]}}, defaultdict(set, {1: {5}}))
        right = side({10: {'refs': [(4, 200)]}, 20: {'refs': [(8, 200)]}},
                     defaultdict(set, {10: {50}, 20: {60}}))
        matcher = Matcher(left, right)
        matcher.bind(5, 50, 'reviewed')
        self.assertEqual(matcher.pair_data_users({100: (200, 'refs')}), 1)
        self.assertEqual(matcher.pairs[1], 10)

    def test_single_data_anchor_requires_a_known_callee(self):
        from collections import defaultdict
        left = SimpleNamespace(functions={1: {'refs': [(0, 100)]}},
                               callees=defaultdict(set), symbol_names={})
        right = SimpleNamespace(functions={10: {'refs': [(0, 200)]}},
                                callees=defaultdict(set), symbol_names={})
        matcher = Matcher(left, right)
        self.assertEqual(matcher.pair_data_users({100: (200, 'refs')}), 0)


class LeafBodyPairingTests(unittest.TestCase):
    def test_kext_target_has_priority_and_native_addresses_remain_distinct(self):
        import struct
        code = struct.pack('<2I', 0xf9407000, 0xd65f03c0)
        def side(addresses):
            return SimpleNamespace(functions={a: {'end': a + len(code)} for a in addresses},
                symbol_names={}, image=SimpleNamespace(read=lambda a, size: code))
        matcher = Matcher(side([0x1000, 0x2000]), side([0x3000]))
        self.assertEqual(matcher.pair_leaf_bodies({0x2000}), 1)
        self.assertEqual(matcher.pairs, {0x2000: 0x3000})
        self.assertEqual(matcher.pair_leaf_bodies({0x1000}), 0)


class TailBodyPairingTests(unittest.TestCase):
    def matcher(self, *, native_target=0x4000, native_prefix=()):
        import struct
        def side(start, target, prefix):
            branch = 0x14000000 | (((target - start - len(prefix) * 4) // 4) & 0x3ffffff)
            code = struct.pack('<' + 'I' * (len(prefix) + 1), *prefix, branch)
            return SimpleNamespace(functions={start: {'end': start + len(code)}}, symbol_names={},
                image=SimpleNamespace(read=lambda a, size: code))
        return Matcher(side(0x1000, 0x3000, ()), side(0x2000, native_target, native_prefix))

    def test_bound_callee_and_identical_prefix_bind_the_wrapper(self):
        matcher = self.matcher()
        matcher.bind(0x3000, 0x4000, 'reviewed')
        self.assertEqual(matcher.pair_tail_bodies({0x1000}), 1)
        self.assertEqual(matcher.pairs[0x1000], 0x2000)

    def test_unknown_or_different_callee_and_changed_arguments_are_rejected(self):
        matcher = self.matcher()
        self.assertEqual(matcher.pair_tail_bodies({0x1000}), 0)
        for matcher in (self.matcher(native_target=0x5000), self.matcher(native_prefix=(0xd2800000,))):
            matcher.bind(0x3000, 0x4000, 'reviewed')
            self.assertEqual(matcher.pair_tail_bodies({0x1000}), 0)


class AllocationViewTests(unittest.TestCase):
    def side(self, base, *, size=4, signature=b'', count=1):
        import struct
        data = bytearray(0x6000)
        data[0x5000:0x5000 + len(signature) + 1] = signature + b'\0'
        fixups = {}
        for i in range(count):
            at = base + i * 64
            struct.pack_into('<I', data, at + 44, size)
            fixups[at + 16] = (0x4000, 0, None, None, None)
            fixups[at + 32] = (0x5000, 0, None, None, None)
        image = SimpleNamespace(data=data, offset=lambda address: address,
            read=lambda address, length: data[address:address + length],
            cstring=lambda address: 'site.example' if address == 0x4000 else None,
            sections=[{'name': '__kalloc_type', 'va': base, 'size': count * 64}])
        return SimpleNamespace(image=image, fixups=fixups)

    def test_unique_name_signature_and_size_bind_the_descriptor(self):
        matcher = Matcher(self.side(0x1000), self.side(0x2000))
        known = {}
        self.assertEqual(matcher.pair_allocation_views(known), 1)
        self.assertEqual(known, {0x1000: (0x2000, 'allocation-view')})

    def test_duplicate_name_changed_signature_or_changed_size_are_rejected(self):
        for native in (self.side(0x2000, count=2), self.side(0x2000, signature=b'1'), self.side(0x2000, size=8)):
            matcher = Matcher(self.side(0x1000), native)
            self.assertEqual(matcher.pair_allocation_views({}), 0)

