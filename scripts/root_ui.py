#!/usr/bin/env python3
"""Control live iOS input and capture scanout through the resident RootStage service."""
import argparse
import json
from pathlib import Path
import struct
import time
import zlib

from root_control import RootUsb
from capture_ios18_resident import RetainedKernel, kernel_symbols, png_chunk


def memory_for(channel, symbols):
    if channel.command('getvar:phase') == 'firmware':
        return RetainedKernel(channel)
    base, size = (int(v, 16) for v in channel.command('getvar:kernel-memory').split(':'))
    address = base + 0x4000 + symbols['_cpu_ttep'] - 0xfffffff006e04000
    channel.command(f'oem memory-read:{address:016x}:00000008')
    memory = object.__new__(RetainedKernel)
    memory.channel, memory.base, memory.size = channel, base, size
    memory.ttbr1 = int.from_bytes(channel.read_back(), 'little')
    memory.bits, memory.tcr, memory.tables = 39, 25 | (25 << 16), {}
    if not memory.ttbr1:
        raise RuntimeError('Live kernel translation root is zero')
    return memory


class RootUI:
    def __init__(self, channel):
        self.channel = channel
        if channel.command('getvar:phase') != 'kernel':
            raise RuntimeError('Input requires the running kernel')
        if 'input-control' not in channel.command('getvar:features').split(','):
            raise RuntimeError('Firmware input-control capability is absent')

    def wait_idle(self, seconds=3):
        deadline = time.monotonic() + seconds
        while int(self.channel.command('getvar:control'), 16):
            if time.monotonic() >= deadline:
                raise TimeoutError('Kernel input acknowledgement timed out')
            time.sleep(0.02)

    def input(self, operation, x=0, y=0):
        if not 0 <= x < 1080 or not 0 <= y < 2400:
            raise ValueError('Input coordinate outside 1080x2400')
        self.wait_idle()
        self.channel.command(f'oem input:{operation:02x}:{x:04x}:{y:04x}')

    def contact(self, x, y, active):
        self.input(2 if active else 3, x, y)

    def tap(self, x, y):
        self.input(1, x, y)
        self.wait_idle()

    def swipe(self, x, y, to_x, to_y, duration_ms, hold_ms=0):
        steps = max(2, duration_ms // 8)
        began = time.monotonic()
        try:
            for step in range(steps + 1):
                self.contact(x + (to_x-x)*step//steps, y + (to_y-y)*step//steps, True)
                if step < steps:
                    time.sleep(max(0, began + duration_ms/1000*(step+1)/steps - time.monotonic()))
            if hold_ms:
                time.sleep(hold_ms/1000)
        finally:
            self.contact(to_x, to_y, False)
            self.wait_idle()

    def screenshot(self, kernel, output):
        self.input(4)
        self.wait_idle()
        symbols = kernel_symbols(kernel)
        memory = memory_for(self.channel, symbols)
        width, height, stride = struct.unpack('<3I', memory.read(symbols['_marble_display_geometry'], 12))
        pixels = int.from_bytes(memory.read(symbols['_marble_display_pixels'], 8), 'little')
        if not pixels or not 0 < width <= 1080 or not 0 < height <= 2400 or stride < width*4:
            raise RuntimeError('Live display geometry is unavailable')
        raw = memory.read(pixels, stride*height)
        rows = bytearray()
        for y in range(height):
            source = raw[y*stride:y*stride+width*4]
            row = bytearray(width*3)
            row[0::3], row[1::3], row[2::3] = source[2::4], source[1::4], source[0::4]
            rows.extend(b'\0' + row)
        png = b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 2, 0, 0, 0))
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(png + png_chunk(b'IDAT', zlib.compress(rows)) + png_chunk(b'IEND', b''))
        return dict(output=str(output), width=width, height=height)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['tap', 'swipe', 'longpress', 'home', 'switcher', 'screen'])
    parser.add_argument('--x', type=int)
    parser.add_argument('--y', type=int)
    parser.add_argument('--to-x', type=int)
    parser.add_argument('--to-y', type=int)
    parser.add_argument('--duration-ms', type=int, default=400)
    parser.add_argument('--hold-ms', type=int, default=800)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    c = RootUsb(timeout_ms=5000)
    try:
        ui = RootUI(c)
        if args.command == 'screen':
            if args.kernel is None or args.output is None:
                parser.error('screen requires --kernel and --output')
            print(json.dumps(ui.screenshot(args.kernel.resolve(), args.output.resolve())))
        elif args.command in ('home', 'switcher'):
            ui.swipe(540, 2385, 540, 1400, 400, args.hold_ms if args.command == 'switcher' else 0)
        elif args.command == 'swipe':
            if None in (args.x, args.y, args.to_x, args.to_y):
                parser.error('swipe requires start and end coordinates')
            ui.swipe(args.x, args.y, args.to_x, args.to_y, args.duration_ms)
        elif args.command == 'longpress':
            if None in (args.x, args.y):
                parser.error('longpress requires --x and --y')
            ui.contact(args.x, args.y, True)
            time.sleep(args.hold_ms/1000)
            ui.contact(args.x, args.y, False)
            ui.wait_idle()
        else:
            if None in (args.x, args.y):
                parser.error('tap requires --x and --y')
            ui.tap(args.x, args.y)
    finally:
        c.close()


if __name__ == '__main__':
    main()
