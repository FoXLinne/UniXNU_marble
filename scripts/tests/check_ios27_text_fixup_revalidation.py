"""Check exact configured fixups while preserving unrelated code-page damage."""
from pathlib import Path
import json
import re
import subprocess
repo = Path(__file__).resolve().parents[2]
r = repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119'
patch = (repo/'patches/ios27-debug/text-fixup-revalidation.patch').read_text()
# Extract the helper added by the production patch.
added = '\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
start = added.index('uint32_t\ncs_text_fixup_original_page(')
end = added.index('\n}\n',start)+3
helper = added[start:end]
profile=json.loads((repo/'profiles/runtime/ios27-24B119/fixups.json').read_text())
page_offset=0x2766000;cache=Path('/private/tmp/marble-24b119-cryptex/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e.36')
with cache.open('rb') as f:f.seek(page_offset);original=f.read(4096)
entries=[];patched=bytearray(original)
for q in profile['entries']:
    name,offset,before,after=q['entry'].split(',');offset=int(offset,16);before=bytes.fromhex(before);after=bytes.fromhex(after)
    if name==cache.name and page_offset<=offset and offset+len(before)<=page_offset+4096:
        at=offset-page_offset;assert patched[at:at+len(before)]==before;patched[at:at+len(after)]=after
        entries.append((name,offset,before,after))
def array(data):return '{'+','.join(str(v) for v in data)+'}'
head='''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096
#define NULLVP NULL
typedef uint64_t memory_object_offset_t;
struct vnode {const char *v_name;};
struct cs_text_fixup {const char *name;uint64_t offset;uint8_t original[32],replacement[32];uint32_t length;};
static struct cs_text_fixup cs_text_fixups[32];
static uint32_t cs_text_fixup_count;
'''
fixture='static const uint8_t original[4096]='+array(original)+';\nstatic const uint8_t patched[4096]='+array(patched)+';\n'
setup=''
for i,(name,offset,before,after) in enumerate(entries):
    setup+=f'cs_text_fixups[{i}]=(struct cs_text_fixup){{"{name}",{offset}ULL,{array(before)},{array(after)},{len(before)}}};\n'
setup+=f'cs_text_fixup_count={len(entries)};\n'
body='''int main(void) {
 struct vnode vnode={"dyld_shared_cache_arm64e.36"};uint8_t input[4096],output[4096];
'''+setup+'''
 memcpy(input,patched,4096);
 assert(cs_text_fixup_original_page(&vnode,0x2766000,input,NULL)==6);
 assert(cs_text_fixup_original_page(&vnode,0x2766000,input,output)==6);
 assert(!memcmp(output,original,4096)&&!memcmp(input,patched,4096));
 input[0]^=1;cs_text_fixup_original_page(&vnode,0x2766000,input,output);
 assert(output[0]!=original[0]&&memcmp(output,original,4096));
 memcpy(input,patched,4096);input[0xfc]^=1;
 assert(cs_text_fixup_original_page(&vnode,0x2766000,input,output)==5);
 assert(memcmp(output,original,4096)&&output[0xfc]==input[0xfc]);
 vnode.v_name="other";assert(!cs_text_fixup_original_page(&vnode,0x2766000,input,output));
 assert(!memcmp(output,input,4096));
 vnode.v_name="dyld_shared_cache_arm64e.36";cs_text_fixup_count=1;cs_text_fixups[0].offset=UINT64_MAX-3;cs_text_fixups[0].length=32;
 assert(!cs_text_fixup_original_page(&vnode,0x2766000,input,output));
 cs_text_fixups[0].offset=0x2766ff0;
 assert(!cs_text_fixup_original_page(&vnode,0x2766000,input,output));
 assert(!cs_text_fixup_original_page(NULL,0x2766000,input,output));
 puts("PASS: six production Powerlog fixups reconstruct original page; unknown and malformed changes survive; executable input preserved; vnode and boundary checks pass");
}
'''
out=r/'host-tests/text-fixup-revalidation';out.mkdir(parents=True,exist_ok=True);(out/'test.c').write_text(head+fixture+helper+body)
subprocess.run(['xcrun','clang','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',str(out/'test.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
