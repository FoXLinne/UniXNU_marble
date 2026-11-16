"""Own-source HID command mailbox at a genuine timer checkpoint; QEMU only."""
import hashlib,json,subprocess
from pathlib import Path

FIELDS={'native_hid_stage':4,'native_hid_load_status':4,'native_hid_matching_load_status':4,
 'native_hid_input_meta_status':4,'native_hid_input_object':8,'native_hid_input_command':4,
 'native_hid_input_events':32,'native_hid_input_results':32,'native_hid_input_registry':2048,
 'native_legacy_transfer_calls':4,'native_legacy_retirement_calls':4}

class LiveHid:
 def __init__(self,kernel,directory):
  self.directory=Path(directory);self.directory.mkdir(parents=True,exist_ok=True)
  self.last_id=0;self.events=[]
  output=subprocess.check_output(['xcrun','nm','-U',str(kernel)],text=True,timeout=15)
  names={p[2][1:]:int(p[0],16) for line in output.splitlines() if len(p:=line.split())==3}
  self.address=names['native_hid_live_checkpoint'];self.fields={n:(names[n],size) for n,size in FIELDS.items()}
  for n,size in {'native_keystore_stage':4,'native_keystore_load_status':4,
                 'native_keystore_service':8,'native_keystore_class_size':4,
                 'osvariant_status':8, 'native_keyevents_stage':4, 'native_keyevents_status':4,
                 'native_keyevents_service':8, 'native_keyevents_refused_opens':4}.items():
   if n in names:self.fields[n]=(names[n],size)
  for n,size in {'native_legacy_stage':4, 'native_legacy_transfer_status':4,
                 'native_legacy_transfer_bytes':8, 'native_legacy_swap_validation_calls':4,
                 'native_legacy_swap_validation_result':4, 'native_legacy_swap_validation_reason':4}.items():
   if n in names:self.fields[n]=(names[n],size)
  # Synthetic touch (QEMU only): the same published finger set the hardware touch driver writes (see TouchProbe.cpp).
  self.touch={n:names[n] for n in ('native_touch_seq','native_touch_count','native_touch_xs','native_touch_ys','native_touch_ids') if n in names}
  self.identity={'kernel_sha256':hashlib.sha256(Path(kernel).read_bytes()).hexdigest(),'checkpoint':hex(self.address)}
  self.publish({'state':'waiting-for-real-timer','identity':self.identity})
 def publish(self,status):
  temporary=self.directory/'status.tmp';temporary.write_text(json.dumps(status,indent=2)+'\n');temporary.replace(self.directory/'status.json')
 def checkpoint(self,packet,elapsed):
  data={}
  for name,(address,size) in self.fields.items():
   raw=packet(f'm{address:x},{size:x}')
   if raw.startswith('E') or len(raw)!=size*2:raise RuntimeError('Cannot read owned HID observation '+name)
   b=bytes.fromhex(raw)
   if name.endswith('registry'):value=b.split(b'\0',1)[0].decode(errors='replace')
   elif size==32:value=[int.from_bytes(b[i:i+4],'little',signed=name.endswith('results')) for i in range(0,32,4)]
   else:value=int.from_bytes(b,'little',signed=name.endswith('status'))
   data[name]=value
  request_path=self.directory/'command.json';stop=False
  if request_path.exists():
   if request_path.stat().st_size>1024:raise RuntimeError('Oversized live HID command')
   request=json.loads(request_path.read_text())
   touch=request.get('action') in ('touch','lift')
   if (set(request)!={'id','action'} and not (touch and set(request)<={'id','action','x','y'})) or type(request['id']) is not int or not 1<=request['id']<=10000 or request['action'] not in ('home','stop','touch','lift'):
    raise RuntimeError('Invalid live HID command; only bounded Home, stop, touch or lift accepted')
   if request['id']>self.last_id and touch:
    if len(self.touch)!=5:raise RuntimeError('Kernel has no touch publication symbols')
    def put(address,value):
     if packet(f'M{address:x},4:'+int(value&0xffffffff).to_bytes(4,'little').hex())!='OK':raise RuntimeError('Touch publication write failed')
    seq=int.from_bytes(bytes.fromhex(packet(f"m{self.touch['native_touch_seq']:x},4")),'little')
    if request['action']=='touch':
     x,y=request['x'],request['y']
     if type(x) is not int or type(y) is not int or not 0<=x<30480 or not 0<=y<20320:raise RuntimeError('Touch coordinate outside the 30480x20320 axes')
     put(self.touch['native_touch_ids'],1);put(self.touch['native_touch_xs'],x);put(self.touch['native_touch_ys'],y);put(self.touch['native_touch_count'],1)
    else:put(self.touch['native_touch_count'],0)
    put(self.touch['native_touch_seq'],(seq+2)&0xfffffffe)
    self.last_id=request['id'];self.events.append({**request,'elapsed_seconds':round(elapsed,3)})
   elif request['id']>self.last_id:
    if request['action']=='stop':stop=True;self.last_id=request['id']
    elif data['native_hid_input_command']==0:
     address=self.fields['native_hid_input_command'][0]
     if packet(f'M{address:x},4:01000000')!='OK' or packet(f'm{address:x},4')!='01000000':
      raise RuntimeError('Owned input mailbox write/readback failed')
     self.last_id=request['id'];data['native_hid_input_command']=1
    if self.last_id==request['id']:self.events.append({**request,'elapsed_seconds':round(elapsed,3)})
  self.publish({'state':'stopping' if stop else 'running','identity':self.identity,'elapsed_seconds':round(elapsed,3),
                'accepted_command_id':self.last_id,'commands':self.events,'observed':data})
  return stop
