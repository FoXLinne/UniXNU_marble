"""Disassemble every Powerlog replacement and check the retained FP arithmetic."""
from pathlib import Path
import json
import re
import subprocess
repo=Path(__file__).resolve().parents[2]
mc=Path('/opt/homebrew/opt/llvm/bin/llvm-mc')
profile=json.loads((repo/'profiles/runtime/ios27-24B119/fixups.json').read_text())
source=(repo/'package/ios27-runtime/MarbleUiPolicy27.c').read_text()
cache=Path('/private/tmp/marble-24b119-cryptex/System/Library/Caches/com.apple.dyld')
def decode(data):
    text='\n'.join(' '.join(hex(v) for v in data[i:i+4]) for i in range(0,len(data),4))+'\n'
    return subprocess.run([str(mc),'--disassemble','--triple=aarch64'],input=text,text=True,capture_output=True,check=True)
count=0
for q in profile['entries']:
    if q['group']!='powerlog':continue
    entry=q['entry'];assert entry in source
    name,offset,before,after=entry.split(',');before=bytes.fromhex(before);after=bytes.fromhex(after)
    with (cache/name).open('rb') as f:f.seek(int(offset,16));assert f.read(len(before))==before
    result=decode(after);assert 'invalid instruction encoding' not in result.stderr,(entry,result.stderr)
    if int(offset,16)==0x27660fc:
        assert after[4:8]==before[4:8] # retained call
        assert after[12:16]==before[12:16]==bytes.fromhex('0039691e')
        assert 'fsub\td0, d8, d9' in result.stdout
        for ins in ['fcvtzs\tx24, d0','scvtf\td8, x24','fcvtzs\tx27, d0']:assert ins in result.stdout
    count+=1
assert 'invalid instruction encoding' in decode(bytes.fromhex('0039699e')).stderr
print(f'PASS: {count} Powerlog patches match kernel policy and on-disk originals; replacements decode; FP subtraction and call preserved; invalid old encoding detected')
