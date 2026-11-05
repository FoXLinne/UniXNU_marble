#!/usr/bin/env python3
"""Find marble's resident return and verify USB across an idle interval."""
import argparse
import json
from pathlib import Path
import time

import usb.backend.libusb1
import usb.core
import usb.util

from root_control import RootUsb
from capture_ios18_resident import capture


def verify(kernel, output, timeout=180, hold=90, settle=45):
    output.mkdir(parents=True, exist_ok=True)
    began = time.monotonic()
    report = dict(completed=False, observations=[], communication=[])
    channel = None

    def save():
        (output / 'usb-verification.json').write_text(json.dumps(report, indent=2) + '\n')

    backend = usb.backend.libusb1.get_backend(
        find_library=lambda _: '/opt/homebrew/lib/libusb-1.0.dylib')
    try:
        previous = None
        while time.monotonic() - began < timeout:
            state = []
            selected = None
            for device in usb.core.find(find_all=True, idVendor=0x18d1, backend=backend):
                try:
                    serial = device.serial_number
                except (ValueError, usb.core.USBError) as error:
                    state.append(dict(pid=f'{device.idProduct:04x}', enumerating=str(error)))
                    usb.util.dispose_resources(device)
                    continue
                state.append(dict(pid=f'{device.idProduct:04x}', serial=serial))
                if device.idProduct == 0xd00d and serial == 'marble':
                    selected = device
                else:
                    usb.util.dispose_resources(device)
            if state != previous:
                report['observations'].append(dict(seconds=time.monotonic()-began, devices=state))
                print(json.dumps(report['observations'][-1]), flush=True)
                previous = state
                save()
            # The loader exposes the same descriptors before entering XNU.
            # Give the UFS boot its handoff interval before issuing commands.
            if selected is not None and time.monotonic() - began >= settle:
                channel = RootUsb(device=selected, timeout_ms=5000)
                returns = int(channel.command('getvar:returns'), 16)
                if returns:
                    break
                channel.close()
                channel = None
            elif selected is not None:
                usb.util.dispose_resources(selected)
            time.sleep(5)
        else:
            raise TimeoutError('RootStage resident return deadline expired')

        capture_output = output / 'capture'
        capture_output.mkdir(exist_ok=True)
        report['capture'] = capture(channel, kernel, capture_output)
        held = time.monotonic()
        for at in (0, 15, 45, hold):
            delay = held + at - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            sample = dict(seconds=time.monotonic()-held)
            for key in ('version', 'returns', 'watchdog', 'kernel-memory'):
                sample[key] = channel.command('getvar:' + key)
            if sample['version'] != 'RootStage2' or int(sample['returns'], 16) != returns:
                raise RuntimeError('Resident session changed during the USB hold')
            report['communication'].append(sample)
            print(json.dumps(sample), flush=True)
            save()
        report['completed'] = report['capture']['completed']
        save()
        return report
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
        save()
        raise
    finally:
        if channel is not None:
            channel.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--settle-seconds', type=int, default=45)
    args = parser.parse_args()
    verify(args.kernel.resolve(), args.output.resolve(), settle=args.settle_seconds)
