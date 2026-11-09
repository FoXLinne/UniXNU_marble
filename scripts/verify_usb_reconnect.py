#!/usr/bin/env python3
"""Check resident USB reconnects before XNU, in XNU, and after resident return."""
import argparse
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import time

from root_control import RootUsb
from capture_ios18_resident import RetainedKernel, kernel_symbols

DEVICE = Path(__file__).resolve().parents[1]


def topology():
    tree = plistlib.loads(subprocess.check_output(['ioreg', '-p', 'IOUSB', '-a', '-l']))

    def visit(node, hub=None):
        if isinstance(node, list):
            for item in node:
                result = visit(item, hub)
                if result:
                    return result
        elif isinstance(node, dict):
            location = node.get('locationID', 0)
            if node.get('idVendor') == 0x18d1 and node.get('USB Serial Number') == 'marble':
                if hub is None:
                    raise RuntimeError('The resident device needs an individually powered hub port')
                difference = location ^ hub
                shift = ((difference.bit_length() - 1) // 4) * 4
                return dict(location=location, hub=hub, port=(location >> shift) & 15)
            if node.get('bDeviceClass') == 9:
                hub = location
            return visit(node.get('IORegistryEntryChildren', []), hub)
        return None

    result = visit(tree)
    if result is None:
        raise RuntimeError('Resident USB topology is unavailable')
    return result


def sample(channel):
    result = {key: channel.command('getvar:' + key) for key in
              ('version', 'phase', 'returns', 'usb-session', 'watchdog', 'usb-affinity', 'kernel-ready')}
    values = [int(value, 16) for value in channel.command('getvar:usb-stats').split(':')]
    if len(values) != 10:
        raise RuntimeError('USB statistics version differs')
    result['stats'] = dict(zip(('polls', 'busy_ticks', 'waits', 'timeouts', 'connects', 'disconnects',
                                'restarts', 'errors', 'counter', 'frequency'), values))
    return result


def wait_phase(phase, timeout=180, ready=False):
    began = time.monotonic()
    errors = []
    while time.monotonic() - began < timeout:
        channel = None
        try:
            channel = RootUsb(timeout_ms=3000)
            state = sample(channel)
            if state['phase'] == phase and (not ready or state['kernel-ready'] == 'ready'):
                return state
        except Exception as error:
            errors.append(str(error))
        finally:
            if channel:
                channel.close()
        time.sleep(5)
    raise TimeoutError(f'USB phase {phase} deadline expired; {errors[-3:]}')


def diagnostics(kernel):
    symbols = kernel_symbols(kernel)
    channel = RootUsb(timeout_ms=5000)
    try:
        if channel.command('getvar:phase') == 'firmware':
            memory = RetainedKernel(channel)
        else:
            base, size = (int(value, 16) for value in channel.command('getvar:kernel-memory').split(':'))
            address = base + 0x4000 + symbols['_cpu_ttep'] - 0xfffffff006e04000
            channel.command(f'oem memory-read:{address:016x}:00000008')
            memory = object.__new__(RetainedKernel)
            memory.channel, memory.base, memory.size = channel, base, size
            memory.ttbr1 = int.from_bytes(channel.read_back(), 'little')
            memory.bits, memory.tcr, memory.tables = 39, 25 | (25 << 16), {}
        result = {}
        for name in ('_marble_usb_heartbeat', '_marble_usb_stalls', '_marble_display_retirements',
                     '_marble_scanout_latch_count', '_real_ncpus'):
            result[name] = int.from_bytes(memory.read(symbols[name], 4), 'little')
        result['panic'] = memory.read(symbols['_native_resident_panic_message'], 512).split(b'\0')[0].decode(errors='replace')
        return result
    finally:
        channel.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--hub-tool', required=True, type=Path)
    parser.add_argument('--manual', action='store_true', help='Wait for an operator cable cycle at each phase')
    parser.add_argument('--resume', action='store_true', help='Continue an evidence record after its completed phases')
    parser.add_argument('--start-system', action='store_true', help='Start with an already running kernel')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = dict(completed=False, phases={})
    if args.resume:
        report = json.loads((output / 'usb-reconnect.json').read_text())
        report.pop('error', None)

    def save():
        (output / 'usb-reconnect.json').write_text(json.dumps(report, indent=2) + '\n')

    def reconnect(name, phase, off_ms):
        channel = RootUsb(timeout_ms=5000)
        try:
            before = sample(channel)
        finally:
            channel.close()
        if before['phase'] != phase:
            raise RuntimeError(f'{name}: phase differs before disconnect')
        port = topology()
        record = dict(before=before, topology=port, off_milliseconds=off_ms, before_monotonic=time.monotonic())
        report['phases'][name] = record
        save()
        print(json.dumps(dict(phase=name, action='disconnect', topology=port, off_ms=off_ms)), flush=True)
        if args.manual:
            input(f'Cable cycle in {name}; keep disconnected for {off_ms / 1000:g} seconds, reconnect, then press Enter: ')
            record['operator_cycle'] = True
        else:
            power = subprocess.run([str(args.hub_tool.resolve()), str(port['hub']), str(port['port']), str(off_ms)],
                                   capture_output=True, text=True, timeout=off_ms / 1000 + 15)
            record['power_output'] = power.stdout + power.stderr
            power.check_returncode()
            if 'power_off=00000000' not in power.stdout or 'power_on=00000000' not in power.stdout:
                raise RuntimeError('Individual hub port power cycle was unavailable')
        after = wait_phase(phase, timeout=90, ready=phase == 'kernel')
        record['after'] = after
        if after['returns'] != before['returns']:
            raise RuntimeError(f'{name}: resident return count changed during unplug')
        if after['stats']['disconnects'] <= before['stats']['disconnects'] and after['usb-session'] == before['usb-session']:
            raise RuntimeError(f'{name}: transport session did not change during unplug')
        record['passed'] = True
        save()
        print(json.dumps(dict(phase=name, passed=True, session=after['usb-session'])), flush=True)

    try:
        if not args.start_system:
            if not report['phases'].get('rootstage', {}).get('passed'):
                report['preboot'] = wait_phase('firmware')
                reconnect('rootstage', 'firmware', 5000)
            channel = RootUsb(timeout_ms=5000)
            try:
                report['continue'] = channel.command('continue')
            finally:
                channel.close()
        report['system_ready'] = wait_phase('kernel', ready=True)
        # Include userspace boot before the long cable absence.
        if not args.start_system:time.sleep(45)
        reconnect('system', 'kernel', 35000)
        report['system_diagnostics'] = diagnostics(args.kernel.resolve())
        if report['system_diagnostics']['panic']:
            raise RuntimeError('Kernel panic message appeared during system unplug')
        before=report['system_diagnostics']
        for command in (['home'],['swipe','--x','900','--y','1200','--to-x','100','--to-y','1200']):
            subprocess.run([sys.executable,str(DEVICE/'scripts/root_ui.py'),*command],check=True,
                           stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30)
        time.sleep(3)
        report['system_progress']=diagnostics(args.kernel.resolve())
        if report['system_progress']['_marble_display_retirements']<=before['_marble_display_retirements']:
            raise RuntimeError('Display retirement stopped after USB reconnect')
        save()
        channel = RootUsb(timeout_ms=5000)
        try:
            first = sample(channel)
        finally:
            channel.close()
        time.sleep(20)
        channel = RootUsb(timeout_ms=5000)
        try:
            second = sample(channel)
            report['idle_window'] = dict(before=first, after=second)
            elapsed = (second['stats']['counter'] - first['stats']['counter']) / second['stats']['frequency']
            report['idle_window']['polls_per_second'] = (second['stats']['polls'] - first['stats']['polls']) / elapsed
            report['idle_window']['usb_driver_busy_percent'] = (
                100 * (second['stats']['busy_ticks'] - first['stats']['busy_ticks']) /
                (second['stats']['counter'] - first['stats']['counter']))
            report['interrupt_reply'] = channel.command('oem interrupt')
        finally:
            channel.close()
        returned = wait_phase('firmware', timeout=90)
        report['resident_return'] = returned
        if int(returned['returns'], 16) != int(report['system_ready']['returns'], 16) + 1:
            raise RuntimeError('Requested resident return count differs')
        reconnect('returned_rootstage', 'firmware', 5000)
        report['returned_diagnostics'] = diagnostics(args.kernel.resolve())
        if report['returned_diagnostics']['panic'] != 'RootStage USB host interrupt':
            raise RuntimeError('Resident panic reason differs from the requested interrupt')
        channel = RootUsb(timeout_ms=5000)
        try:
            report['retry_reply'] = channel.command('oem retry')
        finally:
            channel.close()
        report['retry'] = wait_phase('kernel', ready=True)
        report['completed'] = True
        save()
        print(json.dumps(dict(completed=True, evidence=str(output / 'usb-reconnect.json'))), flush=True)
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
        save()
        raise


if __name__ == '__main__':
    main()
