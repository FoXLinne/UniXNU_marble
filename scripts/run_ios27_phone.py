#!/usr/bin/env python3
"""Boot a temporary marble candidate and retain firmware evidence across recovery."""
import argparse
from datetime import datetime
import json
import itertools
import errno
import os
import shutil
import sys
import tempfile
import re
import struct
from pathlib import Path
import subprocess
import time

from root_control import RootUsb
from capture_ios27 import symbols_for

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--candidate', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--attach', action='store_true', help='Attach to the same candidate already running on RootStage')
parser.add_argument('--context-process', action='append', default=[], help='Process name prefix for live thread capture')
parser.add_argument('--max-context-threads', type=int, default=24, choices=range(1,129), help='Maximum threads per live process capture')
args = parser.parse_args()
candidate = json.loads(args.candidate.read_text())
_, kernel_symbols = symbols_for(Path(candidate['kernel']), Path(__file__).resolve().parents[1] / 'ios27/sources/XnuPrelink')
args.output.mkdir(parents=True, exist_ok=True)
screen_output = Path(tempfile.mkdtemp(prefix='marble-live-ui-', dir='/private/tmp'))

def redirect_evidence(reason):
    previous = args.output
    target = Path(tempfile.mkdtemp(prefix='marble-observer-', dir='/private/tmp'))
    note = {'time': datetime.now().astimezone().isoformat(), 'previous': str(previous.resolve()),
            'output': str(target), 'reason': str(reason), 'image_sha256': candidate['image_sha256']}
    # Keep the USB owner alive when its evidence volume fills. Redirect both
    # Python and inherited descriptors so a full stdout log cannot close it.
    with (target / 'observer.log').open('a') as stream:
        os.dup2(stream.fileno(), 1)
        os.dup2(stream.fileno(), 2)
    args.output = target
    for name in ('ui-request.json', 'context-request.json', 'memory-request.json', 'observer-stop-request'):
        source = previous / name
        if source.exists():
            try:
                shutil.copyfile(source, target / name)
                source.unlink()
            except OSError:
                pass
    (target / 'observer-output-redirect.json').write_text(json.dumps(note, indent=2) + '\n')
    try:
        (previous / 'observer-output-redirect.json').write_text(json.dumps(note, indent=2) + '\n')
    except OSError:
        pass
    return note

def record(name, value):
    encoded = json.dumps(value, ensure_ascii=False, indent=2) + '\n'
    try:
        (args.output / name).write_text(encoded)
        print(name, json.dumps(value, ensure_ascii=False), flush=True)
    except OSError as error:
        if error.errno not in (errno.ENOSPC, errno.EDQUOT):
            raise
        note = redirect_evidence(error)
        (args.output / name).write_text(encoded)
        print('observer-output-redirect', json.dumps(note), flush=True)

diagnostic_worker = None
def request_device_logs(operation):
    import http.client
    import ipaddress
    import threading
    global diagnostic_worker
    if diagnostic_worker is not None and diagnostic_worker.is_alive():
        raise ValueError('A device log read is active')
    address = str(ipaddress.IPv4Address(operation['host']))
    if not ipaddress.ip_address(address).is_private:
        raise ValueError('Device diagnostic address must be local')
    token = Path(operation['auth_file']).read_text().strip()
    if not re.fullmatch('[0-9a-f]{64}', token):
        raise ValueError('Invalid device diagnostic credential')
    name = operation['name']
    destination = args.output
    routes = {'device-logs': '/unified.log', 'device-registration-status': '/webkit-status.json'}
    route = routes[operation['command']]
    def read_logs():
        result = {'host': address, 'route': route, 'began_at': time.time()}
        connection = http.client.HTTPConnection(address, 23456, timeout=20)
        try:
            connection.request('GET', route, headers={'Authorization': 'Bearer '+token})
            response = connection.getresponse()
            data = response.read((4 << 20)+1)
            result.update(http_status=response.status, bytes=min(len(data),4 << 20), truncated=len(data)>4 << 20)
            (destination/(name+'.log')).write_bytes(data[:4 << 20])
        except Exception as error:
            result['error'] = str(error)
        finally:
            connection.close()
            result['finished_at'] = time.time()
            (destination/(name+'.status.json')).write_text(json.dumps(result,indent=2)+'\n')
    diagnostic_worker = threading.Thread(target=read_logs, name='marble-log-read', daemon=True)
    diagnostic_worker.start()
    return {'name': name, 'command': operation['command'], 'status': 'started'}


def bootloader():
    result = subprocess.run(['fastboot', 'devices'], capture_output=True, text=True, timeout=5)
    if not any(row.split()[:1] == ['556fdfe'] for row in result.stdout.splitlines()):
        return False
    try:
        reply = subprocess.run(['fastboot', '-s', '556fdfe', 'getvar', 'product'],
                               capture_output=True, text=True, timeout=3)
        return reply.returncode == 0 and 'product: marble' in reply.stderr
    except subprocess.TimeoutExpired:
        return False

booted_here = False if args.attach else bootloader()
if booted_here:
    subprocess.run(['fastboot', '-s', '556fdfe', 'boot', candidate['image']], check=True, timeout=30)
else:
    if not args.attach:
        raise RuntimeError('Candidate boot requires Xiaomi fastboot; use the active observer firmware reboot command before loading a new image')
record('boot.json', {'time': datetime.now().astimezone().isoformat(), 'candidate': str(args.candidate.resolve()),
                     'image_sha256': candidate['image_sha256'], 'boot_command': 'completed' if booted_here else 'attached'})
channel = None
try:
    for attempt in range(12):
        try:
            channel = RootUsb(timeout_ms=8000)
            break
        except Exception as error:
            record(f'connect-{attempt}.json', {'error': str(error)})
            if attempt >= 4 and bootloader():
                record('recovery.json', {'state': 'bootloader-return-before-continue'})
                break
            time.sleep(5)
    if channel is None:
        raise RuntimeError('Firmware channel unavailable; recovery observations saved')
    try:
        channel.command('oem previous-boot-log')
        previous=channel.read_back()
        (args.output/'previous-boot-log.bin').write_bytes(previous)
        if previous[:8]==b'MBTRACE1':
            magic,count,frequency=struct.unpack_from('<3Q',previous)
            events=[]
            for ordinal in range(max(0,count-120),count):
                counter,event,address,value=struct.unpack_from('<4Q',previous,32+32*(ordinal%120))
                events.append({'counter':counter,'event':hex(event),'address':hex(address),'value':hex(value)})
            record('previous-boot-log.json',{'count':count,'frequency':frequency,'events':events})
            previous_globals={}
            base=int(channel.command('getvar:kernel-memory').split(':')[0],16)
            for name,size in [('_armcpu_smp_step',32),('_armcpu_smp_mpidr',32),('_armcpu_smp_seen_mpidr',32),
                              ('_armcpu_smp_fail',32),('_armcpu_smp_psci_rc',32),('_armcpu_gic_boot',32),
                              ('_armcpu_registered',4),('_armcpu_irq_phase',4),('_armcpu_irq_id',4),
                              ('_native_root_block_panic_text',4096),('_native_resident_panic_message',512),
                              ('_marble_primary_ui_exits',1760),('_marble_primary_ui_exit_reasons',2048),
                              ('_marble_primary_ui_exit_count',8)]:
                if name not in kernel_symbols:
                    continue
                at=base+candidate['boot_log_offset']+kernel_symbols[name]-kernel_symbols['_native_marble_boot_events']
                channel.command(f'oem memory-read:{at:016x}:{size:08x}')
                raw=channel.read_back()
                (args.output/f'previous-{name}.bin').write_bytes(raw)
                previous_globals[name]=list(struct.unpack('<'+str(size//4)+'I',raw)) if size<=32 else raw.split(b'\0')[0].decode(errors='replace')
            record('previous-globals.json',previous_globals)
            targets=[int(e['address'],16) for e in events if e['event']=='0x106']
            if targets and candidate.get('boot_log_offset') is not None:
                from capture_ios18_resident import RetainedKernel
                class PreviousMemory(RetainedKernel):
                    def __init__(self):
                        self.channel=channel
                        self.base,self.size=(int(v,16) for v in channel.command('getvar:kernel-memory').split(':'))
                        delta=kernel_symbols['_cpu_ttep']-kernel_symbols['_native_marble_boot_events']
                        at=self.base+candidate['boot_log_offset']+delta
                        channel.command(f'oem memory-read:{at:016x}:00000008')
                        self.ttbr1=int.from_bytes(channel.read_back(),'little')
                        self.bits=38;self.tables={}
                previous_memory=PreviousMemory()
                record('previous-gic-map.json',{'root':hex(previous_memory.ttbr1),
                       'virtual':hex(targets[-1]),'physical':hex(previous_memory.translate(targets[-1]))})

    except (RuntimeError,ValueError) as error:
        print('previous boot log',error,flush=True)
    cached_bases=None
    def capture_live_ring(index):
        if cached_bases is None:
            return
        virtual,physical=cached_bases
        def field(name):
            location=kernel_symbols[name]-virtual+physical
            channel.command(f'oem memory-read:{location:016x}:00000008')
            return int.from_bytes(channel.read_back(),'little')
        root=field('_cpu_ttep')
        msg=field('_msgbufp')
        if not root or root&4095 or not msg>>63:
            return
        from capture_ios18_resident import RetainedKernel
        class LiveRing(RetainedKernel):
            def __init__(self):
                self.channel=channel
                self.base,self.size=(int(v,16) for v in channel.command('getvar:kernel-memory').split(':'))
                self.ttbr1=root
                self.bits=38
                self.tables={}
        memory=LiveRing()
        if '_console_ring' in kernel_symbols:
            try:
                header=memory.read(kernel_symbols['_console_ring'],40)
                pointer,size,used,reserved=struct.unpack_from('<Qiii',header)
                write_pointer=struct.unpack_from('<Q',header,24)[0]
                if pointer and 0<size<=2<<20 and pointer<=write_pointer<=pointer+size:
                    raw=memory.read(pointer,size)
                    cursor=(write_pointer-pointer)%size
                    (args.output/f'console-{index:03}.log').write_bytes(
                        (raw[cursor:]+raw[:cursor]).replace(b'\0',b''))
            except (RuntimeError,ValueError,IOError):
                pass
        try:
            header=memory.read(msg,24)
            _,size,cursor,_,pointer=struct.unpack('<4IQ',header)
            if 0<size<=16<<20 and 0<cursor<size:
                count=min(cursor,256<<10)
                raw=memory.read(pointer+cursor-count,count)
                (args.output/f'priority-kernel-{index:03}.log').write_bytes(raw.replace(b'\0',b''))
        except (RuntimeError,ValueError,IOError):
            pass
    for index in itertools.count():
        try:
            if time.monotonic() - locals().get('last_space_check', 0) >= 60:
                last_space_check = time.monotonic()
                if shutil.disk_usage(args.output).free < 128 << 20:
                    redirect_evidence('Evidence volume free space below 128 MiB')
            if (args.output/'observer-stop-request').exists():
                graceful_stop=True
                record('observer-stopped.json',{'time':datetime.now().astimezone().isoformat(),'state':'USB operation boundary reached'})
                break
            ui_request=args.output/'ui-request.json'
            if ui_request.exists():
                from root_ui import RootUI
                operations=json.loads(ui_request.read_text())
                ui_request.unlink()
                try:
                    ui=RootUI(channel)
                    results=[]
                    for operation in operations:
                        if operation['command']=='tap':
                            ui.tap(*operation['point'])
                        elif operation['command']=='button':
                            masks={'power':2,'volume-up':4,'volume-down':8}
                            ui.input(5,masks[operation['button']],1)
                            ui.wait_idle()
                        elif operation['command']=='swipe':
                            duration=int(operation.get('duration_ms',400))
                            hold=int(operation.get('hold_ms',0))
                            if not 16 <= duration <= 2000:raise ValueError('Swipe duration outside 16..2000 milliseconds')
                            if not 0 <= hold <= 2000:raise ValueError('Swipe hold outside 0..2000 milliseconds')
                            ui.swipe(*operation['from'],*operation['to'],duration,hold)
                        else:raise ValueError('Unknown UI input command')
                        results.append(operation)
                    record('ui-response.json',{'time':datetime.now().astimezone().isoformat(),'operations':operations})
                except (TimeoutError,ValueError,KeyError,RuntimeError) as error:
                    record('ui-response.json',{'time':datetime.now().astimezone().isoformat(),'operations':operations,'error':str(error)})
                if any(operation.get('capture', True) for operation in operations):
                    (args.output/'context-request.json').write_text(json.dumps({'processes':[], 'fresh_screen':True}))
            request=args.output/'memory-request.json'
            if request.exists():
                operations=json.loads(request.read_text())
                if any(operation.get('command') == 'reboot-bootloader' for operation in operations):
                    request.unlink()
                    reply=channel.command('reboot-bootloader')
                    record('memory-response.json',[{'command':'reboot-bootloader','reply':reply}])
                    raise RuntimeError('Firmware reboot requested through live session')
            capture_live_ring(index)
            try:
                channel.command('oem boot-log')
                raw=channel.read_back()
                (args.output/f'boot-events-{index:03}.bin').write_bytes(raw)
                if raw[:8]==b'MBTRACE1':
                    _,count,frequency=struct.unpack_from('<3Q',raw)
                    events=[struct.unpack_from('<4Q',raw,32+32*(n%120)) for n in range(max(0,count-120),count)]
                    record(f'boot-events-{index:03}.json',{'count':count,'frequency':frequency,
                           'events':[{'counter':v[0],'event':hex(v[1]),'address':hex(v[2]),'value':hex(v[3])} for v in events]})
            except RuntimeError:
                pass
            state = {'time': datetime.now().astimezone().isoformat()}
            for name in ['phase', 'watchdog', 'returns', 'handoff-fault', 'kernel-ready', 'usb-stats', 'gic-waker', 'gic-wakers']:
                try:
                    state[name] = channel.command('getvar:' + name)
                except RuntimeError as error:
                    state[name] = str(error)
            record(f'state-{index:03}.json', state)
            channel.command('oem firmware-log')
            log = channel.read_back().decode('utf-16-le', errors='replace').split('\0')[0]
            (args.output / f'firmware-{index:03}.log').write_text(log)
            print(log[-2600:], flush=True)
            arguments = re.findall(r'args=0x([0-9A-Fa-f]+)', log)
            if arguments:
                address = int(arguments[-1], 16) + 0x840
                channel.command(f'oem memory-read:{address:016x}:00000020')
                words = struct.unpack('<4Q', channel.read_back())
                record(f'handoff-{index:03}.json', {'magic': hex(words[0]), 'event': words[1],
                                                  'address': hex(words[2]), 'counter': words[3]})
                args_address=int(arguments[-1],16)
                channel.command(f'oem memory-read:{args_address:016x}:00000028')
                boot=channel.read_back()
                virtual,physical=struct.unpack_from('<QQ',boot,8)
                cached_bases=(virtual,physical)
                early={}
                for name,count in [('_native_resident_panic_message',512),('_native_root_block_panic_text',4096),
                                   ('_BootArgs',8),('_gPhysBase',8),('_msgbufp',8),('_cpu_ttep',8),('_marble_setup_policy_status',4),
                                   ('_native_touch_dispatch_state',32),('_marble_hid_events',32),('_marble_hid_results',32),
                                   ('_marble_scanout_wait_stats',48),('_marble_poster_signature_queries',8),
                                   ('_native_gop_gpu_offers',8),('_native_gop_gpu_completions',8),('_native_gop_gpu_failures',8),('_native_gop_gpu_abandoned',8),
                                   ('_native_legacy_direct',56),('_native_legacy_swapstat',64),('_native_legacy_surfaces',56),('_native_legacy_formats',40),('_native_legacy_swap_timeouts',8),
                                   ('_real_ncpus',4),('_desiredvnodes',4),('_numvnodes',4),('_numvnodes_max',4),('_freevnodes',4),('_deadvnodes',4),('_kern_maxvnodes',4),('_marble_ufs_reads',8),('_marble_ufs_writes',8),('_marble_display_frames',4),('_marble_display_pm',12),('_marble_display_swap_stage',4),('_marble_display_transfers',4),('_marble_display_retirements',4),('_marble_display_stage',4),('_marble_metadata_directory_open_count',8),('_marble_metadata_directory_access_count',8),('_armcpu_stackshot_unavailable',8),('_armcpu_ipi_sent',8),('_armcpu_ipi_interrupts',8),('_armcpu_timer_interrupts',8),('_armdt_platform_configured',4),
                                   ('_armdt_platform_cpu_count',4),('_armcpu_registered',4),('_armcpu_smp_running',4),('_marble_systemos_phase',4),('_marble_systemos_error',4)]:
                    if name not in kernel_symbols:
                        continue
                    location=kernel_symbols[name]-virtual+physical
                    channel.command(f'oem memory-read:{location:016x}:{count:08x}')
                    raw=channel.read_back()
                    (args.output/f'{index:03}-{name}.bin').write_bytes(raw)
                    early[name]=raw.split(b'\0')[0].decode(errors='replace') if count>8 else hex(int.from_bytes(raw,'little'))
                record(f'early-{index:03}.json',early)
                if early.get('_gPhysBase') not in (None, '0x0') and early.get('_cpu_ttep') not in (None,'0x0'):
                    from capture_ios18_resident import RetainedKernel
                    class LiveMemory(RetainedKernel):
                        def __init__(self):
                            self.channel=channel
                            self.base,self.size=(int(v,16) for v in channel.command('getvar:kernel-memory').split(':'))
                            self.ttbr1=int(early['_cpu_ttep'],16)
                            # Native start.s TCR T1SZ=26 for this 4 KiB target.
                            self.bits=38
                            self.tcr=(26 << 16) | 17
                            self.tables={}
                    memory=LiveMemory()
                    request=args.output/'memory-request.json'
                    if request.exists():
                        operations=json.loads(request.read_text())
                        request.unlink()
                        results=[]
                        for operation in operations:
                            if operation.get('command')=='reboot-bootloader':
                                results.append({'command':'reboot-bootloader','reply':channel.command('reboot-bootloader')})
                                record('memory-response.json',results)
                                raise RuntimeError('Firmware reboot requested through live memory session')
                            name=operation['name']
                            if not re.fullmatch(r'[A-Za-z0-9_.-]+',name):raise ValueError('Invalid memory capture file name')
                            try:
                                if operation.get('command') in ('device-logs', 'device-registration-status'):
                                    results.append(request_device_logs(operation)); continue
                                if operation.get('command') == 'text-page-diagnostics':
                                    from ios27_text_page_capture import capture
                                    results.append(dict(name=name, **capture(memory, kernel_symbols, args.output, name,
                                                                             operation.get('after_sequence', -1))))
                                    continue
                                address=kernel_symbols[operation['symbol']] if 'symbol' in operation else int(operation['address'],16)
                                length=int(operation['size'])
                                if not 0 < length <= 1<<20:raise ValueError('Memory read length outside 1..1048576')
                                root=None;bits=None
                                if 'pid' in operation:
                                    from capture_ios18_resident import capture_processes
                                    layout=json.loads((Path(candidate['kernel']).parents[1]/'kernel.layout.json').read_text())
                                    process=next(p for p in capture_processes(memory,kernel_symbols,layout['proc']) if p['pid']==operation['pid'])
                                    task=int(process['address'],16)+int.from_bytes(memory.read(kernel_symbols['_proc_struct_size'],8),'little')
                                    vm_map=int.from_bytes(memory.read(task+layout['user']['task_map'],8),'little')
                                    pmap=int.from_bytes(memory.read(vm_map+layout['user']['map_pmap'],8),'little')
                                    root=int.from_bytes(memory.read(pmap+layout['user']['pmap_ttep'],8),'little');bits=47
                                raw=memory.read(address,length,root,bits) if root else memory.read(address,length)
                                (args.output/(name+'.bin')).write_bytes(raw)
                                results.append({'name':name,'bytes':len(raw),'address':hex(address)})
                            except (KeyError,ValueError,RuntimeError,IOError,StopIteration) as error:
                                results.append({'name':name,'error':str(error)})
                        record('memory-response.json',results)
                    if '_marble_path_calls' in kernel_symbols:
                        try:
                            count=int.from_bytes(memory.read(kernel_symbols['_marble_path_call_count'],4),'little')
                            size=int.from_bytes(memory.read(kernel_symbols['_marble_path_call_bytes'],4),'little')
                            if count and count != locals().get('last_path_call_count') and 96 <= size <= 4096:
                                pointer=int.from_bytes(memory.read(kernel_symbols['_marble_path_calls'],8),'little')
                                raw=memory.read(pointer,size*1024)
                                (args.output/'path-calls.bin').write_bytes(raw)
                                record('path-calls.json',{'count':count,'record_bytes':size,'slots':1024})
                                last_path_call_count=count
                            for symbol,length in [('_marble_primary_ui_exits',1760),('_marble_primary_ui_exit_reasons',2048),
                                                  ('_marble_primary_ui_exit_count',8),('_marble_sgi_probe',80),('_armcpu_gic_boot',32)]:
                                if symbol in kernel_symbols:
                                    raw=memory.read(kernel_symbols[symbol],length)
                                    (args.output/(symbol+'.bin')).write_bytes(raw)
                                    if length <= 80:
                                        record(symbol+'.json',{'words':list(struct.unpack('<'+str(length//8 if length==80 else length//4)+('Q' if length==80 else 'I'),raw))})
                        except (KeyError,ValueError,RuntimeError,IOError) as error:
                            record('path-calls-error.json',{'error':str(error)})
                    context_request=args.output/'context-request.json'
                    if (index >= 30 and index % 15 == 0) or (args.attach and index == 0) or context_request.exists() or early.get('_native_root_block_panic_text'):
                        try:
                            from capture_ios18_resident import capture_processes
                            layout=json.loads((Path(candidate['kernel']).parents[1]/'kernel.layout.json').read_text())
                            fresh_screen=False
                            screenshot_only=(context_request.exists() and json.loads(context_request.read_text()).get('processes')==[])
                            periodic=(index>=30 and index%15==0) or (args.attach and index==0) or early.get('_native_root_block_panic_text')
                            processes=capture_processes(memory,kernel_symbols,layout['proc']) if periodic or not screenshot_only else []
                            if processes:record(f'processes-{index:03}.json',processes)
                            if (args.attach and index == 0) or context_request.exists():
                                from capture_ios18_resident import capture_init
                                selected=args.context_process or ['backboardd','thermalmonitord','driverkitd']
                                context_output=args.output
                                thread_limit=args.max_context_threads
                                if context_request.exists():
                                    context_spec=json.loads(context_request.read_text())
                                    fresh_screen=bool(context_spec.get('fresh_screen',False))
                                    selected=context_spec['processes']
                                    thread_limit=int(context_spec.get('max_threads',thread_limit))
                                    if not 1<=thread_limit<=128:raise ValueError('Context thread limit outside 1..128')
                                    context_request.unlink()
                                    context_output=args.output/f'sample-{index:03}'
                                    context_output.mkdir(exist_ok=True)
                                for process in processes:
                                    if process['name'].startswith(tuple(selected)):
                                        try:
                                            context=capture_init(memory,kernel_symbols,layout,lambda value:hex(value),
                                                context_output,proc=int(process['address'],16),label=process['name'],max_threads=thread_limit)
                                            (context_output/(process['name']+'-live-context.json')).write_text(json.dumps(context,indent=2)+'\n')
                                        except (KeyError,ValueError,RuntimeError,IOError) as error:
                                            (context_output/(process['name']+'-context-error.json')).write_text(json.dumps({'pid':process['pid'],'error':str(error)},indent=2)+'\n')

                            if fresh_screen or not (screen_output/'screen.png').exists():
                                channel.command('oem input:04:0000:0000')
                                for snapshot_wait in range(6):
                                    if channel.command('getvar:control') == '00000000':
                                        break
                                    time.sleep(0.05)
                                ready=kernel_symbols.get('_marble_display_snapshot_ready')
                                if ready and int.from_bytes(memory.read(ready,4),'little'):
                                    from capture_ios27 import screenshot
                                    screenshot(memory,kernel_symbols,screen_output)
                                    record('live-screen.json', {'path':str(screen_output/'screen.png'),
                                           'captured_at_ns':time.time_ns()})
                        except (KeyError,ValueError,RuntimeError,IOError) as error:
                            record(f'process-capture-error-{index:03}.json',{'error':str(error)})
                    if index >= 10 or early.get('_native_root_block_panic_text'):
                        try:
                            count = int.from_bytes(memory.read(kernel_symbols['_marble_ui_exit_count'],4),'little')
                            size = int.from_bytes(memory.read(kernel_symbols['_marble_ui_exit_bytes'],4),'little')
                            if 368 <= size <= 4096:
                                exits = memory.read(kernel_symbols['_marble_ui_exits'],size*16)
                                reasons = memory.read(kernel_symbols['_marble_ui_exit_reasons'],1024*16)
                                rows=[]
                                for ordinal in range(max(0,count-16),count):
                                    slot=ordinal%16
                                    entry=exits[slot*size:(slot+1)*size]
                                    pid,namespace,code=struct.unpack_from('<IIQ',entry)
                                    rows.append({'pid':pid,'namespace':namespace,'code':hex(code),
                                        'name':entry[16:48].split(b'\0')[0].decode(errors='replace'),
                                        'description':entry[368:].split(b'\0')[0].decode(errors='replace'),
                                        'reason':reasons[slot*1024:(slot+1)*1024].split(b'\0')[0].decode(errors='replace'),
                                        'state':{n:hex(struct.unpack_from('<Q',entry,48+o)[0]) for n,o in [('pc',264),('lr',248),('fp',240),('sp',256)]}})
                                record(f'exits-{index:03}.json',{'count':count,'records':rows})
                        except (KeyError, ValueError, RuntimeError, IOError) as error:
                            record(f'exits-error-{index:03}.json',{'error':str(error)})
                    try:
                        msg=int(early['_msgbufp'],16)
                        header=memory.read(msg,24)
                        _,size,cursor,_,pointer=struct.unpack('<4IQ',header)
                        if 0<size<=16<<20 and 0<cursor<size:
                            count=min(cursor,256<<10)
                            raw=memory.read(pointer+cursor-count,count)
                            (args.output/f'kernel-{index:03}.log').write_bytes(raw.replace(b'\0',b''))
                    except (ValueError,RuntimeError,IOError) as error:
                        record(f'ring-error-{index:03}.json',{'error':str(error)})


                    if early.get('_native_root_block_panic_text'):
                        try:
                            from capture_ios18_resident import capture_init
                            for process in processes:
                                if process['name'] not in ('driverkitd', 'backboardd', 'thermalmonitord', 'SpringBoard'):
                                    continue
                                context=capture_init(memory,kernel_symbols,layout,lambda value:hex(value),
                                    args.output,proc=int(process['address'],16),label=process['name'],max_threads=8)
                                record(process['name']+'-fault-context.json',context)
                        except (NameError,KeyError,ValueError,RuntimeError,IOError) as error:
                            record('service-context-error.json',{'error':str(error)})
                        if '_CpuDataEntries' in kernel_symbols:
                            cpu_report=[]
                            cpu_table=memory.read(kernel_symbols['_CpuDataEntries'],128)
                            for cpu in range(8):
                                data_address=struct.unpack_from('<Q',cpu_table,cpu*16+8)[0]
                                if not data_address:continue
                                row={'cpu':cpu,'cpu_data':hex(data_address)}
                                try:
                                    cpu_data=memory.read(data_address,256)
                                    (args.output/f'fault-cpu{cpu}-data.bin').write_bytes(cpu_data)
                                    row['signals']=hex(struct.unpack_from('<I',cpu_data,72)[0])
                                    thread=struct.unpack_from('<Q',cpu_data,48)[0]
                                    thread=(thread&((1<<38)-1))|(((1<<64)-1)^((1<<38)-1))
                                    thread_data=memory.read(thread,layout['thread']['bytes'])
                                    (args.output/f'fault-cpu{cpu}-thread.bin').write_bytes(thread_data)
                                    stack=struct.unpack_from('<Q',thread_data,layout['thread']['kernel_stack'])[0]
                                    stack=(stack&((1<<38)-1))|(((1<<64)-1)^((1<<38)-1))
                                    row['thread']=hex(thread);row['stack']=hex(stack)
                                    (args.output/f'fault-cpu{cpu}-stack.bin').write_bytes(memory.read(stack,16384))
                                except (ValueError,RuntimeError,IOError) as error:
                                    row['error']=str(error)
                                cpu_report.append(row)
                            record('fault-cpus.json',cpu_report)
                        record('retained-fault.json', {'time':datetime.now().astimezone().isoformat(),
                            'panic':early['_native_root_block_panic_text'], 'status':'USB memory reads and firmware reboot available'})
                        while True:
                            request=args.output/'memory-request.json'
                            if request.exists():
                                operations=json.loads(request.read_text())
                                request.unlink()
                                results=[]
                                process_roots={}
                                for operation in operations:
                                    if operation.get('command') == 'reboot-bootloader':
                                        results.append({'command':'reboot-bootloader','reply':channel.command('reboot-bootloader')})
                                        record('memory-response.json',results)
                                        raise RuntimeError('Firmware reboot requested through retained session')
                                    if operation.get('command') == 'capture-processes':
                                        from capture_ios18_resident import capture_init, capture_processes
                                        selected=operation['processes']
                                        destination=args.output/f'requested-{time.time_ns()}'
                                        destination.mkdir()
                                        for process in capture_processes(memory,kernel_symbols,layout['proc']):
                                            if process['name'].startswith(tuple(selected)):
                                                try:
                                                    context=capture_init(memory,kernel_symbols,layout,lambda value:hex(value),
                                                        destination,proc=int(process['address'],16),label=process['name'],max_threads=32)
                                                    (destination/(process['name']+'-live-context.json')).write_text(json.dumps(context,indent=2)+'\n')
                                                except (KeyError,ValueError,RuntimeError,IOError) as error:
                                                    (destination/(process['name']+'-context-error.json')).write_text(json.dumps({'error':str(error)},indent=2)+'\n')
                                        results.append({'command':'capture-processes','output':str(destination)})
                                        continue
                                    name=operation['name']
                                    if not re.fullmatch(r'[A-Za-z0-9_.-]+',name):
                                        raise ValueError('Invalid memory capture file name')
                                    try:
                                        if operation.get('command') in ('device-logs', 'device-registration-status'):
                                            results.append(request_device_logs(operation)); continue
                                        address=int(operation['address'],16)
                                        length=int(operation['size'])
                                        if not 0 < length <= 1<<20:raise ValueError('Memory read length outside 1..1048576')
                                        if 'pid' in operation:
                                            pid=int(operation['pid'])
                                            if pid not in process_roots:
                                                from capture_ios18_resident import capture_processes
                                                processes=capture_processes(memory,kernel_symbols,layout['proc'])
                                                process=next(p for p in processes if p['pid']==pid)
                                                if process['name'] != operation['process']:
                                                    raise ValueError('Process identity changed before memory capture')
                                                def read64(at):return int.from_bytes(memory.read(at,8),'little')
                                                def canon(value):return value | (0xffffffffffffffff ^ ((1<<memory.bits)-1)) if value else 0
                                                task=int(process['address'],16)+read64(kernel_symbols['_proc_struct_size'])
                                                vm_map=canon(read64(task+layout['user']['task_map']))
                                                pmap=canon(read64(vm_map+layout['user']['map_pmap']))
                                                process_roots[pid]=read64(pmap+layout['user']['pmap_ttep'])
                                            raw=memory.read(address,length,process_roots[pid],64-(memory.tcr&63))
                                        else:
                                            raw=memory.read(address,length)
                                        (args.output/(name+'.bin')).write_bytes(raw)
                                        results.append({'name':name,'bytes':len(raw)})
                                    except (ValueError,RuntimeError,IOError,StopIteration) as error:
                                        results.append({'name':name,'error':str(error)})
                                record('memory-response.json',results)
                            channel.command('getvar:watchdog')
                            time.sleep(3)


            if index == 0 and state.get('phase') == 'firmware':
                print('continue', channel.command('continue'), flush=True)
            time.sleep(2)
        except OSError as error:
            if error.errno not in (errno.ENOSPC, errno.EDQUOT):
                raise
            redirect_evidence(error)
            continue

except Exception as error:
    record('channel-error.json', {'time': datetime.now().astimezone().isoformat(), 'error': str(error)})
finally:
    if channel:
        channel.close()
if locals().get('graceful_stop'):
    raise SystemExit(0)
for attempt in range(12):
    if bootloader():
        record('recovery.json', {'time': datetime.now().astimezone().isoformat(), 'state': 'bootloader 556fdfe'})
        break
    time.sleep(5)
