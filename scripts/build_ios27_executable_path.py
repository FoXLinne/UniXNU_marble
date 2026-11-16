#!/usr/bin/env python3
from pathlib import Path
import sys,subprocess,json,hashlib,re,struct,tempfile,datetime
import os
os.chdir(Path(__file__).resolve().parents[1])
sys.path.insert(0,str(Path(__file__).resolve().parent));from assemble_ios27_runtime import unpack_addon,pack_addon
import argparse
parser=argparse.ArgumentParser(description='Package the driverkitd executable URL adapter in a signed runtime addon')
parser.add_argument('--root',required=True,type=Path)
parser.add_argument('--runtime',required=True,type=Path)
parser.add_argument('--output',type=Path)
args=parser.parse_args()
r=args.root.resolve();runtime=args.runtime.resolve();source=Path('package/ios27-runtime/userspace/MarbleExecutablePath27.m');out=args.output.resolve() if args.output else r/'runtime-payload-execpath';out.mkdir(exist_ok=True);lib=out/'MarbleExecutablePath.dylib'
def run(*args):
 result=subprocess.run(list(map(str,args)),capture_output=True,text=True)
 if result.returncode:print(result.stderr,flush=True);result.check_returncode()
 return result
sdk=run('xcrun','--sdk','iphoneos','--show-sdk-path').stdout.strip()
run('xcrun','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-O2','-Wall','-Wextra','-Werror','-dynamiclib','-fno-objc-arc',source,'-framework','Foundation','-lobjc','-lproc','-install_name','@loader_path/MarbleExecutablePath.dylib','-o',lib)
run('codesign','--force','--sign','-','--pagesize','4096','--digest-algorithm','sha256','--identifier','org.unixnu.marble.executable-path',lib)
run('codesign','--verify','--strict',lib)
files=unpack_addon(runtime/'runtime.gpuaddon');prefix='addons/NativeAdrenoMetal.bundle/'
with tempfile.TemporaryDirectory(prefix='marble-execpath-') as temp:
 bundle=Path(temp)/'NativeAdrenoMetal.bundle';bundle.mkdir()
 for name,(mode,data) in files.items():
  if name.startswith(prefix):
   p=bundle/name[len(prefix):];p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data);p.chmod(mode)
 (bundle/lib.name).write_bytes(lib.read_bytes());(bundle/lib.name).chmod(0o755)
 run('codesign','--force','--sign','-','--pagesize','4096','--digest-algorithm','sha256','--identifier','org.blu-sharky.NativeAdrenoMetal',bundle);run('codesign','--verify','--strict',bundle)
 hashes=[]
 for p in [lib,bundle]:
  detail=run('codesign','-dv','--verbose=4',p);hashes.append(bytes.fromhex(re.search(r'^CDHash=(\w+)$',detail.stderr,re.M)[1]))
 for p in bundle.rglob('*'):
  if p.is_file():files[prefix+p.relative_to(bundle).as_posix()]=(p.stat().st_mode&0o777,p.read_bytes())
pack_addon(files,out/'runtime.gpuaddon')
old=(runtime/'owner.trustcache').read_bytes();count=struct.unpack_from('<I',old,20)[0];entries={old[24+i*24:24+i*24+20]:old[24+i*24+20:24+i*24+24] for i in range(count)}
for h in hashes:entries[h]=bytes([2,0,0,0])
body=b''.join(h+entries[h] for h in sorted(entries));(out/'owner.trustcache').write_bytes(struct.pack('<I16sI',2,hashlib.sha256(body).digest()[:16],len(entries))+body)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
report=json.loads((runtime/'report.json').read_text());report.update(addon=str(out/'runtime.gpuaddon'),addon_sha256=digest(out/'runtime.gpuaddon'),owner_trust_cache=str(out/'owner.trustcache'),trust_cache_sha256=digest(out/'owner.trustcache'),cdhash_count=len(entries),files={name:{'sha256':hashlib.sha256(data).hexdigest(),'mode':mode} for name,(mode,data) in files.items()},executable_path={'source':str(source.resolve()),'sha256':digest(source),'library_sha256':digest(lib),'scope':'NSBundle executablePath main bundle nil result in driverkitd'});(out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
p=Path('CURRENT.json');d=json.loads(p.read_text());d['executable_url']['sources'].pop('package/ios27-runtime/MarbleExecutablePath27.m',None);d['executable_url']['sources'][str(source)]=digest(source);d['executable_url'].update(runtime=str(out/'report.json'),runtime_addon_sha256=report['addon_sha256'],status='userspace helper built; kernel policy building');d['updated_at']=datetime.datetime.now().astimezone().isoformat();p.write_text(json.dumps(d,ensure_ascii=False,indent=2)+'\n');print(report['executable_path'],flush=True)
