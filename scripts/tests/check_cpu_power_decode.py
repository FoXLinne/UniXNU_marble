from pathlib import Path
import json,struct,subprocess,sys,tempfile
repo=Path(__file__).resolve().parents[2];root=repo.parents[1];out=root/'outputs/UniXNU_marble/ios27_24B119';sys.path.insert(0,str(repo/'scripts'));from capture_ios27 import symbols_for
candidate=out/'candidate-charge-source-v3/candidate.json';d=json.loads(candidate.read_text());_,symbols=symbols_for(Path(d['kernel']),repo/'ios27/sources/XnuPrelink')
results=[]
for matched in [True,False]:
 with tempfile.TemporaryDirectory(prefix='cpu-profile-fixture-') as tmp:
  e=Path(tmp);(e/'boot.json').write_text(json.dumps(dict(candidate=str(candidate))));(e/'cpu-clock-001.bin').write_bytes(struct.pack('<Q',19200000));(e/'gpu-power-workload-status.json').write_text(json.dumps(dict(samples=[dict(source='battery-001.bin')])));pc=bytearray(16384);lr=bytearray(8192);frames=bytearray(20480);cpu,seq=2,7;slot=seq&63;meta=(1<<32)|(5<<8)|cpu;target=symbols['_PE_parse_boot_argn_internal'];parent=symbols['_PE_parse_boot_argn'];second=symbols['_proc_selfpid']
  struct.pack_into('<4Q',pc,(cpu*64+slot)*32,seq,target,meta,19200000*1000);struct.pack_into('<2Q',lr,(cpu*64+slot)*16,seq,parent+4);struct.pack_into('<5Q',frames,(cpu*64+slot)*40,seq if matched else seq+1,parent+4,second+4,parent+4,0)
  (e/'cpu-power-pc-001.bin').write_bytes(pc);(e/'cpu-power-lr-001.bin').write_bytes(lr);(e/'cpu-power-frames-001.bin').write_bytes(frames)
  p=subprocess.run([sys.executable,str(repo/'scripts/decode_ios27_cpu_power.py'),str(e),'--screen-off-window','--recent-seconds','120'],capture_output=True,text=True,check=True);data=json.loads((e/'cpu-power-profile.json').read_text());assert data['busy_samples']==1;assert len(data['rows'][0]['parents'])==(3 if matched else 0);results.append(dict(matched=matched,status='passed'))
(out/'host-tests/cpu-power-decode-result.json').write_text(json.dumps(results,indent=2));print('PASS: actual kernel symbols, same-sequence parent frames, mismatched-frame exclusion and power-window bounds')
