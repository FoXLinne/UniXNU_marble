import struct
import unittest

from fileset_shape import checked_shape


def shape(words):
    return checked_shape(struct.pack('<' + 'I' * len(words), *words), 0x10000)


class CheckedShapeTests(unittest.TestCase):
    def test_authenticated_call_discriminator_copies(self):
        copied = [0xf8468e08, 0xaa1003e9, 0xaa0003f6, 0xaa0903f1, 0xf2e2c031, 0xd73f0911]
        reused = [0xf8468e08, 0xaa0003f6, 0xf2e2c030, 0xd73f0910]
        self.assertEqual(shape(copied)[0], shape(reused)[0])
        self.assertEqual(shape(copied)[1][20], 12)

    def test_discriminator_copy_with_intervening_operand_use_is_kept(self):
        code = [0xf8468e08, 0xaa1003e9, 0xaa1003e0, 0xaa0903f1, 0xf2e2c031, 0xd73f0911]
        self.assertEqual(len(shape(code)[0]), len(code))

    def test_leading_bti_preserves_internal_branches_and_reference_positions(self):
        body = [0xb4000040, 0x90000008, 0xf9400900, 0xd65f03c0]
        first, second = shape(body), shape([0xd503245f, *body])
        self.assertEqual(first[0], second[0])
        self.assertEqual(second[1][8], 4)
        self.assertEqual(second[1][12], 8)

    def test_branch_to_entry_keeps_the_landing_pad(self):
        code = [0xd503245f, 0x17ffffff]
        self.assertEqual(len(shape(code)[0]), 2)

    def test_internal_bti_is_kept(self):
        self.assertEqual(len(shape([0xd503201f, 0xd503245f, 0xd65f03c0])[0]), 3)

    def test_authentication_check_preserves_branch_geometry_and_reference_sites(self):
        # cbz x1,end; autda x16,x17; [check]; mov x0,x16; ret
        check = [0xaa1003f1,0xdac147f1,0xeb11021f,0x54000040,0xd4388e40]
        release = [0xb4000061,0xdac11a30,0xaa1003e0,0xd65f03c0]
        development = [0xb4000101,0xdac11a30,*check,0xaa1003e0,0xd65f03c0]
        a,b = shape(release),shape(development)
        self.assertEqual(a[0],b[0])
        self.assertEqual(b[1][28],8)
        self.assertEqual(b[1][32],12)

    def test_partial_or_unrelated_check_is_kept(self):
        check = [0xaa1003f1,0xdac147f1,0xeb11021f,0x54000040,0xd4388e40]
        self.assertEqual(len(shape([0xd503201f,*check,0xd65f03c0])[0]),7)
        self.assertEqual(len(shape([0xdac11a30,*check[:-1],0xd65f03c0])[0]),6)

    def test_branch_entering_check_disables_normalization(self):
        check = [0xaa1003f1,0xdac147f1,0xeb11021f,0x54000040,0xd4388e40]
        code = [0xb4000040,0xdac11a30,*check,0xd65f03c0]
        self.assertEqual(len(shape(code)[0]),len(code))

    def test_lr_check_requires_tail_branch_and_preserves_call_site(self):
        check = [0xca1e07d0,0xb6f00050,0xd4388e20]
        a = shape([0xd50323ff,0x14000400])
        b = shape([0xd50323ff,*check,0x140003fd])
        self.assertEqual(a[0],b[0])
        self.assertEqual(b[1][16],4)
        self.assertEqual(a[2][0][1],b[2][0][1])
        self.assertEqual(len(shape([*check,0xd65f03c0])[0]),4)


class LeafBodyTests(unittest.TestCase):
    def leaf(self, words):
        from fileset_shape import relocation_free_leaf
        return relocation_free_leaf(struct.pack('<' + 'I' * len(words), *words), 0x10000)

    def test_memory_accessor_is_identical_with_a_native_landing_pad(self):
        body = [0xf9407000, 0xd65f03c0]
        self.assertEqual(self.leaf(body), self.leaf([0xd503245f, *body]))
        self.assertIsNotNone(self.leaf(body))
        self.assertNotEqual(self.leaf(body), self.leaf([0xf9407400, 0xd65f03c0]))

    def test_pc_relative_operands_and_calls_are_rejected(self):
        for word in (0x90000008, 0x10000000, 0x58000000, 0x94000001, 0xd63f0100):
            self.assertIsNone(self.leaf([word, 0xd65f03c0]))

    def test_internal_conditional_branch_keeps_its_geometry(self):
        body = [0xb4000040, 0xf9400c00, 0xd65f03c0]
        self.assertEqual(self.leaf(body), self.leaf([0xd503245f, *body]))
        self.assertIsNone(self.leaf([0xb4000080, 0xd65f03c0]))
        self.assertIsNone(self.leaf([0x14000002, 0xd65f03c0]))


if __name__ == '__main__':
    unittest.main()
