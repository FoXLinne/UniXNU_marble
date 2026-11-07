#!/usr/bin/env python3
"""Read or synchronize the iOS calendar through the resident RTC bridge."""
import argparse
import datetime
import json
import math
from pathlib import Path
import struct
import time

from capture_ios18_resident import kernel_symbols
from root_control import RootUsb
from root_ui import memory_for


def sample(kernel, synchronize=False):
    symbols = kernel_symbols(kernel)
    channel = RootUsb(timeout_ms=5000)
    try:
        if synchronize:
            target = math.ceil(time.time())
            time.sleep(max(0, target - time.time()))
        began = time.time()
        command = f'oem rtc-sync:{target:016x}' if synchronize else 'oem rtc-sample'
        channel.command(command)
        time.sleep(0.25)
        if int(channel.command('getvar:control'), 16):
            raise RuntimeError('Kernel clock request is pending')
        memory = memory_for(channel, symbols)
        values = struct.unpack('<6Q', memory.read(symbols['_marble_rtc_observation'], 48))
        persisted = int.from_bytes(memory.read(symbols['_marble_rtc_persist'], 4), 'little')
        ended = time.time()
        rtc, status = (int(value, 16) for value in channel.command('getvar:rtc').split(':'))
        seconds, micros, provider, provider_micros, available, error = values
        system_time = seconds + micros / 1_000_000
        return {
            'host_before': began, 'host_after': ended,
            'system_unix': system_time,
            'system_utc': datetime.datetime.fromtimestamp(system_time, datetime.timezone.utc).isoformat(),
            'provider_unix': provider + provider_micros / 1_000_000,
            'provider_available': available, 'bridge_error': error,
            'calibration_write_status': persisted,
            'firmware_unix': rtc, 'firmware_status': status,
            'host_delta_seconds': system_time - (began + ended) / 2,
            'within_one_second': began - 1 <= system_time <= ended + 1,
        }
    finally:
        channel.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['sample', 'sync'])
    parser.add_argument('--kernel', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(sample(args.kernel, args.command == 'sync'), indent=2))


if __name__ == '__main__':
    main()
