#!/usr/bin/env python3
"""Read retained firmware output and XNU memory through RootStage USB."""
import argparse
from bisect import bisect_right
import json
from pathlib import Path
import re
import struct
import sys
import zlib

from root_control import RootUsb


class RetainedKernel:
    def __init__(self, channel):
        self.channel = channel
        self.base, self.size = (int(v, 16) for v in channel.command('getvar:kernel-memory').split(':'))
        self.ttbr1, self.tcr = (int(v, 16) for v in channel.command('getvar:handoff-translation').split(':'))
        self.bits = 64 - ((self.tcr >> 16) & 63)
        if not self.ttbr1 or not 25 <= self.bits <= 48:
            raise ValueError('Retained kernel translation registers are unavailable')
        self.tables = {}

    def physical(self, address, count):
        if count < 1 or address < self.base or address + count > self.base + self.size:
            raise ValueError(f'Read outside retained kernel memory: {address:#x}+{count:#x}')
        self.channel.command(f'oem memory-read:{address:016x}:{count:08x}')
        data = self.channel.read_back()
        if len(data) != count:
            raise IOError(f'Short kernel memory read: {len(data)} of {count}')
        return data

    def translate(self, address):
        address &= (1 << self.bits) - 1
        table = self.ttbr1 & 0x0000fffffffff000
        first = 4 - (self.bits - 12 + 8) // 9
        for level in range(first, 4):
            if table not in self.tables:
                self.tables[table] = self.physical(table, 4096)
            shift = 12 + 9 * (3 - level)
            entry, = struct.unpack_from('<Q', self.tables[table], ((address >> shift) & 511) * 8)
            if not entry & 1:
                raise ValueError(f'Unmapped retained kernel address: {address:#x}, level {level}')
            if level == 3 or entry & 3 == 1:
                mask = (1 << shift) - 1
                return (entry & 0x0000fffffffff000 & ~mask) | (address & mask)
            table = entry & 0x0000fffffffff000
        raise ValueError('Invalid retained kernel page table')

    def read(self, address, count):
        data = bytearray()
        while len(data) < count:
            current = address + len(data)
            size = min(count - len(data), 4096 - (current & 4095))
            data.extend(self.physical(self.translate(current), size))
        return bytes(data)


def kernel_symbols(kernel):
    tools = next(parent / 'XnuPrelink' for parent in kernel.parents
                 if (parent / 'XnuPrelink/kc_image.py').is_file())
    sys.path.insert(0, str(tools))
    from kc_image import Image
    collection = Image(kernel.read_bytes())
    image = Image(collection.data, collection.entries['com.apple.kernel'][1])
    symbols = {name: address for address, names in image.symbols().items() for name in names}
    for identifier, (_, offset) in collection.entries.items():
        if identifier == 'com.apple.kernel':
            continue
        component = Image(collection.data, offset)
        for address in component.function_starts():
            symbols[f'{identifier}+{address - component.base:#x}'] = address
    return symbols


def capture_init(memory, symbols, layout, symbolicate, output):
    def pointer(value):
        return ((value & ((1 << memory.bits) - 1)) |
                (((1 << 64) - 1) ^ ((1 << memory.bits) - 1))) if value else 0
    def read64(address):
        return int.from_bytes(memory.read(address, 8), 'little')
    proc = pointer(read64(symbols['_initproc']))
    if not proc:
        return None
    proc_layout, thread_layout = layout['proc'], layout['thread']
    task = proc + read64(symbols['_proc_struct_size'])
    head = task + thread_layout['task_threads_head']
    thread = pointer(read64(head))
    result = {'address': hex(proc), 'pid': int.from_bytes(memory.read(proc + proc_layout['pid'], 4), 'little'),
              'state': memory.read(proc + proc_layout['state'], 1)[0],
              'name': memory.read(proc + proc_layout['name'], 64).split(b'\0', 1)[0].decode(errors='replace'),
              'threads': []}
    seen = set()
    while thread and thread != head and thread not in seen and len(seen) < 16:
        seen.add(thread)
        blob = memory.read(thread, thread_layout['bytes'])
        def field(name, size=8):
            at = thread_layout[name]
            return int.from_bytes(blob[at:at + size], 'little')
        item = {'address': hex(thread), 'state': field('state', 4),
                'continuation': symbolicate(field('continuation')),
                'wait_event': hex(field('wait_event'))}
        upcb = pointer(field('upcb'))
        if upcb:
            saved = memory.read(upcb, 320)
            (output / f'init-thread-{len(seen)}-user.bin').write_bytes(saved)
            item['user'] = {name: hex(struct.unpack_from('<Q', saved, offset)[0])
                            for name, offset in (('fp', 240), ('lr', 248), ('sp', 256), ('pc', 264),
                                                 ('far', 280), ('esr', 288))}
            item['user']['cpsr'] = hex(struct.unpack_from('<I', saved, 272)[0])
        stack, context = pointer(field('kernel_stack')), pointer(field('kstackptr'))
        if stack:
            data = memory.read(stack, 16384)
            (output / f'init-thread-{len(seen)}-stack.bin').write_bytes(data)
            frames = []
            if stack <= context and context + 96 <= stack + len(data):
                fp, lr = struct.unpack_from('<QQ', data, context - stack + 80)
                frames.append(symbolicate(lr))
                for _ in range(24):
                    fp = pointer(fp)
                    if not stack <= fp <= stack + len(data) - 16:
                        break
                    fp, lr = struct.unpack_from('<QQ', data, fp - stack)
                    frames.append(symbolicate(lr))
            item['kernel_frames'] = frames
        result['threads'].append(item)
        thread = pointer(field('task_threads'))
    return result


def capture(channel, kernel, output):
    output.mkdir(parents=True, exist_ok=True)
    result = {'kernel': str(kernel.resolve()), 'completed': False}
    report = output / 'capture.json'
    try:
        features = channel.command('getvar:features').split(',')
        channel.command('oem select:2')
        firmware = channel.read_back()
        (output / 'firmware.bin').write_bytes(firmware)
        console = firmware[:65536].decode('utf-16-le', errors='strict').split('\0', 1)[0]
        (output / 'firmware.log').write_text(console)
        screen = firmware[65536:]
        if screen[:8] == b'ROOTRGB1':
            width, height, stride, count = struct.unpack_from('<4I', screen, 8)
            if not width or not height or stride != width * 3 or count != stride * height or count > len(screen) - 32:
                raise ValueError('Invalid retained firmware screen geometry')
            def png_chunk(kind, payload):
                return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
            rows = b''.join(b'\0' + screen[32 + row * stride:32 + (row + 1) * stride] for row in range(height))
            png = b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 2, 0, 0, 0))
            png += png_chunk(b'IDAT', zlib.compress(rows)) + png_chunk(b'IEND', b'')
            (output / 'screen.png').write_bytes(png)
            result['screen'] = str(output / 'screen.png')
        result['fault'] = channel.command('getvar:handoff-fault')
        if 'memory-read' not in features:
            raise RuntimeError('Firmware lacks retained kernel memory reads')
        memory = RetainedKernel(channel)
        result['translation'] = {'ttbr1': hex(memory.ttbr1), 'tcr': hex(memory.tcr),
                                 'base': hex(memory.base), 'size': memory.size}
        symbols = kernel_symbols(kernel)
        addresses = sorted((address, name) for name, address in symbols.items())
        starts = [address for address, name in addresses]
        def symbolicate(raw):
            address = raw & ((1 << memory.bits) - 1)
            if not address & (1 << (memory.bits - 1)):
                return {'address': hex(address), 'space': 'user'} if address else {'address': '0x0'}
            address |= ((1 << 64) - 1) ^ ((1 << memory.bits) - 1)
            index = bisect_right(starts, address) - 1
            resolved = {'address': hex(address)}
            if index >= 0 and address - starts[index] < 65536:
                resolved.update(symbol=addresses[index][1], offset=hex(address - starts[index]))
            return resolved
        frames = [symbolicate(int(value, 16)) for value in
                  re.findall(r'Resident frame \d+: fp=[0-9A-Fa-f]+ lr=([0-9A-Fa-f]+)', console)]
        result['frames'] = frames
        for label in ('message', 'format'):
            match = re.search(r'Resident ' + label + r': ([^\r\n]+)', console)
            if match:
                result[label] = match[1]
        result['globals'] = {}
        for name, size in (('_mac_late', 4), ('_amfi', 8), ('_armcpu_registered', 4),
                           ('_armcpu_timer_interrupts', 8), ('_armcpu_ipi_sent', 8)):
            if name in symbols:
                result['globals'][name] = int.from_bytes(memory.read(symbols[name], size), 'little')
        address, = struct.unpack('<Q', memory.read(symbols['_msgbufp'], 8))
        header = memory.read(address, 24)
        (output / 'msgbuf-header.bin').write_bytes(header)
        magic, size, cursor, reader, data = struct.unpack('<4IQ', header)
        if magic != 0x063061 or not 0 < size <= 1048576 or not 0 <= cursor < size:
            raise ValueError(f'Invalid XNU message buffer: {magic:#x}, size {size}, cursor {cursor}')
        ring = memory.read(data, size)
        (output / 'msgbuf.bin').write_bytes(ring)
        ordered = (ring[cursor:] + ring[:cursor]).lstrip(b'\0')
        (output / 'kernel.log').write_bytes(ordered)
        result['msgbuf'] = {'address': hex(address), 'data': hex(data), 'size': size,
                            'cursor': cursor, 'reader': reader, 'exported': len(ring)}
        varargs = re.search(r'Resident varargs: ([0-9A-Fa-f ]+)', console)
        if varargs and '(saved state: %p%s)' in console:
            values = [int(value, 16) for value in varargs[1].split()]
            if len(values) > 3:
                state = values[3]
                (output / 'saved-state.bin').write_bytes(memory.read(state, 512))
                result['saved_state'] = hex(state)
                result['exception_pc'] = symbolicate(values[1])
                result['exception_lr'] = symbolicate(values[2])
        frame = re.search(r'Resident fp=([0-9A-Fa-f]+)', console)
        if frame:
            address = int(frame[1], 16) & ~4095
            (output / 'panic-stack.bin').write_bytes(memory.read(address, 4096))
            result['panic_stack'] = hex(address)
        layout = kernel.with_name('kernel.layout.json')
        if layout.is_file() and '_initproc' in symbols and '_proc_struct_size' in symbols:
            try:
                result['initproc'] = capture_init(memory, symbols, json.loads(layout.read_text()), symbolicate, output)
            except (ValueError, IOError) as error:
                result['process_capture_error'] = str(error)
        result['completed'] = True
        return result
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        report.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    channel = RootUsb()
    try:
        print(json.dumps(capture(channel, args.kernel.resolve(), args.output.resolve()), indent=2))
    finally:
        channel.close()
