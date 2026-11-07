"""Bounded read-only UI thread snapshots using the exact compiler layout.

Saved registers and bounded user frame records are read. User stacks are read
through the task's actual native 4K tables using QEMU physical debugger reads;
no guest registers, memory or translations are changed. A saved PC observes
the most recent user/kernel boundary, not proof of a thread's current wait cause.
"""
import struct


def capture(packet, processes, plan, registers):
    layout = plan['thread_snapshot_layout']
    common = plan['process_layout']
    if any(not isinstance(v, int) or not 0 <= v < 65536 for v in layout.values()):
        raise ValueError('Unbounded compiler thread layout')

    def read(address, size):
        if not 0xfffffff000000000 <= address <= (1 << 64) - size or not 0 < size <= 304:
            raise ValueError('Not a bounded kernel saved-state read')
        raw = packet(f'm{address:x},{size:x}')
        if raw.startswith('E') or len(raw) != size * 2:
            raise ValueError('Kernel saved-state memory unavailable')
        return bytes.fromhex(raw)

    def word(address, size=8):
        return int.from_bytes(read(address, size), 'little')

    results = []
    names = ('PreBoard', 'Setup', 'SpringBoard', 'backboardd', 'lsd', 'runningboardd', 'datamigrator',
             'com.apple.datami', 'keybagd', 'containermanager', 'installd', 'cfprefsd', 'launchd',
             'chronod', 'liveactivitiesd')
    selected = [p for p in processes if p['name'] in names]
    for proc in selected[:16]:
        row = dict(process=proc, threads=[], complete=False)
        results.append(row)
        try:
            address = int(proc['address'], 16)
            ro = word(address + layout['proc_ro'])
            if word(ro + layout['proc_ro_owner']) != address:
                raise ValueError('Process read-only owner mismatch')
            task = word(ro + layout['proc_ro_task'])
            region = word(task + common['task_region'])
            row.update(task=hex(task), thread_count=word(task + layout['task_thread_count'], 4),
                       cache_slide=word(region + common['region_slide'], 4) if region else None)
            vm_map = word(task + layout['task_map'])
            pmap = word(vm_map + layout['map_pmap'])
            attr = word(plan['thread_snapshot_native_pt_attr'])
            row.update(pmap=hex(pmap), ttep=word(pmap + layout['pmap_ttep']),
                       root_level=word(attr + layout['attr_root'], 4),
                       page_shift=word(attr + layout['attr_shift']))
            head = task + layout['task_threads']
            node = word(head)
            seen = set()
            while node != head and node not in seen and len(row['threads']) < 64:
                seen.add(node)
                # XNU queue_enter/queue_iterate stores object pointers, not
                # addresses of the embedded task_threads linkage.
                thread = node
                tro = word(thread + common['thread_ro'])
                if word(tro + common['ro_owner']) != thread or word(tro + common['ro_task']) != task:
                    raise ValueError('Thread read-only owner/task mismatch')
                state = word(thread + common['thread_user_state'])
                item = dict(thread=hex(thread), thread_id=word(thread + layout['thread_id']),
                            state=word(thread + layout['thread_state'], 4),
                            continuation=hex(word(thread + layout['continuation'])),
                            wait_event=hex(word(thread + layout['wait_event'])), saved_state=hex(state))
                row['threads'].append(item)
                if state:
                    raw = read(state, 304)
                    if struct.unpack_from('<2I', raw) == (21, 72):
                        regs = struct.unpack_from('<33Q', raw, 8)
                        cpsr, = struct.unpack_from('<I', raw, 272)
                        if cpsr & 31 == 0:
                            item.update(user_pc=hex(regs[32]), user_lr=hex(regs[30]),
                                        user_fp=hex(regs[29]), user_sp=hex(regs[31]),
                                        user_arguments=[hex(v) for v in regs[:8]], cpsr=hex(cpsr))
                node = word(thread + layout['thread_link'])
            row['complete'] = node == head and len(row['threads']) == row['thread_count']
        except ValueError as error:
            row['error'] = str(error)
    tcr = int(registers['TCR_EL1'], 16)
    if (tcr >> 14) & 3 or (tcr >> 30) & 3 != 2:
        raise ValueError('Stack snapshot requires active native 4K CPU translation')
    old_mode = packet('qqemu.PhyMemMode')
    if old_mode not in ('0', '1') or packet('Qqemu.PhyMemMode:1') != 'OK':
        raise ValueError('QEMU physical debugger reads unavailable')
    try:
        def physical(address, size):
            if not 0 < address < (1 << 48) - size or not 0 < size <= 16:
                raise ValueError('Invalid bounded physical read')
            raw = packet(f'm{address:x},{size:x}')
            if raw.startswith('E') or len(raw) != size * 2:
                raise ValueError('Unbacked physical memory')
            return bytes.fromhex(raw)

        for row in results:
            if row.get('page_shift') != 12 or row.get('root_level') not in (0, 1, 2):
                row['stack_error'] = 'Unqualified native 4K task geometry'
                continue

            def user_record(va):
                va_bits = 64 - (tcr & 63)
                root_bits = va_bits - (12 + 9 * (3 - row['root_level']))
                if not 1 <= root_bits <= 9:
                    raise ValueError('Unsupported task root-table extent')
                if not 0 < va < 1 << va_bits or va & 15:
                    raise ValueError('Invalid user frame address')
                table = row['ttep']
                # A native 4K hierarchy can have a smaller initial table:
                # T0SZ=26 uses 256 L1 entries, aligned to 2048 bytes.
                if table & ((8 << root_bits) - 1):
                    raise ValueError('Unaligned task page table')
                for level in range(row['root_level'], 4):
                    shift = 12 + 9 * (3 - level)
                    descriptor = int.from_bytes(physical(table + ((va >> shift) & 511) * 8, 8), 'little')
                    if descriptor & 1 == 0:
                        raise ValueError('User frame is not resident')
                    base = descriptor & 0x0000fffffffff000
                    if level < 3 and descriptor & 3 == 3:
                        table = base
                        continue
                    if (level == 3 and descriptor & 3 != 3) or (level < 3 and descriptor & 3 != 1):
                        raise ValueError('Invalid user frame page descriptor')
                    if not descriptor & (1 << 6):
                        raise ValueError('User frame page lacks EL0 access')
                    return physical((base & ~((1 << shift) - 1)) | (va & ((1 << shift) - 1)), 16)
                raise ValueError('Missing user frame leaf')

            for item in row['threads']:
                if 'user_fp' not in item:
                    continue
                fp, sp = int(item['user_fp'], 16), int(item['user_sp'], 16)
                item['user_frames'] = []
                item['user_stack_complete'] = False
                try:
                    for _ in range(32):
                        if not fp:
                            break
                        if not sp <= fp < sp + 65536:
                            raise ValueError('Frame exceeds bounded stack window')
                        next_fp, lr = struct.unpack('<2Q', user_record(fp))
                        item['user_frames'].append(dict(fp=hex(fp), lr=hex(lr)))
                        if next_fp and next_fp <= fp:
                            raise ValueError('Non-increasing user frame chain')
                        fp = next_fp
                    item['user_stack_complete'] = fp == 0
                except ValueError as error:
                    item['stack_error'] = str(error)
    finally:
        if packet('Qqemu.PhyMemMode:' + old_mode) != 'OK':
            raise RuntimeError('Cannot restore debugger virtual-memory mode')
    return dict(native_sha256=plan['native_sha256'], read_only=True,
                process_limit=16, threads_per_process_limit=64, frames_per_thread_limit=32,
                stack_window_bytes=65536, stack_source='actual-native-4k-task-tables',
                selected_processes=len(selected), processes=results)
