#!/usr/bin/env python3
"""Observe one RootStage session and save boot state at a bounded interval."""
import argparse
import fcntl
import json
from pathlib import Path
import signal
import struct
import tempfile
import threading
import time

from root_control import RootUsb
from capture_ios18_resident import RetainedKernel, kernel_symbols, capture_processes, capture_init


def observe(kernel, layout_path, output, interval, duration):
    symbols = kernel_symbols(kernel)
    layout = json.loads(layout_path.read_text())
    stop = threading.Event()
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, lambda *_: stop.set())
    output.mkdir(parents=True, exist_ok=True)
    # One observer owns the bulk command/reply stream. Finish the active
    # transaction before honoring a stop signal so the next client can connect.
    with open(Path(tempfile.gettempdir()) / 'marble-rootstage-observer.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        began = time.monotonic()
        index = 0
        while not stop.is_set() and time.monotonic() - began <= duration:
            destination = output / f'sample-{index:03d}'
            destination.mkdir(exist_ok=True)
            report = {'monotonic': time.monotonic(), 'elapsed': time.monotonic() - began}
            channel = None
            try:
                channel = RootUsb(timeout_ms=5000)
                for key in ('version', 'phase', 'returns'):
                    report[key] = channel.command('getvar:' + key)
                if report['phase'] == 'firmware':
                    memory = RetainedKernel(channel)
                else:
                    base, size = (int(v, 16) for v in channel.command('getvar:kernel-memory').split(':'))
                    address = base + 0x4000 + symbols['_cpu_ttep'] - 0xfffffff006e04000
                    channel.command(f'oem memory-read:{address:016x}:00000008')
                    memory = object.__new__(RetainedKernel)
                    memory.channel, memory.base, memory.size = channel, base, size
                    memory.ttbr1 = int.from_bytes(channel.read_back(), 'little')
                    memory.bits, memory.tcr, memory.tables = 39, 25 | (25 << 16), {}
                report['panic'] = memory.read(symbols['_native_resident_panic_message'], 512).split(b'\0')[0].decode(errors='replace')
                fields = (('_marble_display_transfers', 4), ('_marble_display_frames', 4),
                          ('_marble_ufs_reads', 8), ('_marble_ufs_writes', 8),
                          ('_marble_watchdog_checkins', 8), ('_marble_ui_exit_count', 4),
                          ('_real_ncpus', 4), ('_armcpu_registered', 4),
                          ('_armcpu_smp_requested', 4), ('_armcpu_smp_running', 4),
                          ('_armcpu_ipi_sent', 8), ('_armcpu_ipi_interrupts', 8),
                          ('_sheng_watchdog_policy_status', 4), ('_marble_setup_policy_status', 4),
                          ('_native_staged_apps_stage', 4), ('_native_staged_apps_files', 4),
                          ('_native_staged_apps_mib', 4), ('_native_staged_apps_failed', 4),
                          ('_native_staged_apps_error', 4),
                          ('_marble_app_union_status', 4), ('_marble_app_union_visible', 4),
                          ('_marble_ls_store_status', 4), ('_marble_ls_store_error', 4))
                fields += tuple((name, 4) for name in (
                    '_marble_display_registered', '_marble_display_retirements',
                    '_native_gpu_addon_stage', '_native_gpu_addon_status', '_native_gpu_addon_files',
                    '_native_gpu_addon_written', '_native_gpu_addon_failed', '_native_gpu_addon_error',
                    '_native_adreno_io_coherent'))
                report['globals'] = {name: int.from_bytes(memory.read(symbols[name], size), 'little')
                                     for name, size in fields if name in symbols}
                report['gpu'] = {
                    name: struct.unpack('<' + 'I' * count, memory.read(symbols[name], count * 4))
                    for name, count in (
                        ('_native_adreno_cp_aperture', 4), ('_native_adreno_cp_check', 6),
                        ('_native_adreno_table_switches', 8), ('_native_adreno_recovery', 8),
                        ('_native_adreno_view_diag', 6), ('_native_gpu_dcvs', 15),
                        ('_native_adreno_metal_diag', 24)) if name in symbols}
                if '_native_adreno_metal_rows' in symbols:
                    rows = memory.read(symbols['_native_adreno_metal_rows'], 48 * 96)
                    report['metal_rows'] = [rows[i:i + 96].split(b'\0')[0].decode(errors='replace')
                                           for i in range(0, len(rows), 96) if rows[i]]
                if '_native_adreno_metal_diag' in symbols:
                    text = memory.read(symbols['_native_adreno_metal_diag'] + 390, 128)
                    report['metal_reports'] = [text[i:i + 64].split(b'\0')[0].decode(errors='replace')
                                               for i in (0, 64)]
                if '_native_adreno_client_text' in symbols:
                    text = memory.read(symbols['_native_adreno_client_text'], 8 * 84)
                    report['metal_clients'] = [
                        {'name': text[i:i + 20].split(b'\0')[0].decode(errors='replace'),
                         'text': text[i + 20:i + 84].split(b'\0')[0].decode(errors='replace')}
                        for i in range(0, len(text), 84) if text[i]]
                exit_count = report['globals'].get('_marble_ui_exit_count', 0)
                if exit_count:
                    exit_size = int.from_bytes(memory.read(symbols['_marble_ui_exit_bytes'], 4), 'little')
                    exits = memory.read(symbols['_marble_ui_exits'], 16 * exit_size)
                    report['ui_exits'] = []
                    for sequence in range(max(0, exit_count - 16), exit_count):
                        entry = exits[sequence % 16 * exit_size:(sequence % 16 + 1) * exit_size]
                        pid, namespace, code = struct.unpack_from('<IIQ', entry)
                        saved = entry[48:368]
                        report['ui_exits'].append({
                            'pid': pid, 'namespace': namespace, 'code': hex(code),
                            'name': entry[16:48].split(b'\0')[0].decode(errors='replace'),
                            'pc': hex(struct.unpack_from('<Q', saved, 264)[0]),
                            'far': hex(struct.unpack_from('<Q', saved, 280)[0]),
                            'esr': hex(struct.unpack_from('<Q', saved, 288)[0]),
                            'description': entry[368:].split(b'\0')[0].decode(errors='replace')})
                if '__ZL15domainFrequency' in symbols:
                    report['cpu_khz'] = struct.unpack('<3I', memory.read(symbols['__ZL15domainFrequency'], 12))
                report['cpu_state'] = {name: struct.unpack('<8I', memory.read(symbols[name], 32))
                                       for name in ('_armcpu_smp_step', '_armcpu_smp_fail',
                                                    '_armcpu_core_timer', '_armcpu_core_sgi') if name in symbols}
                processes = capture_processes(memory, symbols, layout['proc'])
                (destination / 'processes.json').write_text(json.dumps(processes, indent=2) + '\n')
                focus = {'CommCenter', 'locationd', 'com.apple.datamigrator',
                         'com.apple.migrationpluginwrapper', 'installcoordinationd',
                         'backboardd', 'SpringBoard', 'Setup', 'Preferences',
                         'runningboardd', 'cfprefsd', 'iconservicesagent',
                         'PosterBoard', 'launchd'}
                report['process_count'] = len(processes)
                report['focus'] = [p for p in processes if p['name'] in focus]
                if index % 5 == 0 or report['panic']:
                    threads = []
                    for process in report['focus']:
                        try:
                            threads.append(capture_init(memory, symbols, layout, hex, destination,
                                                        int(process['address'], 16), process['name'], 32))
                        except (ValueError, IOError) as error:
                            threads.append({'name': process['name'], 'error': str(error)})
                    (destination / 'threads.json').write_text(json.dumps(threads, indent=2) + '\n')
                pointer = int.from_bytes(memory.read(symbols['_msgbufp'], 8), 'little')
                _, size, cursor, _, address = struct.unpack('<4IQ', memory.read(pointer, 24))
                data = memory.read(address, size)
                (destination / 'kernel.log').write_bytes((data[cursor:] + data[:cursor]).replace(b'\0', b''))
                report['communication_end'] = channel.command('getvar:version')
            except Exception as error:
                report['error'] = f'{type(error).__name__}: {error}'
            finally:
                if channel is not None:
                    channel.close()
            (destination / 'state.json').write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps(report), flush=True)
            if report.get('error') or report.get('panic'):
                break
            index += 1
            stop.wait(max(0, began + index * interval - time.monotonic()))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--layout', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--interval', default=60, type=float)
    parser.add_argument('--duration', default=7200, type=float)
    args = parser.parse_args()
    if args.interval < 30:
        parser.error('--interval must be at least 30 seconds')
    observe(args.kernel.resolve(), args.layout.resolve(), args.output.resolve(), args.interval, args.duration)
