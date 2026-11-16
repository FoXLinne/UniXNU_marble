#!/usr/bin/env python3
"""Measure a quiescent GPU power candidate through its sole USB observer."""
from pathlib import Path
import json
import os
import struct
import subprocess
import sys
import time

root=Path(sys.argv[1])
report=root/'gpu-power-workload-status.json'
state=dict(status='waiting-for-cache',samples=[])

def save():
    state['observed_at']=time.time()
    temporary=report.with_suffix('.tmp')
    temporary.write_text(json.dumps(state,indent=2)+'\n')
    temporary.replace(report)

def alive():
    if (root/'channel-error.json').exists():
        state['status']='channel-ended-during-power-experiment'
        save()
        return False
    return True

def latest(prefix,fmt):
    files=sorted(root.glob(prefix+'-[0-9]*.bin'))
    if not files:return None
    try:return struct.unpack(fmt,files[-1].read_bytes())
    except struct.error:return None

def queue(name,operations):
    request=root/name
    temporary=root/f'power-request-{os.getpid()}.tmp'
    temporary.write_text(json.dumps(operations)+'\n')
    try:os.link(temporary,request);return True
    except FileExistsError:return False
    finally:temporary.unlink(missing_ok=True)

def action(operation):
    while alive():
        began=time.time_ns()
        if queue('ui-request.json',[operation]):break
        time.sleep(15)
    else:return False
    while alive():
        response=root/'ui-response.json'
        try:
            data=json.loads(response.read_text())
            if response.stat().st_mtime_ns>=began and data.get('operations')==[operation]:
                state.setdefault('actions',[]).append(data);save()
                if data.get('error'):raise RuntimeError(data['error'])
                return True
        except (FileNotFoundError,json.JSONDecodeError):pass
        time.sleep(15)
    return False

save()
while alive():
    rows=sorted(root.glob('rows-[0-9]*.bin'))
    backlight=latest('backlight','<8I')
    if rows and b'PR compiling=0 libs=165/165' in rows[-1].read_bytes() and backlight and backlight[0]==3:break
    time.sleep(30)
else:sys.exit(0)
state['status']='requesting-screen-off';save()
if backlight[7] and not action(dict(command='button',button='power')):sys.exit(0)
while alive():
    backlight=latest('backlight','<8I')
    if backlight and backlight[7]==0:break
    time.sleep(15)
else:sys.exit(0)
began=time.time();state.update(status='measuring-screen-off',began_at=began);seen=set();save()
while alive():
    for f in sorted(root.glob('battery-[0-9]*.bin')):
        if f.name in seen or f.stat().st_mtime<began:continue
        suffix=f.name.removeprefix('battery-')
        paths=[root/(prefix+suffix) for prefix in ['backlight-','battery-raw-','gpu-idle-power-']]
        if not all(p.exists() for p in paths):continue
        try:
            b=struct.unpack('<16i',f.read_bytes());l=struct.unpack('<8I',paths[0].read_bytes())
            raw=struct.unpack('<10i',paths[1].read_bytes());gpu=struct.unpack('<16Q',paths[2].read_bytes())
        except struct.error:continue
        seen.add(f.name)
        state['samples'].append(dict(source=f.name,time=f.stat().st_mtime,
            current_ma=b[3],percent=b[1],temperature_c=b[4]/100,charge_raw=raw[4],
            panel_on=l[7],gpu_low_confirmed=gpu[1],gpu_failures=gpu[4],
            requested_gpu_hz=gpu[11],transition_max_us=gpu[7]))
    save()
    samples=state['samples']
    if samples and samples[-1]['time']-samples[0]['time']>=600:break
    time.sleep(30)
else:sys.exit(0)
valid=[s for s in samples if s['panel_on']==0]
if len(valid)!=len(samples) or len(valid)<2:
    state['status']='screen-off-window-interrupted';save();sys.exit(0)
duration=valid[-1]['time']-valid[0]['time']
mean=sum((a['current_ma']+b['current_ma'])/2*(b['time']-a['time'])
         for a,b in zip(valid,valid[1:]))/duration
state.update(status='screen-off-measurement-complete',duration_s=duration,
    time_weighted_current_ma=mean,charge_start=valid[0]['charge_raw'],
    charge_end=valid[-1]['charge_raw'],temperature_start_c=valid[0]['temperature_c'],
    temperature_end_c=valid[-1]['temperature_c'],
    confirmed_low_samples=sum(s['gpu_low_confirmed']!=0 for s in valid),
    net_charging=mean>0 and valid[-1]['charge_raw']>valid[0]['charge_raw'])
save()
if list(root.glob('cpu-power-pc-[0-9]*.bin')):
    decoder=Path(__file__).with_name('decode_ios27_cpu_power.py')
    result=subprocess.run([sys.executable,str(decoder),str(root),'--screen-off-window','--recent-seconds','120'],capture_output=True,text=True)
    (root/'cpu-power-decode.log').write_text(result.stdout+result.stderr)
    state['cpu_profile_decoded']=result.returncode==0
    save()
backlight=latest('backlight','<8I')
if backlight and not backlight[7] and not action(dict(command='button',button='power')):sys.exit(0)
for operation in [dict(command='swipe',**{'from':[540,2390],'to':[540,900]},duration_ms=300,capture=False),
                  dict(command='swipe',**{'from':[1000,20],'to':[1000,1300]},duration_ms=600,capture=False)]:
    if not action(operation):sys.exit(0)
state['after_wake_gpu_words']=latest('gpu-idle-power','<16Q')
words=state['after_wake_gpu_words']
state['wake_peak_vote_confirmed']=bool(words and words[11]==580000000 and not words[1] and not words[13])
state['fresh_screen_requested']=queue('context-request.json',dict(processes=[],fresh_screen=True))
state['status']='measurement-and-wake-actions-complete';save()
