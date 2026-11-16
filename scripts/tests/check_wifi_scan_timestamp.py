"""Exercise the 24B119 scan consumer's timestamp units and wire offset."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
source = (repo / 'package/MarbleWifiNet.cpp').read_text()
wire = source[source.index('struct scan_result {'):source.index('struct assoc_parameters {')]
fill = source[source.index('void fill_channel('):source.index('int copy_out(')]
program = r'''
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstddef>
struct channel { uint32_t version, channel, flags; };
struct nw_bss_info {
 uint32_t freq_mhz, age_ms; int signal_dbm; uint16_t beacon_int, caps;
 uint8_t bssid[6], ssid_len, ssid[32]; uint32_t ie_len; uint8_t ie[2048];
};
static uint64_t wifi_now_ms() { return 850935; }
'''+wire+fill+r'''
int main() {
 nw_bss_info b{}; b.freq_mhz=5745; b.signal_dbm=-61; b.age_ms=1218;
 b.ie_len=2048; memset(b.ie,0x5a,sizeof b.ie);
 scan_result result; fill_result(result,b);
 static_assert(sizeof(result)==0x8e8);
 uint64_t stamp=0; uint32_t age=0;
 memcpy(&stamp,reinterpret_cast<uint8_t*>(&result)+0x8d0,8);
 memcpy(&age,reinterpret_cast<uint8_t*>(&result)+0x84,4);
 assert(age==1218);
 assert(stamp==849717000000ULL);
 assert(850935-stamp/1000000==1218);
 assert(850935-849717/1000000>10000); // v28 millisecond field is rejected.
 assert(result.elements_length==1024);
 assert(result.elements[1023]==0x5a);
 b.age_ms=0;fill_result(result,b);assert(result.timestamp_ns==850935000000ULL);
 b.age_ms=900000;fill_result(result,b);assert(result.timestamp_ns==0);
}
'''
with tempfile.TemporaryDirectory() as folder:
    path=Path(folder)/'scan.cpp';path.write_text(program)
    binary=Path(folder)/'scan'
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: scan timestamp nanoseconds at 0x8d0; cache age at 0x84; freshness and bounds')
