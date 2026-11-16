#!/usr/bin/env python3
"""Exercise navigation after cache completion through the existing USB observer."""
from datetime import datetime
import json
import os
from pathlib import Path
import re
import sys
import time

root = Path(sys.argv[1])
state = dict(status='waiting-for-cache', completed_actions=[])
def save():
    state['observed_at'] = datetime.now().astimezone().isoformat()
    temporary = root/'gpu-workload-status.tmp'
    temporary.write_text(json.dumps(state, indent=2)+'\n')
    temporary.replace(root/'gpu-workload-status.json')
def disconnected():
    if (root/'channel-error.json').exists():
        state['status']='channel-disconnected';save();return True
    return False
def snapshot():
    files = sorted(root.glob('early-*.json'), key=lambda p:p.stat().st_mtime_ns)
    if not files:return {}
    value = json.loads(files[-1].read_text())
    return dict(file=files[-1].name, data=value)
def queue(operations):
    request=root/'ui-request.json';temporary=root/f'workload-input-{os.getpid()}.tmp'
    if request.exists():return False
    temporary.write_text(json.dumps(operations)+'\n')
    try:os.link(temporary,request)
    except FileExistsError:return False
    finally:temporary.unlink()
    return True
save()
while True:
    if disconnected():sys.exit(0)
    rows = sorted(root.glob('rows-*.bin'), key=lambda p:p.stat().st_mtime_ns)
    if rows:
        raw=rows[-1].read_bytes()
        text='\n'.join(raw[n:n+96].split(b'\0')[0].decode(errors='replace') for n in range(0,len(raw),96))
        if re.search(r'PR compiling=0 libs=165/165',text):break
    time.sleep(30)
state.update(status='exercising-navigation', before=snapshot());save()
actions=[dict(command='swipe', **{'from':[540,2390],'to':[540,900]},duration_ms=300,capture=False)]
for _ in range(4):
    actions.extend([dict(command='swipe', **{'from':[1000,20],'to':[1000,1300]},duration_ms=600,capture=False),
                    dict(command='swipe', **{'from':[540,2390],'to':[540,900]},duration_ms=300,capture=False)])
for action in actions:
    while not queue([action]):
        if disconnected():sys.exit(0)
        time.sleep(15)
    began=time.monotonic()
    while True:
        if disconnected():sys.exit(0)
        time.sleep(15)
        try:response=json.loads((root/'ui-response.json').read_text())
        except (FileNotFoundError,json.JSONDecodeError):continue
        # Consecutive requests differ; a consumed queue plus a matching response
        # identifies acknowledgement without using the screenshot path.
        if response.get('operations')==[action] and not (root/'ui-request.json').exists():
            state['completed_actions'].append(dict(action=action,response=response,elapsed=time.monotonic()-began))
            if response.get('error'):
                state.update(status='input-error',after=snapshot());save();sys.exit(1)
            save();break
        if time.monotonic()-began>120:
            state.update(status='input-acknowledgement-pending',after=snapshot());save()
request=root/'context-request.json';temporary=root/f'workload-screen-{os.getpid()}.tmp'
temporary.write_text(json.dumps(dict(processes=[],fresh_screen=True))+'\n')
try:
    os.link(temporary,request)
    state['fresh_rendered_evidence_requested']=True
except FileExistsError:
    state['fresh_rendered_evidence_requested']=False
finally:
    temporary.unlink()
state.update(status='navigation-actions-acknowledged',after=snapshot());save()
