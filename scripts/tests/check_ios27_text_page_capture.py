"""Check consistent, raced and failed code-page diagnostic reads."""
from pathlib import Path
import struct
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ios27_text_page_capture import capture

symbols = dict(_vmtc_marble_record=0x100, _vmtc_last_before_buffer=0x200,
               _vmtc_last_after_buffer=0x208)
class Memory:
    def __init__(self, failed=False, race=False):
        self.meta = struct.pack('<8Q', 2, 42, 0x8000, 0x18000, 4096, 1, failed, 1)
        self.reads = 0
        self.race = race
    def read(self, address, size):
        if address == 0x100:
            self.reads += 1
            if self.race and self.reads > 1:
                return struct.pack('<8Q', 3, 42, 0x8000, 0x18000, 4096, 1, 0, 1)
            return self.meta
        return {0x200:struct.pack('<Q',0x1000),0x208:struct.pack('<Q',0x2000),
                0x1000:bytes(4096),0x2000:bytes(17)+b'\x01'+bytes(4078)}[address]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    result = capture(Memory(), symbols, root, 'stable')
    assert result['observed_diff_bytes'] == 1 and result['first_diff_offsets'] == [17]
    assert (root/'stable-before.bin').stat().st_size == 4096
    assert capture(Memory(race=True), symbols, root, 'raced')['status'] == 'changed-during-read'
    assert not (root/'raced-before.bin').exists()
    assert capture(Memory(), symbols, root, 'same',2)['status'] == 'unchanged'
    assert not (root/'same-before.bin').exists()
    result = capture(Memory(failed=True), symbols, root, 'failed')
    assert result['copyin_failed'] and not (root/'failed-after.bin').exists()
    busy=Memory();busy.meta=struct.pack('<8Q',3,42,0,0,4096,0,0,1)
    assert capture(busy,symbols,root,'busy')['status']=='updating'
print('PASS: stable page difference, changed-sequence rejection, unchanged skip, failed-copy handling and in-progress skip')
