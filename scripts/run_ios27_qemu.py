#!/usr/bin/env python3
"""Stage and observe the iOS 27 candidate with external QEMU state."""
import argparse
from datetime import datetime
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib

DEVICE = Path(__file__).resolve().parents[1]
WORKSPACE = DEVICE.parents[1]
OUTPUT = WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119'
SOURCE = DEVICE / 'ios27/source'

def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()

def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--stop', help='Kernel address; defaults to the selected fileset panic symbol')
    parser.add_argument('--kernel', type=Path, default=OUTPUT / 'kernel.fileset.macho')
    parser.add_argument('--kernel-logs', action='store_true', help='Capture bounded genuine IOLog calls through GDB')
    parser.add_argument('--virtual-storage', action='store_true', help='Attach the original OS and SystemOS through read-only VirtIO MMIO')
    parser.add_argument('--runtime', type=Path, help='Qualified runtime-payload directory with addon and owner trust module')
    parser.add_argument('--finish-function', action='store_true', help='Observe selected function returning to its caller')
    args = parser.parse_args()
    sys.path.insert(0, str(DEVICE / 'ios27/sources/XnuPrelink'))
    from kc_image import Image
    collection = Image(args.kernel.read_bytes())
    image = Image(collection.data, collection.entries['com.apple.kernel'][1]) if collection.entries else collection
    symbols = {name: address for address, names in image.symbols().items() for name in names}
    panic = symbols['_panic']
    stop = args.stop or hex(panic)
    library = OUTPUT / 'qemu'
    run = library / datetime.now().astimezone().strftime('run-%Y%m%d-%H%M%S')
    payloads = run / 'esp/EFI/UniXNU/Payloads'
    payloads.mkdir(parents=True)
    (run / 'esp/EFI/BOOT').mkdir()
    (library / 'tmp').mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(library / 'tmp'),
               PYTHONPATH=os.pathsep.join([str(SOURCE / 'scripts/qemu-entry'), str(SOURCE / 'scripts')]),
               PATH='/opt/homebrew/bin:' + os.environ.get('PATH', ''))
    stage = OUTPUT / 'candidate-debug/stage/EFI/UniXNU'
    inputs = {}
    for source, target in [
        (stage / 'UniXNU.efi', run / 'esp/EFI/BOOT/BOOTAA64.EFI'),
        (stage / 'Payloads/DarwinBoot.efi', payloads / 'DarwinBoot.efi'),
        (args.kernel.resolve(), payloads / 'kernel.macho'),
        (stage / 'Payloads/kernel.macho.trustcache', payloads / 'kernel.macho.trustcache'),
    ]:
        shutil.copyfile(source, target)
        inputs[str(target.relative_to(run))] = dict(source=str(source), sha256=digest(target))
    if args.runtime:
        runtime = args.runtime.resolve()
        report = json.loads((runtime / 'report.json').read_text())
        addon = runtime / 'runtime.gpuaddon'
        owner = Path(report['owner_trust_cache'])
        if report.get('build') != '24B119' or digest(addon) != report['addon_sha256'] or digest(owner) != report['trust_cache_sha256']:
            raise ValueError('Runtime identities differ from their qualified payload report')
        target = payloads / 'kernel.macho.gpuaddon'
        shutil.copyfile(addon, target)
        inputs[str(target.relative_to(run))] = dict(source=str(addon), sha256=digest(target))
        profile = tomllib.loads((DEVICE / 'profiles/os/ios-27.2-24B119/profile.toml').read_text())
        caches = [OUTPUT / 'ipsw/24B119' / name for name in profile['ipsw']['sha256'] if name.endswith('.trustcache')]
        for cache in caches:
            if digest(cache) != profile['ipsw']['sha256'][cache.name]:
                raise ValueError('Original trust cache changed: ' + str(cache))
        trust = payloads / 'kernel.macho.trustcache'
        subprocess.run([sys.executable, str(SOURCE / 'scripts/bundle-ipsw-trust.py'), str(trust),
                        *map(str, caches), str(owner)], env=env, check=True)
        inputs[str(trust.relative_to(run))] = dict(original_modules=list(map(str, caches)), owner=str(owner), sha256=digest(trust))
    dt_source = SOURCE / 'scripts/qemu-entry/make-dt.py'
    dt_generator = run / 'make-dt.py'
    # A710 cores form one performance cluster. AMP kernels require an explicit
    # cluster type; the older fixture left this at the SMP default.
    dt_generator.write_text(dt_source.read_text().replace(
        "('timebase-frequency',", "('cluster-type', struct.pack('<I', ord('P'))), ('timebase-frequency',")
        .replace("('random-seed',", "('amfi-only-platform-code', struct.pack('<I', 0)), ('random-seed',"))
    dt_command = [sys.executable, str(dt_generator),
                  '--armvirt', '--armcpu', '--smp', '8', '--dram-size', '0x200000000',
                  '--owned-memory-size', '0x100000000', '--trust-cache', '--nvram-proxy',
                  '--crypto-hash-method', 'sha2-384',
                  '--qemu-vmm-present', '--qemu-accel', 'tcg',
                  '--output', str(payloads / 'kernel.macho.adt')]
    disk_records = []
    scratch = None
    if args.virtual_storage:
        disk_records = json.loads((OUTPUT / 'candidate-debug/storage_images.json').read_text())[:2]
        fdt_command = ['qemu-system-aarch64', '-M',
                       f'virt,virtualization=off,secure=off,gic-version=3,dumpdtb={run / "virt.dtb"}',
                       '-accel', 'tcg', '-cpu', 'cortex-a710,cntfrq=62500000', '-m', '8192', '-smp', '8',
                       '-display', 'none', '-nic', 'none', '-global', 'virtio-mmio.force-legacy=false']
        for index, disk in enumerate(disk_records):
            path = Path(disk['source'])
            if path.stat().st_size != disk['raw_bytes']:
                raise ValueError('Virtual disk length differs from the recorded raw image: ' + str(path))
            disk['read_only'] = True
            fdt_command += ['-drive', f'if=none,id=root{index},format=raw,readonly=on,file.locking=off,file={path}',
                            '-device', f'virtio-blk-device,drive=root{index}']
        data_record = json.loads((library / 'disks/scratch-data-create.json').read_text())
        scratch = dict(path=data_record['qcow2'], uuid=data_record['volume']['APFSVolumeUUID'],
                       format='qcow2', read_only=False, virtual_bytes=32 * 1024**3)
        fdt_command += ['-drive', f'if=none,id=scratch,format=qcow2,file={scratch["path"]}',
                        '-device', 'virtio-blk-device,drive=scratch']
        with (run / 'fdt.log').open('w') as log:
            subprocess.run(fdt_command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        dt_command += ['--virtio-mmio-fdt', str(run / 'virt.dtb'),
                       '--root-match-volume-uuid', 'E0577111-0369-4BCF-A4E5-59B612AA9E31']
    subprocess.run(dt_command, env=env, check=True)
    if scratch or args.runtime:
        sys.path.insert(0, str(DEVICE / 'scripts'))
        from build_ios18_arm64e import read_adt, write_adt
        dt_path = payloads / 'kernel.macho.adt'
        dt, end = read_adt(dt_path.read_bytes())
        chosen = next(child for child in dt[1] if child[0]['name'] == b'chosen\0')
        if scratch:
            chosen[0]['qemu-data-volume-uuid'] = scratch['uuid'].encode() + b'\0'
            armio = next(child for child in dt[1] if child[0]['name'] == b'arm-io\0')
            armio[1][:] = [child for child in armio[1] if child[0]['name'] not in (b'ufshc\0', b'ufsphy\0')]
        if args.runtime:
            memory_map = next(child for child in chosen[1] if child[0]['name'] == b'memory-map\0')
            memory_map[0]['GpuAddon'] = bytes(16)
        dt_path.write_bytes(write_adt(dt))
    config = stage.joinpath('unixnu.toml').read_text().replace('timeout = 0', 'timeout = 1')
    config = config.replace('title = "marble iOS 27.2"', 'title = "iOS 27.2 QEMU candidate observation"')
    if args.virtual_storage:
        for option in ('marble-ufs', 'marble-adreno', 'marble-display-mmio', 'dpu-scanout',
                       'marble-wifi', 'marble-audio', 'marble-haptics'):
            config = re.sub(r'\b' + re.escape(option) + r'=\d+', option + '=0', config)
        config = config.replace('args = "',
                                'args = "rd=uuid boot-uuid=E0577111-0369-4BCF-A4E5-59B612AA9E31 diag=1 ', 1)
        arguments = tomllib.loads(config)['entry'][0]['args']
        physical_options = {'marble-ufs', 'marble-adreno', 'marble-display-mmio', 'dpu-scanout',
                            'marble-wifi', 'marble-wifi-service', 'marble-audio', 'marble-haptics',
                            'marble-native-services', 'rootstage-runtime-usb', 'gpu-ttbr',
                            'gpu-preempt', 'early-retire', 'fb-gpu', 'metal-flow'}
        virtual_arguments = ' '.join(word for word in arguments.split()
                                     if word.split('=', 1)[0] not in physical_options)
        if len(virtual_arguments.encode()) >= 1000:
            raise ValueError('Virtual boot arguments exceed the recorded loader capacity')
        config = config.replace('args = "' + arguments + '"', 'args = "' + virtual_arguments + '"')
    (run / 'esp/EFI/UniXNU/unixnu.toml').write_text(config)
    original = SOURCE / 'scripts/qemu-entry/gdb-stop.py'
    observer = run / 'gdb-stop.py'
    # Keep the upstream observer behavior; place its QMP socket on external storage.
    # A short relative socket pathname also fits the macOS Unix socket limit.
    observer.write_text(original.read_text().replace("'cortex-a710'", "'cortex-a710,cntfrq=62500000'")
                        .replace("dir='/tmp'", "dir='.'")
                        .replace("screen_socket=Path(screen_directory.name)/'qmp.sock'",
                                 "screen_socket=Path(screen_directory.name).resolve().relative_to(Path.cwd())/'qmp.sock'")
                        .replace('str(screen_socket.resolve())', 'str(screen_socket)')
                        .replace('{screen_socket.resolve()}', '{screen_socket}'))
    if scratch:
        observer.write_text(observer.read_text().replace('if args.capture_screen:',
            "command += ['-drive', " + repr('if=none,id=qemu-data,format=qcow2,file=' + scratch['path']) +
            ", '-device', 'virtio-blk-device,drive=qemu-data']\nif args.capture_screen:", 1))
    command = [sys.executable, str(observer), stop, '--fixture', str(run),
               '--esp', str(run / 'esp'), '--kernel', str(payloads / 'kernel.macho'),
               '--qemu-memory-mib', '8192', '--smp', '8', '--gic-version', '3',
               '--qemu-accel', 'tcg', '--qemu-ramfb', '--capture-screen',
               '--system-registers', '--timeout', str(args.timeout)]
    if args.finish_function:
        command.append('--finish-function')
    if disk_records:
        command += ['--virtio-block-image', disk_records[0]['source'],
                    '--virtio-extra-block-image', disk_records[1]['source']]
    for name, size in [('_armdt_platform_configured', 4), ('_armdt_platform_child_count', 4),
                       ('_armdt_platform_cpu_count', 4), ('_armdt_platform_bus_count', 4),
                       ('_armdt_platform_nvram_proxy', 4), ('_armdt_platform_device_count', 4),
                       ('_armcpu_smp_running', 4), ('_armcpu_smp_requested', 4),
                       ('_armcpu_smp_step', 32), ('_armcpu_smp_fail', 32),
                       ('_armcpu_smp_psci_rc', 32)]:
        if name in symbols:
            command += ['--read-memory', f'{symbols[name]:x}:{size:x}']
    if '_mac_late' in symbols:
        command += ['--read-memory', f'{symbols["_mac_late"]:x}:4']
    for name, size in [('_native_virtio_ready', 4), ('_native_virtio_read_requests', 4),
                       ('_native_virtio_read_failures', 4), ('_native_virtio_verified_devices', 4),
                       ('_native_virtio_integrity_failures', 4), ('_native_virtio_verified_blocks', 8)]:
        if name in symbols:
            command += ['--read-memory', f'{symbols[name]:x}:{size:x}']
    for name in ('_native_qemu_verified_started', '_native_qemu_writable_started',
                 '_native_qemu_start_failures', '_native_qemu_data_volume_ready',
                 '_native_qemu_data_read_requests', '_native_qemu_data_write_requests',
                 '_native_qemu_data_flush_requests'):
        if name in symbols:
            command += ['--read-memory', f'{symbols[name]:x}:4']
    if '_native_ios27_image4_monitor_requests' in symbols:
        command += ['--read-memory', f'{symbols["_native_ios27_image4_monitor_requests"]:x}:20']
    if args.kernel_logs:
        command += ['--cstring-log-address', hex(symbols['_IOLog'])]
    record = dict(created_at=datetime.now().astimezone().isoformat(), status='running',
                  run=str(run), qemu=subprocess.check_output(['qemu-system-aarch64', '--version'], env=env, text=True).splitlines()[0],
                  cpus=8, memory_mib=8192, owned_memory_mib=4096, inputs=inputs,
                  counter_frequency_hz=62500000,
                  dt_command=dt_command, command=command, platform='QEMU virt / cortex-a710 / GICv3',
                  scope='UEFI and candidate kernel boundary; physical marble execution deferred',
                  device_tree_sha256=digest(payloads / 'kernel.macho.adt'),
                  config_sha256=digest(run / 'esp/EFI/UniXNU/unixnu.toml'),
                  cpu_topology='8 Cortex-A710 cores in one performance cluster',
                  disks=disk_records,
                  scratch_disk=scratch,
                  dt_generator_sha256=digest(dt_generator),
                  upstream_observer_sha256=digest(original), observer_sha256=digest(observer))
    save(run / 'manifest.json', record)
    save(library / 'latest.json', record)
    current_path = DEVICE / 'CURRENT.json'
    current = json.loads(current_path.read_text())
    current['updated_at'] = record['created_at']
    current['qemu'] = record
    current['execution_state']['device_operations'] = 'marble deferred by user; QEMU first'
    save(current_path, current)
    print(str(run), flush=True)
    with (run / 'observer.log').open('w') as log:
        result = subprocess.run(command, cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT)
    report_path = run / 'gdb-stop.json'
    if not report_path.exists():
        report_path = run / 'gdb-failure.json'
    record.update(status=('observed' if result.returncode == 0 else 'observer-error'),
                  observer_exit_code=result.returncode,
                  finished_at=datetime.now().astimezone().isoformat(), report=str(report_path))
    if report_path.exists():
        record['result'] = json.loads(report_path.read_text())
        observed = record['result']
        record['checkpoint_reached'] = (not observed.get('bounded_timeout_interrupted', True)
                                        and observed.get('pc') == hex(int(stop, 0)))
        record['guest_status'] = ('kernel-panic' if record['checkpoint_reached']
                                  and int(stop, 0) == panic else 'checkpoint-observation')
    save(run / 'manifest.json', record)
    save(library / 'latest.json', record)
    save(DEVICE / 'records/ios27-24B119/qemu.json', record)
    current = json.loads(current_path.read_text())
    current['qemu'] = record
    current['updated_at'] = record['finished_at']
    save(current_path, current)
    print(json.dumps(record, ensure_ascii=False, indent=2), flush=True)
    return result.returncode

if __name__ == '__main__':
    sys.exit(main())
