#!/usr/bin/env python3
"""Capture the resident GPU trace and optional home-screen swipe interval."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import time

from capture_ios18_resident import kernel_symbols
from root_control import RootUsb
from root_ui import RootUI, memory_for


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--swipes', default=0, type=int)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    symbols = kernel_symbols(args.kernel.resolve())
    channel = RootUsb(timeout_ms=5000)
    try:
        memory = memory_for(channel, symbols)
        store = symbols['__ZL8gpuTrace']
        slots, capacity, first, dropped = struct.unpack('<QI4xQQ', memory.read(store, 32))
        if not slots or not 0 < capacity <= 65536:
            raise RuntimeError('GPU trace storage is unavailable')
        began = time.monotonic()
        windows = []
        if args.swipes:
            ui = RootUI(channel)
            for index in range(args.swipes):
                start_sequence = struct.unpack('<QI4xQQ', memory.read(store, 32))[2]
                left, right = (950, 130) if index % 2 == 0 else (130, 950)
                ui.swipe(left, 1450, right, 1450, 400)
                end_sequence = struct.unpack('<QI4xQQ', memory.read(store, 32))[2]
                windows.append(dict(index=index, start_sequence=start_sequence, end_sequence=end_sequence))
                time.sleep(0.8)
        elapsed = time.monotonic() - began
        _, _, last, dropped = struct.unpack('<QI4xQQ', memory.read(store, 32))
        raw = memory.read(slots, capacity * 128)
        (args.output / 'slots.bin').write_bytes(raw)
        events = []
        for offset in range(0, len(raw), 128):
            busy, = struct.unpack_from('<I', raw, offset)
            seq, us, event, pid, *values = struct.unpack_from('<QQII12Q', raw, offset + 8)
            if not busy and seq and seq <= last and (not args.swipes or seq > first):
                events.append(dict(sequence=seq, us=us, event=event, pid=pid, values=values))
        events.sort(key=lambda entry: entry['sequence'])
        (args.output / 'events.json').write_text(json.dumps(events, indent=2) + '\n')
        summary = dict(first_sequence=first, last_sequence=last, dropped=dropped,
                       elapsed_seconds=elapsed, swipes=args.swipes,
                       motion_windows=windows,
                       event_counts=dict(Counter(entry['event'] for entry in events)))
        retire = [entry['us'] for entry in events if entry['event'] == 20 and entry['values'][1] == 6]
        intervals = sorted((b - a) / 1000 for a, b in zip(retire, retire[1:]))
        if intervals:
            summary['swap_retirement'] = dict(count=len(retire),
                mean_ms=sum(intervals) / len(intervals),
                p50_ms=intervals[len(intervals) // 2],
                p95_ms=intervals[min(len(intervals) - 1, int(len(intervals) * 0.95))],
                p99_ms=intervals[min(len(intervals) - 1, int(len(intervals) * 0.99))],
                maximum_ms=intervals[-1])
        motion_intervals = []
        for window in windows:
            times = [entry['us'] for entry in events if entry['event'] == 20 and entry['values'][1] == 6
                     and window['start_sequence'] < entry['sequence'] <= window['end_sequence']]
            motion_intervals.extend((b - a) / 1000 for a, b in zip(times, times[1:]))
        if motion_intervals:
            motion_intervals.sort()
            summary['motion_swap_retirement'] = dict(intervals=len(motion_intervals),
                mean_ms=sum(motion_intervals) / len(motion_intervals),
                p99_ms=motion_intervals[min(len(motion_intervals)-1, int(len(motion_intervals)*0.99))],
                over_16_667_ms=sum(value > 1000/60 for value in motion_intervals))
        (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary))
    finally:
        channel.close()


if __name__ == '__main__':
    main()
