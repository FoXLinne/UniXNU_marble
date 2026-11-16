"""Compiler relink compatibility and strict invalidation of changed compiler inputs."""
from pathlib import Path
import json
import subprocess
import sys
repo=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(repo/'ios27/source/scripts'))
from shader_cache_identity import canonical_source, compiler_source, select_identity, legacy_id, digest
profile=dict(mesa_revision='revision',target='ios27',compiler='clang',options=['release'],source_diff_sha256='raw',extra_sources={})
old=legacy_id(profile);binary=b'old Mach-O\0'+old.encode()+b'\0';uuid='1'*32
first=select_identity('a'*64,profile,profile,None,binary,uuid)
assert first['cache_id']==old and first['legacy_image_uuids']==[uuid]
state=dict(first,binary_sha256=digest(binary))
changed=dict(profile,source_diff_sha256='diagnostics')
again=select_identity('a'*64,changed,profile,state,binary,uuid)
assert again['cache_id']==old and again['legacy_image_uuids']==[uuid]
for changed in [dict(profile,compiler='new-clang'),dict(profile,options=['different-codegen']),dict(profile,mesa_revision='new-revision')]:
    value=select_identity('b'*64,changed,profile,state,binary,uuid)
    assert value['cache_id']!=old and value['legacy_image_uuids']==[]
assert select_identity('a'*64,profile,profile,None,b'foreign binary',uuid)['legacy_image_uuids']==[]
assert select_identity('a'*64,profile,None,state,b'changed binary',uuid)['legacy_image_uuids']==[]
state['compiler_fingerprint'] = 'c'*64
ownership_profile = dict(profile, source_diff_sha256='bo-lifetime')
compatible = select_identity('b'*64,ownership_profile,profile,state,binary,uuid,'c'*64)
assert compatible['cache_id'] == old
assert select_identity('b'*64,ownership_profile,profile,state,binary,uuid,'d'*64)['cache_id'] != old
assert select_identity('b'*64,ownership_profile,None,state,b'changed binary',uuid,'c'*64)['cache_id'] != old
backend = (repo/'ios27/sources/Mesa/src/freedreno/vulkan/tu_knl_darwin.cc').read_bytes()
normalized = compiler_source('src/freedreno/vulkan/tu_knl_darwin.cc', backend)
assert normalized != compiler_source('src/freedreno/vulkan/tu_knl_darwin.cc',
                                     backend.replace(b'darwin_device_get_gpu_timestamp',b'changed_gpu_timestamp'))
plain=b'int shader_option = 1;\n'
logging=plain+b'\n#ifdef __APPLE__\nextern void tu_darwin_record_device_loss(const char *, int, const char *);\n#endif'
assert canonical_source('src/vulkan/runtime/vk_device.c',logging)==plain
assert canonical_source('src/vulkan/runtime/vk_device.c',logging.replace(b'option = 1',b'option = 2'))!=plain
assert canonical_source('src/freedreno/ir3/ir3.c',logging)==logging
for name in ('src/vulkan/runtime/vk_device.c','src/vulkan/runtime/vk_queue.c'):
    source=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/tree/.cache/mesa-work'
    baseline=subprocess.check_output(['git','-C',str(source),'show','HEAD:'+name])
    assert canonical_source(name,(source/name).read_bytes())==canonical_source(name,baseline), name
out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/cache-identity';out.mkdir(parents=True,exist_ok=True)
source='''#include <cassert>
#include "PrimeCacheIdentity.h"
int main(){
 const std::string runtime="runtime",compiler="compiler",oldImage(32,'1'),newImage(32,'2');
 const std::string lib="/System/example.metallib\\t4096\\t100";
 auto ids=prime::cacheBuildIds(runtime,compiler,newImage,oldImage);
 std::set<std::string> done{lib+"\\t"+runtime+"-"+oldImage};
 assert(prime::progressContains(done,lib,ids));
 std::set<std::string> migrated{lib+"\\t"+ids.front()};
 auto relinked=prime::cacheBuildIds(runtime,compiler,std::string(32,'3'),"");
 assert(prime::progressContains(migrated,lib,relinked));
 assert(!prime::progressContains(migrated,lib,prime::cacheBuildIds(runtime,"new-compiler",newImage,"")));
 assert(!prime::progressContains(done,lib,prime::cacheBuildIds("new-runtime",compiler,newImage,oldImage)));
 assert(!prime::progressContains(done,lib+"changed-size",ids));
 assert(!prime::progressContains(done,lib,prime::cacheBuildIds(runtime,compiler,newImage,"unverified")));
 const std::string function="/System/example.metallib\\tfunction";
 std::set<std::string> partial{function+"\\t"+runtime+"-"+oldImage};
 assert(prime::progressContains(partial,function,ids));
}
'''
(out/'test.cpp').write_text(source)
subprocess.run(['xcrun','clang++','-std=c++17','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-I',str(repo/'ios27/sources/AdrenoMetal'),str(out/'test.cpp'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
print('PASS: diagnostic relink keeps compiler cache ID; compiler/options/revision/binary mismatches reject compatibility; verified legacy library/function progress migrates; runtime/library changes invalidate')
