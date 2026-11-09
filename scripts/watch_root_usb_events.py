#!/usr/bin/env python3
"""Record USB attach/detach notifications without timed device scans."""
import argparse
import ctypes as c
import json
from pathlib import Path
import time

import usb.core
import usb.util
import usb.backend.libusb1


def watch(output, duration=0):
    library = c.CDLL('/opt/homebrew/lib/libusb-1.0.dylib')
    context = c.c_void_p()
    library.libusb_init.argtypes = [c.POINTER(c.c_void_p)]
    if library.libusb_init(c.byref(context)):
        raise RuntimeError('USB event context initialization failed')

    class Descriptor(c.Structure):
        _fields_ = [('length', c.c_uint8), ('type', c.c_uint8), ('usb', c.c_uint16),
                    ('cls', c.c_uint8), ('subclass', c.c_uint8), ('protocol', c.c_uint8),
                    ('ep0', c.c_uint8), ('vid', c.c_uint16), ('pid', c.c_uint16),
                    ('release', c.c_uint16), ('manufacturer', c.c_uint8),
                    ('product', c.c_uint8), ('serial', c.c_uint8), ('configs', c.c_uint8)]

    callback_type = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_void_p, c.c_int, c.c_void_p)
    library.libusb_get_device_descriptor.argtypes = [c.c_void_p, c.POINTER(Descriptor)]
    library.libusb_get_device_address.argtypes = [c.c_void_p]
    pending = []

    @callback_type
    def event_callback(ctx, device, event, user):
        descriptor = Descriptor()
        if not library.libusb_get_device_descriptor(device, c.byref(descriptor)):
            if descriptor.vid not in (0x18d1,0x05ac):
                return 0
            pending.append(dict(event='attach' if event == 1 else 'detach',
                                vid=f'{descriptor.vid:04x}',
                                pid=f'{descriptor.pid:04x}',
                                address=library.libusb_get_device_address(device),
                                monotonic=time.monotonic()))
        return 0

    library.libusb_hotplug_register_callback.argtypes = [c.c_void_p, c.c_int, c.c_int,
        c.c_int, c.c_int, c.c_int, callback_type, c.c_void_p, c.POINTER(c.c_int)]
    handle = c.c_int()
    result = library.libusb_hotplug_register_callback(context, 3, 1, -1, -1, -1,
                                                     event_callback, None, c.byref(handle))
    if result:
        raise RuntimeError(f'USB notification registration failed: {result}')
    library.libusb_handle_events.argtypes = [c.c_void_p]
    class Timeval(c.Structure):
        _fields_ = [('seconds', c.c_long), ('microseconds', c.c_long)]
    library.libusb_handle_events_timeout_completed.argtypes = [c.c_void_p, c.POINTER(Timeval), c.c_void_p]
    backend = usb.backend.libusb1.get_backend(find_library=lambda _: '/opt/homebrew/lib/libusb-1.0.dylib')
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('a', buffering=1) as stream:
        began = time.monotonic()
        while not duration or time.monotonic() - began < duration:
            while pending:
                event = pending.pop(0)
                if event['event'] == 'attach' and event['vid']=='18d1':
                    device = usb.core.find(idVendor=0x18d1, idProduct=int(event['pid'], 16), backend=backend)
                    if device is not None:
                        try:
                            event['serial'] = device.serial_number
                        except (ValueError, usb.core.USBError) as error:
                            event['descriptor_error'] = str(error)
                        finally:
                            usb.util.dispose_resources(device)
                line = json.dumps(event)
                print(line, flush=True)
                stream.write(line + '\n')
            if duration:
                timeout = Timeval(1, 0)
                result = library.libusb_handle_events_timeout_completed(context, c.byref(timeout), None)
            else:
                result = library.libusb_handle_events(context)
            if result not in (0, -10):
                raise RuntimeError(f'USB event delivery failed: {result}')
    library.libusb_hotplug_deregister_callback.argtypes = [c.c_void_p, c.c_int]
    library.libusb_hotplug_deregister_callback(context, handle.value)
    library.libusb_exit.argtypes = [c.c_void_p]
    library.libusb_exit(context)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--duration', type=float, default=0, help='Stop after this many seconds; zero keeps watching')
    args = parser.parse_args()
    watch(args.output.resolve(), args.duration)
