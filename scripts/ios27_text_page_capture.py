"""Read a consistent RELEASE code-page comparison through the resident reader."""
import hashlib
import json
import struct
from pathlib import Path


def capture(memory, symbols, output, name, after_sequence=-1):
    address = symbols['_vmtc_marble_record']
    raw = memory.read(address, 64)
    sequence, pid, va, pa, size, differences, failed, count = struct.unpack('<8Q', raw)
    result = dict(sequence=sequence, comparisons=count)
    if not sequence or sequence == after_sequence:
        return dict(result, status='unchanged')
    if sequence & 1:
        return dict(result, status='updating')
    if not 0 < size <= 16384 or size & (size - 1):
        raise ValueError('Invalid code-page diagnostic size')
    before_pointer = int.from_bytes(memory.read(symbols['_vmtc_last_before_buffer'], 8), 'little')
    after_pointer = int.from_bytes(memory.read(symbols['_vmtc_last_after_buffer'], 8), 'little')
    if not before_pointer or not after_pointer:
        raise ValueError('Code-page diagnostic buffer pointer is empty')
    before = memory.read(before_pointer, size)
    after = b'' if failed else memory.read(after_pointer, size)
    if memory.read(address, 64) != raw:
        return dict(result, status='changed-during-read')
    result.update(status='captured', pid=pid, code_va=hex(va), page_pa=hex(pa), size=size,
                  kernel_diff_bytes=differences, copyin_failed=bool(failed),
                  before_sha256=hashlib.sha256(before).hexdigest())
    if not failed:
        offsets = [index for index, (a, b) in enumerate(zip(before, after)) if a != b]
        result.update(after_sha256=hashlib.sha256(after).hexdigest(),
                      observed_diff_bytes=len(offsets), first_diff_offsets=offsets[:64])
    output = Path(output)
    (output / (name + '-before.bin')).write_bytes(before)
    if not failed:
        (output / (name + '-after.bin')).write_bytes(after)
    (output / (name + '.json')).write_text(json.dumps(result, indent=2) + '\n')
    (output / 'text-page-latest.json').write_text(json.dumps(result, indent=2) + '\n')
    return result
