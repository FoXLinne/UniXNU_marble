#!/usr/bin/env python3
"""Read retained firmware output and XNU memory through RootStage USB."""
import argparse
from bisect import bisect_right
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import zlib

from root_control import RootUsb


def png_chunk(kind, payload):
    return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))


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
        phase = 'request'
        try:
            self.channel.command(f'oem memory-read:{address:016x}:{count:08x}')
            phase = 'upload'
            data = self.channel.read_back()
        except (OSError, RuntimeError) as error:
            raise IOError(f'Retained kernel read {address:#x}+{count:#x} {phase}: {error}') from error
        if len(data) != count:
            raise IOError(f'Short kernel memory read: {len(data)} of {count}')
        return data

    def translate(self, address, root=None, bits=None):
        bits = self.bits if bits is None else bits
        address &= (1 << bits) - 1
        table = (self.ttbr1 if root is None else root) & 0x0000fffffffff000
        first = 4 - (bits - 12 + 8) // 9
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

    def read(self, address, count, root=None, bits=None):
        data = bytearray()
        while len(data) < count:
            current = address + len(data)
            size = min(count - len(data), 4096 - (current & 4095))
            physical = self.translate(current, root, bits)
            while size < min(count - len(data), 1024 * 1024):
                next_size = min(count - len(data) - size, 4096, 1024 * 1024 - size)
                if self.translate(current + size, root, bits) != physical + size:
                    break
                size += next_size
            data.extend(self.physical(physical, size))
        return bytes(data)


def capture_data(memory, symbols, output):
    address = int.from_bytes(memory.read(symbols['_marble_data_sparse'], 8), 'little')
    if not address:
        return None
    pages, logical, resident = struct.unpack('<3Q', memory.read(address, 24))
    if not pages or not 0 < logical <= 0x100000000 or logical % 4096:
        raise ValueError('Invalid retained Data disk geometry')
    pointers = list(struct.unpack('<' + 'Q' * (logical // 4096), memory.read(pages, logical // 4096 * 8)))
    ranges = []
    for index, pointer in enumerate(pointers):
        if not pointer:
            continue
        if ranges and sum(ranges[-1]) == index * 4096:
            ranges[-1][1] += 4096
        else:
            ranges.append([index * 4096, 4096])
    header = struct.pack('<8sIIQQ', b'DBRDISK1', 1, 4096, logical, len(ranges))
    path = output / 'Data.rdisk'
    physical_pages = sorted((memory.translate(pointer), index)
                            for index, pointer in enumerate(pointer for pointer in pointers if pointer))
    with path.open('w+b') as stream:
        stream.write(header)
        for offset, size in ranges:
            stream.write(struct.pack('<QQ', offset, size))
        payload = stream.tell()
        index = 0
        while index < len(physical_pages):
            physical = physical_pages[index][0]
            end = index + 1
            while end < min(len(physical_pages), index + 256) and physical_pages[end][0] == physical + (end - index) * 4096:
                end += 1
            data = memory.physical(physical, (end - index) * 4096)
            for page in range(index, end):
                stream.seek(payload + physical_pages[page][1] * 4096)
                at = (page - index) * 4096
                stream.write(data[at:at + 4096])
            index = end
        stream.flush()
        stream.seek(0)
        digest = hashlib.file_digest(stream, 'sha256')
    metadata = dict(format='DBRDISK1', logical_bytes=logical, encoded_bytes=path.stat().st_size,
                    resident_bytes=resident, first_install=False, source_build='22H384',
                    volume_uuid=memory.read(symbols['_marble_data_uuid'], 37).split(b'\0', 1)[0].decode(),
                    sha256=digest.hexdigest())
    (output / 'seed.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return dict(path=str(path), **metadata)


def kernel_symbols(kernel, tools=None):
    tools = Path(tools) if tools is not None else next(parent / 'XnuPrelink' for parent in kernel.parents
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


def capture_init(memory, symbols, layout, symbolicate, output, proc=None, label='init', max_threads=16):
    def pointer(value):
        return ((value & ((1 << memory.bits) - 1)) |
                (((1 << 64) - 1) ^ ((1 << memory.bits) - 1))) if value else 0
    def read64(address):
        return int.from_bytes(memory.read(address, 8), 'little')
    proc = pointer(read64(symbols['_initproc'])) if proc is None else proc
    if not proc:
        return None
    proc_layout, thread_layout = layout['proc'], layout['thread']
    task = proc + read64(symbols['_proc_struct_size'])
    user_root = None
    if 'user' in layout:
        user = layout['user']
        vm_map = pointer(read64(task + user['task_map']))
        pmap = pointer(read64(vm_map + user['map_pmap']))
        user_root = read64(pmap + user['pmap_ttep'])
    head = task + thread_layout['task_threads_head']
    thread = pointer(read64(head))
    result = {'address': hex(proc), 'pid': int.from_bytes(memory.read(proc + proc_layout['pid'], 4), 'little'),
              'state': memory.read(proc + proc_layout['state'], 1)[0],
              'name': memory.read(proc + proc_layout['name'], 64).split(b'\0', 1)[0].decode(errors='replace'),
              'threads': []}
    if 'task_cache_slide' in thread_layout:
        result['cache_slide'] = hex(int.from_bytes(memory.read(task + thread_layout['task_cache_slide'], 4), 'little'))
        result['image_info'] = hex(read64(task + thread_layout['task_image_info']))
    context_path = output / f'{label}-context.json'
    context_path.write_text(json.dumps(result, indent=2) + '\n')
    seen = set()
    while thread and thread != head and thread not in seen and len(seen) < max_threads:
        seen.add(thread)
        blob = memory.read(thread, thread_layout['bytes'])
        def field(name, size=8):
            at = thread_layout[name]
            return int.from_bytes(blob[at:at + size], 'little')
        item = {'address': hex(thread), 'state': field('state', 4),
                'continuation': symbolicate(field('continuation')),
                'wait_event': hex(field('wait_event'))}
        if 'jop_pid' in thread_layout:
            item['jop_pid'] = field('jop_pid')
        upcb = pointer(field('upcb'))
        if upcb:
            saved = memory.read(upcb, 320)
            (output / f'{label}-thread-{len(seen)}-user.bin').write_bytes(saved)
            item['user'] = {name: hex(struct.unpack_from('<Q', saved, offset)[0])
                            for name, offset in (('fp', 240), ('lr', 248), ('sp', 256), ('pc', 264),
                                                 ('far', 280), ('esr', 288))}
            item['user']['cpsr'] = hex(struct.unpack_from('<I', saved, 272)[0])
            item['user']['registers'] = {f'x{index}': hex(struct.unpack_from('<Q', saved, 8 + index * 8)[0])
                                         for index in range(29)}
            if user_root:
                bits = 64 - (memory.tcr & 63)
                mask = (1 << bits) - 1
                fp, sp = (int(item['user'][name], 16) & mask for name in ('fp', 'sp'))
                frames = [item['user']['pc'], hex(int(item['user']['lr'], 16) & mask)]
                try:
                    for _ in range(24):
                        if fp & 15 or not sp <= fp < sp + 4 * 1024**2:
                            break
                        next_fp, lr = struct.unpack('<QQ', memory.read(fp, 16, user_root, bits))
                        frames.append(hex(lr & mask))
                        next_fp &= mask
                        if next_fp <= fp:
                            break
                        fp = next_fp
                except (ValueError, IOError) as error:
                    item['user_stack_error'] = str(error)
                item['user_frames'] = frames
        stack, context = pointer(field('kernel_stack')), pointer(field('kstackptr'))
        # UniXNU-sheng c498750: unwind a waiting thread after TH_RUN clears.
        if stack and item['state'] & 0x35 == 1:
            data = memory.read(stack, 16384)
            (output / f'{label}-thread-{len(seen)}-stack.bin').write_bytes(data)
            frames = []
            if stack <= context and context + 96 <= stack + len(data):
                fp, lr = struct.unpack_from('<QQ', data, context - stack + 80)
                frames.append(symbolicate(lr))
                for _ in range(24):
                    fp = pointer(fp)
                    if not stack <= fp <= stack + len(data) - 16:
                        break
                    fp, lr = struct.unpack_from('<QQ', data, fp - stack)
                    if not lr or lr & 3:
                        break
                    frames.append(symbolicate(lr))
            item['kernel_frames'] = frames
        result['threads'].append(item)
        context_path.write_text(json.dumps(result, indent=2) + '\n')
        thread = pointer(field('task_threads'))
    return result


def capture_processes(memory, symbols, layout):
    def pointer(value):
        return ((value & ((1 << memory.bits) - 1)) |
                (((1 << 64) - 1) ^ ((1 << memory.bits) - 1))) if value else 0
    proc = pointer(int.from_bytes(memory.read(symbols['_allproc'], 8), 'little'))
    result, seen = [], set()
    while proc and proc not in seen and len(seen) < 512:
        seen.add(proc)
        blob = memory.read(proc, layout['name'] + 64)
        result.append({'address': hex(proc),
                       'pid': int.from_bytes(blob[layout['pid']:layout['pid'] + 4], 'little'),
                       'state': blob[layout['state']],
                       'name': blob[layout['name']:].split(b'\0', 1)[0].decode(errors='replace')})
        proc = pointer(int.from_bytes(blob[layout['next']:layout['next'] + 8], 'little'))
    return result


def capture(channel, kernel, output, data_snapshot=False):
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
        if '_native_resident_panic_message' in symbols:
            message = memory.read(symbols['_native_resident_panic_message'], 512).split(b'\0', 1)[0].decode(errors='replace')
            if message:
                result['panic_detail'] = message
                (output / 'panic.txt').write_text(message + '\n')
        if '_marble_ui_exit_count' in symbols:
            count = int.from_bytes(memory.read(symbols['_marble_ui_exit_count'], 4), 'little')
            size = (int.from_bytes(memory.read(symbols['_marble_ui_exit_bytes'], 4), 'little')
                    if '_marble_ui_exit_bytes' in symbols else 880)
            records = memory.read(symbols['_marble_ui_exits'], 16 * size)
            result['ui_exits'] = []
            for index in range(max(0, count - 16), count):
                record = records[index % 16 * size:(index % 16 + 1) * size]
                pid, namespace, code = struct.unpack_from('<IIQ', record)
                state = record[48:]
                result['ui_exits'].append({
                    'pid': pid, 'namespace': namespace, 'code': hex(code),
                    'name': record[16:48].split(b'\0', 1)[0].decode(errors='replace'),
                    'state': {name: hex(struct.unpack_from('<Q', state, offset)[0])
                              for name, offset in (('pc', 264), ('lr', 248), ('sp', 256), ('fp', 240),
                                                   ('esr', 288), ('far', 280))},
                    'registers': [hex(value) for value in struct.unpack_from('<29Q', state, 8)]})
                if size >= 880:
                    result['ui_exits'][-1]['description'] = record[368:880].split(b'\0', 1)[0].decode(errors='replace')
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
                           ('_armcpu_timer_interrupts', 8), ('_armcpu_ipi_sent', 8),
                           ('_marble_watchdog_checkins', 8), ('_marble_watchdog_opens', 4),
                           ('_marble_watchdog_selectors', 4), ('_marble_watchdog_monitoring', 4),
                           ('_native_aes_kat_status', 4), ('_native_aes_registered', 4),
                           ('_native_aes_interface_status', 4), ('_native_aes_completed', 4),
                           ('_native_aes_unsupported_key', 4),
                           ('_marble_display_stage', 4), ('_marble_display_registered', 4),
                           ('_marble_display_clients', 4), ('_marble_display_client_status', 4),
                           ('_marble_display_frames', 4), ('_marble_display_transfers', 4),
                           ('_marble_display_transfer_status', 4), ('_marble_display_retirements', 4),
                           ('_marble_display_swap_stage', 4), ('_marble_display_validation', 4),
                           ('_marble_display_rejections', 4), ('_marble_display_mapping', 8),
                           ('_marble_hid_stage', 4), ('_marble_hid_ticks', 4),
                           ('_marble_haptics_stage', 4), ('_marble_haptics_play_count', 4),
                           ('_num_static_trust_caches', 4), ('_num_engineering_trust_caches', 4),
                           ('_cs_native4k_ready_blobs', 8), ('_cs_native4k_bytes', 8),
                           ('_cs_native4k_full_blocks', 8), ('_cs_native4k_page_checks', 8),
                           ('_cs_native4k_read_ticks', 8), ('_cs_native4k_hash_ticks', 8)):
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
        values = [int(value, 16) for value in varargs[1].split()] if varargs else []
        if values:
            result['panic_arguments'] = [hex(value) for value in values]
            if result.get('format', '').startswith('%s @'):
                result['panic_detail'] = memory.read(values[0], 800).split(b'\0', 1)[0].decode(errors='replace')
        if 'exit reason namespace' in result.get('format', '') and len(values) >= 5:
            description = memory.read(values[4], 800).split(b'\0', 1)[0].decode(errors='replace')
            result['exit_reason'] = {'namespace': values[2], 'subcode': hex(values[3]), 'description': description}
        if varargs and '(saved state: %p%s)' in console:
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
                fields = json.loads(layout.read_text())
                result['initproc'] = capture_init(memory, symbols, fields, symbolicate, output)
                if 'next' in fields['proc'] and '_allproc' in symbols:
                    result['processes'] = capture_processes(memory, symbols, fields['proc'])
                    if result.get('panic_detail', '').startswith('busy timeout'):
                        process = next(item for item in result['processes'] if item['name'] == 'kernel_task')
                        result['kernel_task'] = capture_init(
                            memory, symbols, fields, symbolicate, output,
                            int(process['address'], 16), 'kernel_task', 256)
                    result['ui_processes'] = []
                    for process in result['processes']:
                        if process['name'] in ('SpringBoard', 'backboardd', 'watchdogd', 'lsd',
                                               'runningboardd', 'installd', 'containermanagerd',
                                               'com.apple.datamigrator', 'com.apple.migrationpluginwrapper',
                                               'installcoordinationd', 'PreBoard', 'keybagd'):
                            result['ui_processes'].append(capture_init(
                                memory, symbols, fields, symbolicate, output,
                                int(process['address'], 16), process['name'] + '-' + str(process['pid'])))
                    if 'exit_reason' in result:
                        prefix = memory.read(values[1], 128).split(b'\0', 1)[0].decode(errors='replace')
                        match = re.search(r'\[(\d+)\] exited', prefix)
                        if match and int(match[1]) != result['initproc']['pid']:
                            process = next((item for item in result['processes'] if item['pid'] == int(match[1])), None)
                            if process:
                                result['faultproc'] = capture_init(memory, symbols, fields, symbolicate, output,
                                                                   int(process['address'], 16), 'fault')
            except (ValueError, IOError) as error:
                result['process_capture_error'] = str(error)
        if '_marble_display_snapshot_ready' in symbols and int.from_bytes(memory.read(symbols['_marble_display_snapshot_ready'], 4), 'little'):
            width, height, stride = struct.unpack('<3I', memory.read(symbols['_marble_display_geometry'], 12))
            pixels = int.from_bytes(memory.read(symbols['_marble_display_pixels'], 8), 'little')
            raw = memory.read(pixels, stride * height)
            rgb = bytearray(width * height * 3)
            for row in range(height):
                source = raw[row * stride:row * stride + width * 4]
                line = bytearray(width * 3)
                line[0::3], line[1::3], line[2::3] = source[2::4], source[1::4], source[0::4]
                rgb[row * width * 3:(row + 1) * width * 3] = line
            rows = b''.join(b'\0' + rgb[row * width * 3:(row + 1) * width * 3] for row in range(height))
            png = b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 2, 0, 0, 0))
            png += png_chunk(b'IDAT', zlib.compress(rows)) + png_chunk(b'IEND', b'')
            (output / 'display.png').write_bytes(png)
            result['display'] = str(output / 'display.png')
        for name in ('_native_adsp_stats', '_native_battery_stats', '_native_charger_stats', '_native_qrtr_stats'):
            if name in symbols:
                result[name[1:]] = list(struct.unpack('<16i', memory.read(symbols[name], 64)))
        keystore = (('refused_opens', '__ZL12refusedOpens'), ('opened_clients', '__ZL13openedClients'),
                    ('calls', '__ZL11clientCalls'))
        if all(symbol in symbols for _, symbol in keystore):
            result['keystore'] = {key: int.from_bytes(memory.read(symbols[symbol], 4), 'little')
                                  for key, symbol in keystore}
        if '_marble_touch_status' in symbols:
            result['touch'] = memory.read(symbols['_marble_touch_status'], 256).split(b'\0', 1)[0].decode()
        for name, count in (('_marble_hid_events', 8), ('_marble_hid_results', 8),
                            ('_native_touch_edge_install', 3), ('_native_touch_edge_stats', 4)):
            if name in symbols:
                result[name[1:]] = list(struct.unpack('<' + 'i' * count, memory.read(symbols[name], count * 4)))
        for name, count, kind, width in (('_marble_display_pm', 3, 'I', 4),
                                         ('_marble_display_vsync', 2, 'I', 4),
                                         ('_marble_display_frame', 6, 'Q', 8)):
            if name in symbols:
                result[name[1:]] = list(struct.unpack('<' + kind * count, memory.read(symbols[name], count * width)))
        if data_snapshot and '_marble_data_sparse' in symbols:
            result['data_snapshot'] = capture_data(memory, symbols, output)
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
    parser.add_argument('--data-snapshot', action='store_true')
    args = parser.parse_args()
    channel = RootUsb()
    try:
        print(json.dumps(capture(channel, args.kernel.resolve(), args.output.resolve(), args.data_snapshot), indent=2))
    finally:
        channel.close()
