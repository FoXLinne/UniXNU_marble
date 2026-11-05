#!/usr/bin/env python3
"""Measure firmware DSI interface counters through the resident USB service."""
import argparse
import json
from pathlib import Path
import struct
import time

from root_control import RootUsb
from root_ui import RootUI, memory_for
from capture_ios18_resident import kernel_symbols


def sample(channel, memory, symbols):
    if channel.command('getvar:display-timing') != 'kernel':
        raise ValueError('Display timing requires the kernel observation backend')
    ui = RootUI(channel)
    ui.input(5)
    ui.wait_idle()
    deadline = time.monotonic() + 3
    while True:
        state = int.from_bytes(memory.read(symbols['_marble_display_timing_state'], 4), 'little')
        if state == 2:
            break
        if state == 3:
            error = int.from_bytes(memory.read(symbols['_marble_display_timing_error'], 4), 'little')
            raise RuntimeError(f'Display timing request returned {error:#x}')
        if time.monotonic() >= deadline:
            raise TimeoutError('Kernel display timing request timed out')
        time.sleep(0.1)
    words = struct.unpack('<17Q', memory.read(symbols['_marble_display_timing'], 17 * 8))
    if len(words) != 17 or not words[1] or words[-1] < words[0]:
        raise ValueError('Display timing reply has an invalid layout')
    fields = ('status', 'timing_enable', 'count_enable', 'frame', 'line', 'hsync', 'vsync_period')
    return dict(begin_ticks=words[0], frequency=words[1], end_ticks=words[-1],
                interfaces=[dict(zip(fields, words[2 + index * 7:9 + index * 7])) for index in range(2)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=3)
    parser.add_argument('--interval', type=float, default=2)
    args = parser.parse_args()
    if args.samples < 2 or args.interval < 1:
        parser.error('Use at least two samples with an interval of one second or greater')
    channel = RootUsb(timeout_ms=5000)
    try:
        symbols = kernel_symbols(args.kernel.resolve())
        memory = memory_for(channel, symbols)
        samples = []
        for index in range(args.samples):
            if index:
                time.sleep(args.interval)
            samples.append(sample(channel, memory, symbols))
    finally:
        channel.close()
    rates = []
    for before, after in zip(samples, samples[1:]):
        if before['frequency'] != after['frequency']:
            raise ValueError('Counter clock frequency changed between samples')
        seconds = ((after['begin_ticks'] + after['end_ticks']) -
                   (before['begin_ticks'] + before['end_ticks'])) / (2 * before['frequency'])
        interfaces = []
        for first, last in zip(before['interfaces'], after['interfaces']):
            frames = (last['frame'] - first['frame']) & 0xffffffff
            stable = all(first[key] == last[key] for key in ('timing_enable', 'count_enable', 'hsync', 'vsync_period'))
            interfaces.append(dict(frames=frames, seconds=seconds, stable_mode=stable,
                                   measured_hz=frames / seconds if stable and frames else None))
        rates.append(interfaces)
    report = dict(samples=samples, intervals=rates)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
