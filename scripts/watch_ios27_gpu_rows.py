#!/usr/bin/env python3
"""Queue GPU rows and optional code-page comparisons for the sole USB observer."""
import json
import os
from pathlib import Path
import sys
import time
import shutil
from capture_ios27 import symbols_for

root = Path(sys.argv[1])
origin = root
text_pages = '--text-pages' in sys.argv[2:]
loss_record = '--loss-record' in sys.argv[2:]
storage_capacity = '--storage-capacity' in sys.argv[2:]
cpu_idle_power = '--cpu-idle-power' in sys.argv[2:]
cpu_idle_checked = cpu_idle_power
submit_reject = False
gpu_idle_power = False
charge_source = False
cpu_power_profile = False
cpu_power_caller = False
charge_source_v2 = False
wifi_link = False
cpu_power_ancestors = False
wifi_power = False
existing = [int(p.stem[5:]) for p in root.glob('rows-*.bin') if p.stem[5:].isdigit()]
number = max(existing, default=-1) + 1
while True:
    visited=set()
    while (root/'observer-output-redirect.json').exists() and root not in visited:
        visited.add(root)
        target=Path(json.loads((root/'observer-output-redirect.json').read_text())['output'])
        if target==root:break
        if (root/'boot.json').exists() and not (target/'boot.json').exists():
            shutil.copy2(root/'boot.json',target/'boot.json')
        root=target
        number=max(number,max((int(p.stem[5:]) for p in root.glob('rows-[0-9]*.bin')),default=-1)+1)
    if root!=origin:
        logs=sorted(root.glob('kernel-[0-9]*.log'))
        if logs:
            source=logs[-1];destination=origin/source.name
            if not destination.exists() or destination.stat().st_mtime_ns<source.stat().st_mtime_ns:
                temporary=origin/('relay-'+source.name+'.tmp');shutil.copy2(source,temporary);temporary.replace(destination)
        for name in ('memory-request.json','ui-request.json','context-request.json'):
            source=origin/name;destination=root/name
            if source.exists() and not destination.exists():
                data=source.read_bytes();temporary=root/('relay-'+str(os.getpid())+'.tmp');temporary.write_bytes(data)
                try:
                    os.link(temporary,destination)
                    if source.exists() and source.read_bytes()==data:source.unlink()
                except FileExistsError:pass
                finally:temporary.unlink(missing_ok=True)
        if (root/'channel-error.json').exists():shutil.copy2(root/'channel-error.json',origin/'channel-error.json')
    if (root/'channel-error.json').exists():break
    request = root / 'memory-request.json'
    if not request.exists() and (root / 'boot.json').exists():
        if storage_capacity and not cpu_idle_checked:
            boot = json.loads((root / 'boot.json').read_text())
            candidate = json.loads(Path(boot['candidate']).read_text())
            _, symbols = symbols_for(Path(candidate['kernel']),
                                     Path(__file__).resolve().parents[1] / 'ios27/sources/XnuPrelink')
            cpu_idle_power = '_marble_cpu_idle_power' in symbols
            submit_reject = '_native_adreno_submit_reject' in symbols
            gpu_idle_power = '_marble_gpu_idle_power' in symbols
            charge_source = '_marble_charge_source_stats' in symbols
            charge_source_v2 = '_marble_charge_source_stats_v2' in symbols
            cpu_power_profile = '_armcpu_power_pc' in symbols
            cpu_power_caller = '_armcpu_power_lr' in symbols
            cpu_power_ancestors = '_marble_cpu_power_ancestors' in symbols
            wifi_power = '_native_marble_wifi_power_stats' in symbols
            wifi_link = '_native_marble_wifi_link_stats' in symbols
            cpu_idle_checked = True
        operations = [dict(name=f'rows-{number:03}', symbol='_native_adreno_metal_rows', size=4608),
                      dict(name=f'rowsseq-{number:03}', symbol='_native_adreno_metal_rows_seq', size=4)]
        if text_pages:
            try:
                sequence = json.loads((root / 'text-page-latest.json').read_text())['sequence']
            except (FileNotFoundError, KeyError, json.JSONDecodeError):
                sequence = -1
            operations.append(dict(command='text-page-diagnostics', name=f'text-page-{number:03}',
                                   after_sequence=sequence))
        if loss_record:
            operations.extend([dict(name=f'loss-text-{number:03}',symbol='_native_adreno_loss_text',size=256),
                               dict(name=f'loss-record-{number:03}',symbol='_native_adreno_loss_record',size=16)])
        if storage_capacity:
            operations.append(dict(name=f'storage-capacity-{number:03}',symbol='_marble_storage_capacity',size=64))
            if wifi_power:
                operations.extend([dict(name=f'wifi-power-{number:03}',symbol='_native_marble_wifi_power_stats',size=128),
                                   dict(name=f'wifi-runtime-{number:03}',symbol='_native_marble_wifi_runtime_stats',size=64),
                                   dict(name=f'wifi-text-{number:03}',symbol='_native_marble_wifi_text',size=20480)])
            if wifi_link:
                operations.append(dict(name=f'wifi-link-{number:03}',symbol='_native_marble_wifi_link_stats',size=40))
            if cpu_idle_power:
                operations.append(dict(name=f'cpu-idle-power-{number:03}',
                                       symbol='_marble_cpu_idle_power', size=192))
            if submit_reject:
                operations.append(dict(name=f'submit-reject-{number:03}',
                                       symbol='_native_adreno_submit_reject', size=128))
            if gpu_idle_power:
                operations.append(dict(name=f'gpu-idle-power-{number:03}',
                                       symbol='_marble_gpu_idle_power', size=128))
            if charge_source_v2 or charge_source:
                operations.append(dict(name=f'charge-source-{number:03}',
                                       symbol='_marble_charge_source_stats_v2' if charge_source_v2 else '_marble_charge_source_stats',size=80 if charge_source_v2 else 64))
            if cpu_power_profile:
                operations.append(dict(name=f'cpu-power-pc-{number:03}',
                                       symbol='_armcpu_power_pc',size=16384))
            if cpu_power_caller:
                operations.append(dict(name=f'cpu-power-lr-{number:03}',
                                       symbol='_armcpu_power_lr',size=8192))
            if cpu_power_ancestors:
                operations.append(dict(name=f'cpu-power-frames-{number:03}',
                                       symbol='_marble_cpu_power_ancestors',size=20480))
            for label,symbol,size in [('cpu-idle','__ZL5gCore',2048),('cpu-seen','__ZL9gCoreSeen',4),
                ('cpu-clock','__ZL10gCounterHz',8),('battery','_native_battery_stats',64),
                ('battery-raw','_native_battery_raw',40),('charger','_native_charger_stats',64),
                ('backlight','_marble_backlight_stats',32),('cpu-l3','_marble_cpu_l3',16)]:
                operations.append(dict(name=f'{label}-{number:03}',symbol=symbol,size=size))
        temporary = root / f'memory-rows-{os.getpid()}.tmp'
        temporary.write_text(json.dumps(operations) + '\n')
        try:
            os.link(temporary, request)
            number += 1
        except FileExistsError:
            pass
        finally:
            temporary.unlink()
    time.sleep(15)
