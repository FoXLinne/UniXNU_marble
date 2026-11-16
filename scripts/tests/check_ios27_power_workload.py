from pathlib import Path
import json,os,runpy,struct,sys,tempfile,time
repo=Path(__file__).resolve().parents[2];script=repo/'scripts/observe_ios27_power_workload.py'
real_sleep,real_time,real_ns=time.sleep,time.time,time.time_ns
results=[]
for mode in ['charging','discharging','panel-interrupted','channel-ended']:
 with tempfile.TemporaryDirectory(prefix='power-workload-test-') as temp:
  root=Path(temp);now=[1000.0];seq=[0];on=[1]
  (root/'rows-000.bin').write_bytes(b'PR compiling=0 libs=165/165')
  def sample():
   n=seq[0];s=f'{n:03}.bin';b=[0]*16;b[1]=30;b[3]=120 if mode=='charging' else -400;b[4]=3000
   raw=[0]*10;raw[4]=1000+n if mode=='charging' else 1000-n
   light=[3,0,0,0,0,0,0,on[0]];gpu=[0]*16;gpu[0]=1;gpu[1]=int(not on[0]);gpu[7]=60;gpu[11]=220000000 if not on[0] else 580000000
   for label,fmt,data in [('battery','<16i',b),('battery-raw','<10i',raw),('backlight','<8I',light),('gpu-idle-power','<16Q',gpu)]:
    p=root/(label+'-'+s);p.write_bytes(struct.pack(fmt,*data));os.utime(p,(now[0],now[0]))
  sample()
  def sleep(seconds):
   now[0]+=seconds;seq[0]+=1
   request=root/'ui-request.json'
   if request.exists():
    ops=json.loads(request.read_text());request.unlink()
    if ops[0].get('command')=='button':on[0]^=1
    response=root/'ui-response.json';response.write_text(json.dumps(dict(operations=ops)));os.utime(response,(now[0],now[0]))
   if mode=='panel-interrupted' and now[0]>1300:on[0]=1
   if mode=='channel-ended' and now[0]>1150:(root/'channel-error.json').write_text('{}')
   sample()
  time.sleep=sleep;time.time=lambda:now[0];time.time_ns=lambda:int(now[0]*1e9)
  oldargv=sys.argv;sys.argv=[str(script),str(root)]
  try:
   try:runpy.run_path(str(script),run_name='__main__')
   except SystemExit:pass
  finally:sys.argv=oldargv;time.sleep, time.time, time.time_ns=real_sleep,real_time,real_ns
  state=json.loads((root/'gpu-power-workload-status.json').read_text())
  if mode in ['charging','discharging']:
   assert state['status']=='measurement-and-wake-actions-complete',state
   assert state['duration_s']>=600
   assert state['net_charging']==(mode=='charging')
   assert state['after_wake_gpu_words'][11]==580000000
   assert len(state['actions'])==4
  elif mode=='panel-interrupted':assert state['status']=='screen-off-window-interrupted'
  else:assert state['status']=='channel-ended-during-power-experiment'
  results.append(dict(case=mode,status='passed',result=state['status']))
out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/power-workload-result.json';out.write_text(json.dumps(results,indent=2))
print(json.dumps(results))
