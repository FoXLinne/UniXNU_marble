#!/usr/bin/env python3
"""Stage byte-identical btqca sources from the locked Linux revision."""
from pathlib import Path
import hashlib,json,os,urllib.request
root=Path(__file__).resolve().parents[1]
lock=json.loads((root/'tools/source-lock.json').read_text())
out=root/'out/linux';out.mkdir(parents=True,exist_ok=True)
local=os.environ.get('QBT_LINUX_TREE')
for name,digest in lock['files'].items():
    target=out/Path(name).name
    if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest()==digest:
        continue
    if local:
        data=(Path(local)/name).read_bytes()
    else:
        url=lock['repository'].replace('github.com','raw.githubusercontent.com')+'/'+lock['commit']+'/'+name
        with urllib.request.urlopen(url,timeout=30) as response:data=response.read()
    if hashlib.sha256(data).hexdigest()!=digest:raise SystemExit('Linux identity mismatch: '+name)
    target.write_bytes(data)
print('Verified original WCN7850 btqca source: '+lock['commit'])
