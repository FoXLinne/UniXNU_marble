#!/usr/bin/env python3
"""Maintain temporary marble boots, preserve evidence and recharge through Android."""
from pathlib import Path
from datetime import datetime
import fcntl,hashlib,json,os,re,subprocess,sys,time,struct,plistlib
REPO=Path(__file__).resolve().parents[1]
ROOT=REPO.parents[1]
OUT=ROOT/'outputs/UniXNU_marble/ios27_24B119'
CURRENT=REPO/'CURRENT.json'
PYTHON=ROOT/'tools/marble-build/venv/bin/python'
lock=(OUT/'charge-resume.lock').open('a')
try:fcntl.flock(lock.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
except BlockingIOError:raise SystemExit(0)
state={'status':'detecting','resume_percent':30,'charge_below_percent':15}
children={}
resident_seen=False
try:
 previous=json.loads((OUT/'power-resume.json').read_text())
 if previous.get('fault_image_sha256'):state['fault_image_sha256']=previous['fault_image_sha256']
 if previous.get('status')=='resumed' and previous.get('evidence') and not (Path(previous['evidence'])/'boot.json').exists():
  state.update(status='waiting-for-bootloader',candidate=previous['candidate'])
except (FileNotFoundError,ValueError):pass
try:
 pending=json.loads((OUT/'candidate-switch.json').read_text())
 if pending.get('status')=='waiting-for-bootloader':
  state.update(status='waiting-for-bootloader',candidate=pending['candidate'],controlled_switch=pending)
except (FileNotFoundError,ValueError):pass

def run(args):
 return subprocess.run(args,capture_output=True,text=True,timeout=20)
def now():return datetime.now().astimezone().isoformat()
def write(path,value):
 temp=path.with_name(path.name+'.'+str(os.getpid())+'.tmp');temp.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n');temp.replace(path)
def save():
 state['observed_at']=now();write(OUT/'power-resume.json',state)
def spawn(args,log):
 with log.open('a') as f:return subprocess.Popen([str(x) for x in args],cwd=REPO,stdout=f,stderr=subprocess.STDOUT,start_new_session=True)
def resident_present():
 try:
  result=subprocess.run(['ioreg','-p','IOUSB','-l','-a'],capture_output=True,timeout=20)
  payload=plistlib.loads(result.stdout)
  pending=payload if isinstance(payload,list) else [payload]
  while pending:
   node=pending.pop()
   if not isinstance(node,dict):continue
   pending.extend(node.get('IORegistryEntryChildren',[]))
   if node.get('idVendor')==0x18d1 and (node.get('idProduct')==0xd00e or
      (node.get('idProduct')==0xd00d and node.get('USB Serial Number')=='marble')):return True
 except (ValueError,KeyError,subprocess.TimeoutExpired):pass
 return False
def attach_existing():
 global resident_seen
 if any(child.poll() is None for child in children.values()):return False
 present=resident_present()
 state.update(resident_usb_present=present,last_device_check_at=now());state.pop('last_error',None);save()
 if not present:resident_seen=False;return False
 if resident_seen:return False
 resident_seen=True
 active=json.loads(CURRENT.read_text()).get('active_candidate',{}).get('candidate')
 if active:start_observer(Path(active),True);return True
 return False
def candidate_path():
 d=json.loads(CURRENT.read_text());return Path(d['prepared_candidate']['candidate'])
def audit(path):
 q=json.loads(path.read_text());a=json.loads((path.parent/'host-audit.json').read_text())
 assert a['status']=='host-audited' and a['image_sha256']==q['image_sha256']
 h=hashlib.sha256()
 with Path(q['image']).open('rb') as f:
  for chunk in iter(lambda:f.read(1<<20),b''):h.update(chunk)
 assert h.hexdigest()==q['image_sha256']
 ready=run([str(PYTHON),'-c','import usb.core, usb.backend.libusb1'])
 assert ready.returncode==0,ready.stderr
 return q

def start_observer(path,attach=False):
 q=audit(path);e=OUT/('device-'+datetime.now().strftime('%Y%m%d'))/(path.parent.name.removeprefix('candidate-')+'-'+datetime.now().strftime('%H%M%S'));e.mkdir(parents=True)
 d=json.loads(CURRENT.read_text());d['updated_at']=now();d['execution_state'].update(status='temporary candidate boot requested' if not attach else 'attaching after host restart',device_observation=str(e),observed_at=now());d['active_candidate']={k:q[k] for k in ('image','image_sha256','kernel','kernel_sha256')};d['active_candidate'].update(candidate=str(path),device_result='pending');write(CURRENT,d)
 children['observer']=spawn([PYTHON,REPO/'scripts/run_ios27_phone.py','--candidate',path,'--output',e]+(['--attach'] if attach else []),e.with_name(e.name+'-observer.log'))
 children['reader']=spawn([PYTHON,REPO/'scripts/watch_ios27_gpu_rows.py',e,'--text-pages','--loss-record','--storage-capacity'],e.with_name(e.name+'-rows.log'))
 experiment=d.get('prepared_candidate',{}).get('experiment') if d.get('prepared_candidate',{}).get('candidate')==str(path) else None
 if attach and d.get('wifi_optimization'):experiment='passive-wifi'
 workload=('observe_ios27_wifi.py' if experiment=='passive-wifi' else
           'observe_ios27_power_workload.py' if experiment=='screen-off-gpu-idle' else 'observe_ios27_gpu_workload.py')
 children['workload']=spawn([PYTHON,REPO/'scripts'/workload,e],e.with_name(e.name+'-workload.log'))
 state.update(status='resumed',candidate=str(path),evidence=str(e),image_sha256=q['image_sha256'],**{k+'_pid':v.pid for k,v in children.items()});save()
 pending=OUT/'candidate-switch.json'
 if state.get('controlled_switch',{}).get('candidate')==str(path) and pending.exists():
  record=json.loads(pending.read_text());record.update(status='boot-requested',evidence=str(e),observed_at=now());write(pending,record)
  pending.rename(OUT/('candidate-switch-'+datetime.now().strftime('%Y%m%d-%H%M%S')+'.json'))

def capture_capacity(e):
 files=sorted(e.glob('storage-capacity-*.bin'))
 if not files:return
 f=files[-1];v=struct.unpack('<8Q',f.read_bytes())
 if not v[1]:return
 result=dict(status='physical capacity projection observed',observed_at=datetime.fromtimestamp(f.stat().st_mtime).astimezone().isoformat(),source=str(f),calls=v[0],projections=v[1],errors=v[2],system_image_bytes=v[3],data_total_bytes=v[4],data_available_bytes=v[5],exposed_total_bytes=v[6],data_free_bytes=v[7],consistent=v[5]<=v[7]<=v[4] and v[6]==v[4],settings_visual='pending')
 write(e/'storage-capacity-result.json',result)
 if not state.get('capacity_captured'):
  d=json.loads(CURRENT.read_text());d['storage_capacity_repair'].update(device_result=result,status='physical API measurement captured');write(CURRENT,d);state['capacity_captured']=True;print(json.dumps(result),flush=True)

save()
while True:
 try:
  devices=run(['adb','devices']).stdout
  android=bool(re.search(r'^556fdfe\s+device$',devices,re.M))
  if android:
   battery=run(['adb','-s','556fdfe','shell','dumpsys','battery']).stdout
   level=re.search(r'^\s*level: (\d+)',battery,re.M)
   if level:
    percent=int(level[1]);power=dict(status='charging',android_percent=percent,charge_below_percent=15,resume_above_percent=30,observed_at=now());write(OUT/'power-guard.json',power)
    run(['adb','-s','556fdfe','shell','input','keyevent','223'])
    path=candidate_path();state.update(status='waiting-for-charge',candidate=str(path),android_percent=percent);save()
    if percent>=30:
     q=audit(path)
     if q['image_sha256']==state.get('fault_image_sha256'):
      state['status']='charged-awaiting-repair';save();time.sleep(60);continue
     reply=run(['adb','-s','556fdfe','reboot','bootloader']);assert reply.returncode==0,reply.stderr
     state.update(status='waiting-for-bootloader',candidate=str(path));save()
  elif state['status']=='waiting-for-bootloader':
   if any(row.split()[:1]==['556fdfe'] for row in run(['fastboot','devices']).stdout.splitlines()):start_observer(Path(state['candidate']))
   else:attach_existing()
  elif state['status']=='requesting-bootloader-for-charge':
   if any(row.split()[:1]==['556fdfe'] for row in run(['fastboot','devices']).stdout.splitlines()):
    reply=run(['fastboot','-s','556fdfe','reboot']);assert reply.returncode==0,reply.stderr;state['status']='awaiting-android';save()
  elif state['status']=='resumed':
   e=Path(state['evidence']);capture_capacity(e)
   logs=sorted(e.glob('kernel-*.log'))
   if logs and time.time()-logs[-1].stat().st_mtime<180:
    samples=re.findall(r'MarbleBattery percent=(\d+) mv=(\d+) ma=(-?\d+) temp=(\d+)',logs[-1].read_text(errors='replace'))
    if samples:
     percent,mv,ma,temp=map(int,samples[-1]);write(OUT/'power-guard.json',dict(status='watching',percent=percent,mv=mv,ma=ma,temperature_raw=temp,evidence=str(e),observed_at=now()))
     if percent<=15 and not (e/'channel-error.json').exists():
      request=e/'memory-request.json';ops=json.loads(request.read_text()) if request.exists() else []
      if not any(x.get('command')=='reboot-bootloader' for x in ops):ops.append({'command':'reboot-bootloader'})
      write(request,ops);state['status']='requesting-bootloader-for-charge';save()
   if state['status']=='resumed' and (e/'channel-error.json').exists():state.update(status='device-channel-fault',fault_evidence=str(e/'channel-error.json'),fault_image_sha256=state['image_sha256']);save()
  elif state['status'] in ('device-channel-fault','waiting-for-device'):
   attach_existing()
  elif state['status']=='detecting':
   d=json.loads(CURRENT.read_text());active=d.get('active_candidate',{})
   if active.get('candidate') and 'charging' not in d['execution_state'].get('status','').lower() and resident_present():start_observer(Path(active['candidate']),True)
   else:state['status']='waiting-for-device';save()
 except Exception as error:
  state['last_error']=str(error);save();print(str(error),flush=True)
 time.sleep(60)
