from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2];out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/wifi-capability-wire';out.mkdir(parents=True,exist_ok=True)
s=(repo/'package/MarbleWifiNet.cpp').read_text();case=s[s.index('  case kCardCaps: {'):s.index('  case kPhyMode:',s.index('  case kCardCaps: {'))]
head='''#include <cstdint>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <cstdio>
struct apple80211req{uint32_t req_len;uint8_t *req_data;};
struct Out{uint8_t raw[5120];};
static uint32_t nw_wifi_card_capabilities(){return (1u<<3)|(1u<<6)|(1u<<14)|(1u<<17)|(1u<<19);}
static int answer_bytes(apple80211req &rq,const void *p,uint32_t n){memcpy(rq.req_data,p,n);rq.req_len=n;return 0;}
constexpr int kCardCaps=12;
int card(apple80211req &rq){Out out{};switch(kCardCaps){
'''
test='''}}
static bool framework_has(const uint8_t *p,unsigned bit){return (p[bit/8]>>(bit&7))&1;}
int main(){
 uint8_t bytes[64];memset(bytes,0xa5,sizeof bytes);apple80211req rq{23,bytes};
 assert(!card(rq)&&rq.req_len==23);
 for(unsigned bit=0;bit<=180;bit++) assert(framework_has(bytes,bit)==(bit<32&&((nw_wifi_card_capabilities()>>bit)&1)));
 assert(framework_has(bytes,14));assert(!framework_has(bytes,0));
 for(unsigned i=4;i<23;i++)assert(bytes[i]==0);assert(bytes[23]==0xa5);
 uint8_t shifted[23]{};uint32_t version=1,caps=nw_wifi_card_capabilities();memcpy(shifted,&version,4);memcpy(shifted+4,&caps,4);
 assert(!framework_has(shifted,14)&&framework_has(shifted,0));
 rq.req_len=8;assert(!card(rq)&&rq.req_len==8);rq.req_len=3;assert(!card(rq)&&rq.req_len==3);rq.req_len=0;assert(card(rq)==EINVAL);rq.req_len=6000;assert(card(rq)==EINVAL);
 puts("PASS: 24B119 bare 23-byte bitmap consumer sees WPA2 bit14, shifted versioned payload reproduces missing WPA2; bounded output");
}
'''
p=out/'wire.cpp';p.write_text(head+case+test);subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',str(p),'-o',str(out/'wire')],check=True);subprocess.run([str(out/'wire')],check=True)
