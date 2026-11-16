#!/usr/bin/env python3
"""Decode per-core interrupted-PC samples retained by the active observer."""
from pathlib import Path
from collections import Counter
import bisect,json,struct,sys
from capture_ios27 import symbols_for
root=Path(sys.argv[1]);boot=json.loads((root/'boot.json').read_text());candidate=json.loads(Path(boot['candidate']).read_text());repo=Path(__file__).resolve().parents[1]
_,symbols=symbols_for(Path(candidate['kernel']),repo/'ios27/sources/XnuPrelink');by_address={}
for name,address in symbols.items():by_address.setdefault(address,name)
addresses=sorted(by_address);names={};process_files=sorted(root.glob('processes-[0-9]*.json'),key=lambda p:p.stat().st_mtime)
def symbolicate(pc):
    index=bisect.bisect_right(addresses,pc)-1
    return by_address[addresses[index]] if index>=0 else hex(pc)
if process_files:names={p['pid']:p['name'] for p in json.loads(process_files[-1].read_text())}
seen=set();counts=Counter();modes=Counter();raw=[]
files=sorted(root.glob('cpu-power-pc-[0-9]*.bin'),key=lambda p:p.stat().st_mtime)
if '--screen-off-window' in sys.argv:
    window=json.loads((root/'gpu-power-workload-status.json').read_text())
    if window.get('samples'):
        last=int(window['samples'][-1]['source'].removesuffix('.bin').rsplit('-',1)[1])
        files=[p for p in files if int(p.stem.rsplit('-',1)[1])<=last]
cutoff=0
if files and '--recent-seconds' in sys.argv:
    hz=struct.unpack('<Q',sorted(root.glob('cpu-clock-[0-9]*.bin'))[-1].read_bytes())[0]
    latest=files[-1].read_bytes()
    newest=max((struct.unpack_from('<4Q',latest,i)[3] for i in range(0,len(latest),32)),default=0)
    cutoff=max(0,newest-int(sys.argv[sys.argv.index('--recent-seconds')+1])*hz)
for p in files:
 data=p.read_bytes()
 if len(data)!=16384:continue
 callers=p.with_name(p.name.replace('cpu-power-pc-','cpu-power-lr-'))
 lrdata=callers.read_bytes() if callers.exists() else b''
 ancestor_file=p.with_name(p.name.replace('cpu-power-pc-','cpu-power-frames-'))
 ancestors=ancestor_file.read_bytes() if ancestor_file.exists() else b''
 for cpu in range(8):
  for slot in range(64):
   sequence,pc,meta,ticks=struct.unpack_from('<4Q',data,(cpu*64+slot)*32)
   if not sequence or sequence&63!=slot or ticks<cutoff or meta&255!=cpu or (cpu,sequence) in seen:continue
   seen.add((cpu,sequence));pid=meta>>32;mode=(meta>>8)&31;idle=(meta>>16)&1;modes[(mode,idle)]+=1
   if idle:continue
   if mode==0:label='user:'+hex(pc)
   elif mode in (4,5):
    label=symbolicate(pc)
   else:continue
   caller=None
   if len(lrdata)==8192:
    lrseq,lr=struct.unpack_from('<2Q',lrdata,(cpu*64+slot)*16)
    if lrseq==sequence and lr:
     if mode in (4,5):
      lr=(lr&((1<<38)-1))|(((1<<64)-1)^((1<<38)-1));caller=symbolicate(lr-4)
     else:caller='user:'+hex(lr&((1<<39)-1))
   parents=[]
   if len(ancestors)==20480:
    frames=struct.unpack_from('<5Q',ancestors,(cpu*64+slot)*40)
    if frames[0]==sequence:
     for lr in frames[1:]:
      if not lr:continue
      if mode in (4,5):
       lr=(lr&((1<<38)-1))|(((1<<64)-1)^((1<<38)-1));parents.append(symbolicate(lr-4))
      else:parents.append('user:'+hex(lr&((1<<39)-1)))
   counts[(pid,label,caller,tuple(parents))]+=1;raw.append(dict(cpu=cpu,sequence=sequence,pid=pid,mode=mode,pc=hex(pc),counter=ticks,label=label,caller=caller,parents=parents))
rows=[dict(pid=pid,process=names.get(pid,'kernel_task' if pid==0 else '?'),symbol=symbol,caller=caller,parents=list(parents),samples=n) for (pid,symbol,caller,parents),n in counts.most_common()]
report=dict(status='decoded',unique_samples=len(seen),busy_samples=len(raw),cutoff_counter=cutoff,screen_off_window='--screen-off-window' in sys.argv,selected_snapshots=len(files),mode_and_idle_counts=[dict(mode=k[0],from_idle=k[1],samples=v) for k,v in modes.items()],rows=rows,raw=raw)
(root/'cpu-power-profile.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(rows[:20]))
