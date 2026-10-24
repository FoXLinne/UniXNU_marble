#!/usr/bin/env python3
"""Read live marble kernel diagnostics through its EP0 USB interface."""
import argparse
import json
from pathlib import Path
import struct
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
    parser.add_argument('command', choices=['status', 'logs', 'screen', 'surface', 'home', 'return'])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--source', action='store_true', help='Capture the compositor surface before scanout copy')
    args = parser.parse_args()
    backend = usb.backend.libusb1.get_backend(find_library=lambda _: '/opt/homebrew/lib/libusb-1.0.dylib')
    device = usb.core.find(idVendor=0x18d1, idProduct=0xd00e, backend=backend)
    if device is None or device.serial_number != 'marble':
        raise SystemExit('Marble runtime USB is unavailable')
    device.default_timeout = 5000
    try:
        if args.command in ('home', 'return'):
            device.ctrl_transfer(0x40, 3 if args.command == 'home' else 4, 0, 0, b'')
            print(args.command)
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
            header = read(device, 1, 0, 16)
            magic, version, length = struct.unpack('<8sII', header)
            if magic != b'ROOTLOG1' or version != 1 or length > 65520:
                raise RuntimeError('Invalid runtime log header')
            text = read(device, 1, 16, length).decode(errors='replace')
            if args.output:
                args.output.write_text(text)
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
