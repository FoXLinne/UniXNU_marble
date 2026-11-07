"""Bounded page-walker checks; synthetic tables are not hardware evidence."""
import unittest
from unittest.mock import patch
from pathlib import Path
import tempfile
from ufs_allocation import walk4k, exercise


class Walker(unittest.TestCase):
    def setUp(self):
        self.tcr = 0x22a51a251a
        self.va = 0xfffffff800000000
        # 38-bit canonical VA: L1 index 0xe0, not the sign-extended 0x1e0.
        self.table = {0x1000+0xe0*8: 0x2003, 0x2000: 0x3003,
                      0x3000: 0x8cb5e9000 | 0x603}

    def walk(self, va=None, tcr=None):
        return walk4k(lambda address: self.table.get(address,0),
                      self.tcr if tcr is None else tcr, 0x1000, 0xff,
                      self.va if va is None else va)

    def test_short_root_and_high_physical(self):
        page = self.walk(self.va+0xabc)
        self.assertEqual(int(page['pa'],16),0x8cb5e9abc)
        self.assertEqual(page['walk'][0]['descriptor_pa'],'0x1700')
        self.assertEqual((page['level'],page['shareability'],page['mair_byte'],page['ap']), (3,2,255,0))

    def test_adjacent_guard(self):
        with self.assertRaisesRegex(RuntimeError,'unmapped'):
            self.walk(self.va+4096)

    def test_noncanonical_and_wrong_granule(self):
        with self.assertRaisesRegex(RuntimeError,'canonical'):
            self.walk(0x50000000)
        with self.assertRaisesRegex(RuntimeError,'4 KiB'):
            self.walk(tcr=self.tcr ^ (1<<30))

    def test_invalid_leaf(self):
        self.table[0x3000] &= ~2
        with self.assertRaisesRegex(RuntimeError,'Invalid hardware leaf'):
            self.walk()

    def test_block_and_memory_attributes(self):
        self.table[0x2000] = 0x8cb400000 | 0x609 | (1<<6)
        page = self.walk(self.va+0x1234)
        self.assertEqual((int(page['pa'],16),page['level'],page['attr_index'],page['mair_byte'],page['ap']),
                         (0x8cb401234,2,2,0,1))


class StartupGuard(unittest.TestCase):
    def test_ufs_startup_arguments_rejected_before_guest_access(self):
        for argument in ('native-ufs-root=1','native-ufs-probe=3'):
            with self.subTest(argument=argument), tempfile.TemporaryDirectory() as temp:
                esp = Path(temp)
                config = esp/'EFI/UniXNU/unixnu.toml'
                config.parent.mkdir(parents=True)
                config.write_text('[boot]\ndefault="test"\n[[entry]]\nid="test"\nargs="'+argument+'"\n')
                with patch('ufs_allocation.subprocess.check_output') as symbols:
                    with self.assertRaisesRegex(RuntimeError,'must not enable UFS'):
                        exercise(None,None,None,None,None,None,esp)
                    symbols.assert_not_called()


if __name__ == '__main__':
    unittest.main()
