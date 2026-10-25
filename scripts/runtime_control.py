#!/usr/bin/env python3
"""Read live marble kernel diagnostics through its EP0 USB interface."""
import argparse
import json
from pathlib import Path
import struct
import time
import zlib

import usb.backend.libusb1
import usb.core
import usb.util


def read(device, kind, offset, length):
    result = bytearray()
    while len(result) < length:
        at = offset + len(result)
        chunk = device.ctrl_transfer(0xc0, kind, at & 65535, at >> 16,
                                     min(4096, length - len(result)))
        if not chunk:
            raise RuntimeError(f'Short runtime USB read at {at}')
        result.extend(chunk)
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['status', 'logs', 'screen', 'surface', 'home', 'touch', 'gpu', 'adreno', 'hide_display', 'show_display', 'tap', 'swipe', 'trace', 'trace_start', 'metal_cache', 'return'])
    parser.add_argument('--x', type=int)
    parser.add_argument('--y', type=int)
    parser.add_argument('--to-x', type=int)
    parser.add_argument('--to-y', type=int)
    parser.add_argument('--duration-ms', type=int, default=400)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--source', action='store_true', help='Capture the compositor surface before scanout copy')
    parser.add_argument('--follow', action='store_true', help='Keep reading the log ring until the timeout or USB loss')
    parser.add_argument('--seconds', type=float, default=30, help='Follow duration')
    parser.add_argument('--interval', type=float, default=0.5, help='Follow poll interval')
    parser.add_argument('--trigger', choices=['adreno'], help='Send this request once while following')
    parser.add_argument('--trigger-after', type=float, default=3, help='Seconds before the trigger')
    args = parser.parse_args()
    backend = usb.backend.libusb1.get_backend(find_library=lambda _: '/opt/homebrew/lib/libusb-1.0.dylib')
    device = usb.core.find(idVendor=0x18d1, idProduct=0xd00e, backend=backend)
    if device is None or device.serial_number != 'marble':
        raise SystemExit('Marble runtime USB is unavailable')
    device.default_timeout = 5000
    try:
        if args.command == 'swipe':
            if (None in (args.x, args.y, args.to_x, args.to_y) or
                    not 0 <= args.x < 1080 or not 0 <= args.to_x < 1080 or
                    not 0 <= args.y < 2400 or not 0 <= args.to_y < 2400 or args.duration_ms <= 0):
                parser.error('swipe requires --x, --y, --to-x, --to-y within the screen and a positive duration')
            steps = max(2, args.duration_ms // 20)
            began = time.monotonic()
            try:
                for step in range(steps + 1):
                    x = args.x + (args.to_x - args.x) * step // steps
                    y = args.y + (args.to_y - args.y) * step // steps
                    device.ctrl_transfer(0x40, 12, x, y, b'')
                    pause = began + args.duration_ms / 1000 * (step + 1) / steps - time.monotonic()
                    if step < steps and pause > 0:
                        time.sleep(pause)
            finally:
                device.ctrl_transfer(0x40, 13, args.to_x, args.to_y, b'')
            print(f'swipe {args.x},{args.y} -> {args.to_x},{args.to_y}')
        elif args.command == 'tap':
            if args.x is None or args.y is None or not 0 <= args.x < 1080 or not 0 <= args.y < 2400:
                parser.error('tap requires --x 0..1079 and --y 0..2399')
            device.ctrl_transfer(0x40, 10, args.x, args.y, b'')
            print(f'tap x={args.x} y={args.y}')
        elif args.command in ('hide_display', 'show_display'):
            device.ctrl_transfer(0x40, 15, int(args.command == 'hide_display'), 0, b'')
            print(args.command)
        elif args.command in ('home', 'touch', 'gpu', 'adreno', 'return', 'trace_start'):
            device.ctrl_transfer(0x40, {'home': 3, 'return': 4, 'touch': 8, 'gpu': 9, 'trace_start': 11, 'adreno': 14}[args.command], 0, 0, b'')
            print(args.command)
        elif args.command == 'trace':
            magic, version, length = struct.unpack('<8sII', read(device, 8, 0, 16))
            if magic != b'ROOTTRC1' or version != 1 or length > 229360:
                raise RuntimeError('Runtime trace is not ready')
            text = read(device, 8, 16, length).decode(errors='replace')
            if args.output:
                args.output.write_text(text)
                print(args.output)
            else:
                print(text)
        elif args.command == 'metal_cache':
            magic, generation, length = struct.unpack('<8sII', read(device, 16, 0, 16))
            if magic != b'ROOTMTC1' or not length:
                raise RuntimeError('Metal pipeline cache export is not ready')
            if args.output is None:
                parser.error('metal_cache requires --output')
            args.output.write_bytes(read(device, 16, 16, length))
            print(json.dumps(dict(generation=generation, bytes=length, output=str(args.output))))
        elif args.command == 'status':
            fields=struct.unpack('<8s6Q',read(device,5,0,56))
            if fields[0] != b'ROOTRUN1':
                raise RuntimeError('Invalid runtime status header')
            print(json.dumps(dict(protocol=device.product, serial=device.serial_number,
                counter_seconds=fields[1]/1e9, first_frame_counter_seconds=fields[2]/1e9,
                reads=fields[3], frames=fields[4], data_resident_bytes=fields[5], data_logical_bytes=fields[6]),indent=2))
        elif args.command == 'surface':
            data=read(device,7,0,48+16*32*4)
            magic,width,height,format,planes,stride,frame,columns,rows=struct.unpack_from('<8s4I2Q2I',data)
            if magic != b'ROOTSUR1':
                raise RuntimeError('No compositor surface captured')
            pixels=struct.unpack_from('<512I',data,48)
            result=dict(width=width,height=height,format=hex(format),planes=planes,stride=stride,frame=frame,
                columns=columns,rows=rows,pixels=[f'{pixel:08x}' for pixel in pixels],
                white_alpha_zero=sum((p&0xffffff)==0xffffff and p>>24==0 for p in pixels),
                white_alpha_opaque=sum(p==0xffffffff for p in pixels),
                nonpremultiplied=sum(max(p&255,(p>>8)&255,(p>>16)&255)>p>>24 for p in pixels))
            text=json.dumps(result,indent=2)
            if args.output: args.output.write_text(text+'\n')
            else: print(text)
        elif args.command == 'logs':
            def snapshot():
                magic, version, length = struct.unpack('<8sII', read(device, 1, 0, 16))
                if magic != b'ROOTLOG1' or version != 1 or length > 65520:
                    raise RuntimeError('Invalid runtime log header')
                return read(device, 1, 16, length).decode(errors='replace')
            if args.follow:
                if args.output:
                    args.output.write_text('')
                def emit(markup):
                    if args.output:
                        with args.output.open('a') as handle:
                            handle.write(markup)
                    return markup
                def delta_of(text):
                    nonlocal previous
                    if previous and text.startswith(previous):
                        change = text[len(previous):]
                    else:
                        change = None
                        if previous:
                            for size in (4096, 1024, 256):
                                at = text.find(previous[-size:])
                                if at >= 0:
                                    change = text[at + size:]
                                    break
                        if change is None:
                            change = ('\n[log restarted]\n' if previous else '') + text
                    previous = text
                    return change
                began = time.monotonic()
                deadline = began + args.seconds
                previous, tail, fired = '', [], False
                while time.monotonic() < deadline:
                    if args.trigger and not fired and time.monotonic() - began >= args.trigger_after:
                        fired = True
                        stamp = f'\n[trigger {args.trigger} at {time.monotonic() - began:.1f}s: '
                        try:
                            device.ctrl_transfer(0x40, {'adreno': 14}[args.trigger], 0, 0, b'')
                            tail.append(emit(stamp + 'sent]\n'))
                        except Exception as error:
                            tail.append(emit(stamp + f'failed: {error}]\n'))
                            break
                    try:
                        text = snapshot()
                    except Exception as error:
                        tail.append(emit(f'\n[runtime USB ended: {error}]\n'))
                        break
                    change = delta_of(text)
                    if change:
                        tail.append(emit(change))
                    time.sleep(args.interval)
                text = ''.join(tail)
            else:
                text = snapshot()
                if args.output:
                    args.output.write_text(text)
            if args.output:
                print(args.output)
            else:
                print(text)
        else:
            kind = 6 if args.source else 2
            header = read(device, kind, 0, 32)
            magic, width, height, stride, length, frames = struct.unpack('<8sIIIIQ', header)
            if magic != b'ROOTRGB1' or not width or not height or stride != width * 3 or length != stride * height:
                raise RuntimeError('Invalid runtime screen header')
            if not args.output:
                parser.error('screen requires --output')
            pixels = read(device, kind, 32, length)
            def chunk(kind, payload):
                return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
            scanlines = b''.join(b'\0' + pixels[y * stride:(y + 1) * stride] for y in range(height))
            args.output.write_bytes(b'\x89PNG\r\n\x1a\n' +
                chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(scanlines)) + chunk(b'IEND', b''))
            print(json.dumps(dict(output=str(args.output), width=width, height=height, frames=frames), indent=2))
    finally:
        usb.util.dispose_resources(device)


if __name__ == '__main__':
    main()
