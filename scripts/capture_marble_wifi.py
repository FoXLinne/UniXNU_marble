import ctypes as C,json,plistlib,datetime
from pathlib import Path
out=Path(__file__).resolve().parent
lib=C.CDLL('/Applications/i4Tools.app/Contents/Frameworks/libimobiledevice.dylib')
plist=C.CDLL('/Applications/i4Tools.app/Contents/Frameworks/libplist2.dylib')
libc=C.CDLL('/usr/lib/libSystem.B.dylib')
P=C.c_void_p;CP=C.c_char_p;I=C.c_int

def bind(name,args):
 f=getattr(lib,name);f.argtypes=args;f.restype=I;return f
new=bind('idevice_new',[C.POINTER(P),CP]);free=bind('idevice_free',[P])
handshake=bind('lockdownd_client_new_with_handshake',[P,C.POINTER(P),CP])
get=bind('lockdownd_get_value',[P,CP,CP,C.POINTER(P)])
start=bind('lockdownd_start_service',[P,CP,C.POINTER(P)])
ldfree=bind('lockdownd_client_free',[P]);servicefree=bind('lockdownd_service_descriptor_free',[P])
plist.plist_to_xml.argtypes=[P,C.POINTER(P),C.POINTER(C.c_uint32)];plist.plist_free.argtypes=[P];libc.free.argtypes=[P]
def decode(node):
 buf=P();size=C.c_uint32();plist.plist_to_xml(node,C.byref(buf),C.byref(size))
 try:return plistlib.loads(C.string_at(buf,size.value))
 finally:libc.free(buf);plist.plist_free(node)
def check(code,label):
 if code:raise RuntimeError(f'{label}: {code}')
import sys,struct
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
dev=P();client=P();check(new(C.byref(dev),None),'device');check(handshake(dev,C.byref(client),b'wifi-observation'),'lockdown')
report={'time':datetime.datetime.now().astimezone().isoformat()}
try:
 desc=P();check(start(client,b'com.apple.mobile.diagnostics_relay',C.byref(desc)),'service');diag=P()
 check(bind('diagnostics_relay_client_new',[P,P,C.POINTER(P)])(dev,desc,C.byref(diag)),'diagnostics');servicefree(desc)
 try:
  node=P();rc=bind('diagnostics_relay_query_ioregistry_entry',[P,CP,CP,C.POINTER(P)])(diag,b'MarbleWifiDiagnostics',b'IOService',C.byref(node));report['query_status']=rc
  data=decode(node) if rc==0 and node else {}; (out/'wifi-registry.plist').write_bytes(plistlib.dumps(data))
  def find(value,key):
   if isinstance(value,dict):
    if key in value:return value[key]
    for x in value.values():
     y=find(x,key)
     if y is not None:return y
   if isinstance(value,list):
    for x in value:
     y=find(x,key)
     if y is not None:return y
   return None
  for key,count in [('BoardStats',24),('RuntimeStats',16),('SmmuRegisters',16),('AtosStats',8),('DmaFault',8),('BhiRegisters',16),('PciRegisters',8)]:
   raw=find(data,key)
   if isinstance(raw,bytes) and len(raw)==count*4:report[key]=list(struct.unpack('<'+'I'*count,raw))
  raw=find(data,'Logs');count=find(data,'LogCount')
  if isinstance(raw,bytes) and isinstance(count,int):report['logs']=[raw[(i%128)*160:(i%128+1)*160].split(b'\0')[0].decode(errors='replace') for i in range(max(0,count-128),count)]
  if 'BoardStats' not in report:report['registry_response']=data
 finally:bind('diagnostics_relay_client_free',[P])(diag)
finally:ldfree(client);free(dev)
(out/'wifi-status.json').write_text(json.dumps(report,ensure_ascii=False,indent=2,default=lambda x:x.hex() if isinstance(x,bytes) else str(x))+'\n');print(json.dumps(report,ensure_ascii=False,indent=2,default=lambda x:x.hex() if isinstance(x,bytes) else str(x)))
