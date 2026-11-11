#!/usr/bin/env python3
"""Capture the live Actuator transport, motor state and kernel log through firmware USB."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import struct
import tempfile
from datetime import datetime
from root_control import RootUsb
from root_ui import memory_for
from capture_ios18_resident import kernel_symbols, capture_processes

DEVICE = Path(__file__).resolve().parents[1]
RUNTIME_FIELDS = ('ready', 'opens', 'writes', 'frames', 'loud_frames', 'queued_segments', 'consumed_segments',
    'hardware_starts', 'hardware_stops', 'rejected_packets', 'last_error', 'peak_pcm', 'queue_high_water',
    'active', 'clients', 'stop_requests', 'late_segments', 'generation', 'start_requests', 'amplitude')

def decode_audio_route_snapshots(words, count):
    """Decode committed ring rows; discard rows overwritten during USB reads."""
    names = ('Speaker', 'Actuator', 'Hawking', 'Codec', 'VirtualAudioDevice_Default', 'VirtualAudioDevice_Actuator')
    signed = lambda value: value - (1 << 32) if value & 0x80000000 else value
    result = []
    for n in range(max(0, count - 64), count):
        row = words[(n % 64) * 128:(n % 64 + 1) * 128]
        if len(row) != 128 or row[0] != n * 2 + 2:
            continue
        snapshot = dict(sequence=n + 1, host_time=row[1] | row[2] << 32, pid=row[3],
            speaker_starts=row[4], speaker_stops=row[5], speaker_users=row[6], start_error=signed(row[7]),
            default_output_status=signed(row[8]), default_output=row[9], plugin_status=signed(row[10]), plugin=row[11],
            serialization_status=signed(row[12]), serialization_present=row[13], category_status=signed(row[14]),
            category=row[15], mode_status=signed(row[16]), mode=row[17], active_ports_status=signed(row[18]),
            active_output_ports=row[19], connected_ports_status=signed(row[20]), connected_output_ports=row[21],
            active_port_types=row[22:24], devices={})
        for i, name in enumerate(names):
            d = row[32 + i * 16:48 + i * 16]
            snapshot['devices'][name] = dict(lookup_status=signed(d[0]), device=d[1], alive_status=signed(d[2]),
                alive=d[3], running_status=signed(d[4]), running=d[5], hog_status=signed(d[6]), hog_pid=signed(d[7]),
                output_streams_status=signed(d[8]), output_streams=d[9], output_activity_status=signed(d[10]),
                output_activity_bits=d[11], input_streams_status=signed(d[12]), input_streams=d[13],
                input_activity_status=signed(d[14]), input_activity_bits=d[15])
        result.append(snapshot)
    return result

def plugin_state(memory, symbols, layout, process, binary_path, probe=False):
    from kc_image import Image
    binary = Image(binary_path.read_bytes())
    command = binary.command(0x1b)
    uuid = binary.data[command[0] + 8:command[0] + 24]
    read64 = lambda address: int.from_bytes(memory.read(address, 8), 'little')
    task = int(process['address'], 16) + read64(symbols['_proc_struct_size'])
    vm_map = read64(task + layout['user']['task_map'])
    pmap = read64(vm_map + layout['user']['map_pmap'])
    root = read64(pmap + layout['user']['pmap_ttep'])
    read = lambda address, size: memory.read(address, size, root=root, bits=39)
    # XNU 11417 vm_map.h: hdr at +16; circular links {prev,next,start,end}.
    # Walk mapped Mach-O headers: dyld's image array can be paged out after boot.
    entry, seen, address = read64(vm_map + 24), set(), None
    while entry and entry != vm_map + 16 and entry not in seen and len(seen) < 20000:
        seen.add(entry)
        previous, following, start, end = struct.unpack('<4Q', memory.read(entry, 32))
        if start < 0x180000000 and end - start >= 4096:
            try:
                page = read(start, 4096)
                if page[:4] == b'\xcf\xfa\xed\xfe' and uuid in page:
                    address = start
                    break
            except ValueError:
                pass
        entry = following
    result = {'pid': process['pid'], 'base': hex(address) if address else None}
    if address:
        names = {name: location for location, names in binary.symbols().items() for name in names}
        fields = (('_marble_haptics_probe_results', 256), ('_marble_haptics_probe_devices', 1536),
                  ('_marble_haptics_probe_version', 4096)) if probe else (
            ('_diagnostics', 128), ('_companionUsers', 52), ('_companionSeed', 104), ('_streamInactive', 16),
            ('_users', 4), ('_speakerEndpoint', 4), ('_endpoint', 4), ('_users', 4), ('_propertyTrace', 4096), ('_propertyTraceCount', 4),
            ('_routingTraceCount', 4), ('_routingTrace', 4096),
            ('_audioRouteSnapshotCount', 4), ('_audioRouteSnapshots', 64 * 128 * 4), ('_audioRouteQuery', 24),
            ('_serverDiagnostics', 64), ('_serverError', 512), ('_routeDiagnostics', 128))
        for name, size in fields:
            if name in names:
                if name == '_propertyTrace':
                    count = int.from_bytes(read(address + names['_propertyTraceCount'] - binary.base, 4), 'little')
                    size = min(count, 256) * 16
                    if not size:
                        result[name] = []
                        continue
                if name == '_routingTrace':
                    count = int.from_bytes(read(address + names['_routingTraceCount'] - binary.base, 4), 'little')
                    size = min(count, 128) * 32
                    if not size:
                        result[name] = []
                        continue
                if name == '_audioRouteSnapshots':
                    count = result.get('_audioRouteSnapshotCount', [0])[0]
                    if not isinstance(count, int):
                        continue
                    # Observe the generation before and after reading each row.
                    # A ring row can be reused while firmware reads its pages.
                    words = [0] * (64 * 128)
                    for n in range(max(0, count - 64), count):
                        offset = (n % 64) * 512
                        at = address + names[name] - binary.base + offset
                        try:
                            before = int.from_bytes(read(at, 4), 'little')
                            data = read(at, 512)
                            after = int.from_bytes(read(at, 4), 'little')
                            if before == after == n * 2 + 2:
                                words[(n % 64) * 128:(n % 64 + 1) * 128] = struct.unpack('<128I', data)
                        except ValueError:
                            continue
                    result['audio_route_snapshots'] = decode_audio_route_snapshots(words, count)
                    continue
                try:
                    data = read(address + names[name] - binary.base, size)
                except ValueError as error:
                    result[name] = str(error)
                    continue
                if name == '_serverError':
                    result[name] = data.split(b'\0')[0].decode(errors='replace')
                elif name.endswith('_version'):
                    result[name] = [data[:256].split(b'\0')[0].decode(errors='replace'),
                                    data[256:2048].split(b'\0')[0].decode(errors='replace'),
                                    data[2048:].split(b'\0')[0].decode(errors='replace')]
                elif name.endswith('_devices'):
                    result[name] = [data[i:i + 96].split(b'\0')[0].decode(errors='replace')
                                    for i in range(0, len(data), 96)]
                else:
                    result[name] = list(struct.unpack('<' + 'I' * (size // 4), data))
    return result


def decode_audio_probe(p):
    signed = lambda value: value - (1 << 32) if value & 0x80000000 else value
    pins = ('gpio121_sck', 'gpio122_data0', 'gpio123_ws', 'gpio124_data1')
    result = {'signal_samples': p[12], 'silence_samples': p[13],
        'signal_transitions': dict(zip(pins, p[0:4])), 'silence_transitions': dict(zip(pins, p[4:8])),
        'signal_high_samples': dict(zip(pins, p[8:12]))}
    for amp in range(2):
        a = p[16 + amp * 24:40 + amp * 24]
        result[f'amp{amp}'] = {'signal_polls': a[0], 'silence_polls': a[1],
            'signal_isn': [signed(a[2]), signed(a[3])], 'signal_vsn': [signed(a[4]), signed(a[5])],
            'silence_isn': [signed(a[6]), signed(a[7])], 'silence_vsn': [signed(a[8]), signed(a[9])],
            'signal_bstvout_max': a[10], 'silence_bstvout_max': a[11],
            'signal_sysst_or': hex(a[12]), 'signal_sysst_and': hex(a[13]), 'sysint_or': hex(a[14]),
            'i2sint_or': hex(a[15]), 'i2scapcnt': hex(a[16]), 'anasta1': hex(a[17]), 'hagcst': hex(a[18]),
            'isn_last': signed(a[19]), 'vsn_last': signed(a[20]), 'sysst_last': hex(a[21]), 'i2c_errors': a[23]}
    return result

def capture(kernel, output, plugin=None, probe=None):
    output.mkdir(parents=True, exist_ok=True)
    symbols = kernel_symbols(kernel, DEVICE / 'ios18/sources/XnuPrelink')
    report = {'captured_at': datetime.now().astimezone().isoformat(), 'kernel': str(kernel),
        'kernel_sha256': hashlib.sha256(kernel.read_bytes()).hexdigest()}
    with (Path(tempfile.gettempdir()) / 'marble-rootstage-observer.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        channel = RootUsb(timeout_ms=5000)
        try:
            report['phase'] = channel.command('getvar:phase')
            memory = memory_for(channel, symbols)
            for name, count in (('_marble_haptics_stage', 1), ('_marble_haptics_play_count', 1),
                ('_marble_haptics_stats', 64), ('_marble_haptics_runtime_stats', 32),
                ('_marble_audio_stats', 64), ('_marble_audio_amp_registers', 256),
                ('_marble_audio_messages', 256), ('_marble_audio_gpio', 256),
                ('_marble_audio_smmu', 8), ('_marble_audio_probe', 64),
                ('_marble_haptics_overlay_stage', 2), ('_marble_haptics_overlay_status', 2)):
                if name in symbols:
                    values = list(struct.unpack('<' + 'I' * count, memory.read(symbols[name], count * 4)))
                    report[name] = values[0] if count == 1 else values
            if '_marble_audio_probe' in report:
                report['audio_probe'] = decode_audio_probe(report['_marble_audio_probe'])
            if '_marble_haptics_runtime_stats' in report:
                report['runtime'] = dict(zip(RUNTIME_FIELDS, report['_marble_haptics_runtime_stats']))
            address, = struct.unpack('<Q', memory.read(symbols['_msgbufp'], 8))
            magic, size, cursor, reader, data = struct.unpack('<4IQ', memory.read(address, 24))
            if magic != 0x063061 or not 0 < size <= 1048576: raise ValueError('Invalid kernel message buffer')
            ring = memory.read(data, size)
            log = (ring[cursor:] + ring[:cursor]).lstrip(b'\0')
            (output / 'kernel.log').write_bytes(log)
            (output / 'haptics.log').write_text('\n'.join(line[:2000] for line in log.decode(errors='replace').splitlines()
                if any(word in line.lower() for word in ('haptic', 'actuator', 'virtualaudio', 'audiomxd')))+'\n')
            layout = json.loads((DEVICE.parents[1] / 'outputs/UniXNU_marble/ios18_arm64e/kernel.layout.json').read_text())
            processes = capture_processes(memory, symbols, layout['proc'])
            (output / 'processes.json').write_text(json.dumps(processes, indent=2)+'\n')
            report['audio_processes'] = [p for p in processes if any(word in p['name'].lower()
                for word in ('audio', 'haptic', 'backboard', 'springboard'))]
            audio = next((p for p in processes if p['name'] == 'audiomxd'), None)
            if plugin and audio:
                try:
                    report['plugin'] = plugin_state(memory, symbols, layout, audio, plugin)
                except (ValueError, IOError) as error:
                    report['plugin_error'] = str(error)
            probe_process = next((p for p in processes if p['name'].startswith('marble-haptics-p')), None)
            if probe and probe_process:
                try:
                    report['probe'] = plugin_state(memory, symbols, layout, probe_process, probe, True)
                except (ValueError, IOError) as error:
                    report['probe_error'] = str(error)
        finally: channel.close()
    (output / 'haptics.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report.get('runtime', report), indent=2))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--plugin', type=Path, help='Matching HAL binary for its loaded UUID and live diagnostics')
    parser.add_argument('--probe', type=Path, help='Diagnostic system/CoreHaptics test executable')
    args = parser.parse_args()
    capture(args.kernel.resolve(), args.output.resolve(), args.plugin.resolve() if args.plugin else None,
            args.probe.resolve() if args.probe else None)
