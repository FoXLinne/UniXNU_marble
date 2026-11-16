from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2]
out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/wifi-scan'
out.mkdir(parents=True,exist_ok=True)
(out/'test.c').write_text('''#include "MarbleWifiScanPolicy.h"
#include <assert.h>
int main(void) {
 assert(marble_wifi_scan_target_cached(484090,484080,700));
 assert(!marble_wifi_scan_target_cached(484090,484080,1000));
 assert(!marble_wifi_scan_target_cached(484580,484080,700));
 assert(!marble_wifi_scan_target_cached(484090,0,0));
 assert(!marble_wifi_scan_target_cached(484000,484080,0));
 assert(!marble_wifi_scan_cached(10000,0,0,0,0,0,0));
 for(unsigned i=0;i<5000;i++) assert(marble_wifi_scan_cached(10000+i,0,10000,0,0,0,1));
 for(unsigned i=0;i<5000;i++) assert(!marble_wifi_scan_cached(10000+i,10000,10000,0,0,1,1));
 assert(!marble_wifi_scan_cached(15000,10000,10000,1,0,1,1));
 assert(marble_wifi_scan_cached(39999,10000,10000,1,0,0,1));
 assert(!marble_wifi_scan_cached(40000,10000,10000,1,0,0,1));
 assert(!marble_wifi_scan_cached(20000,10000,10000,0,0,0,1));
 assert(marble_wifi_scan_cached(90000,10000,10000,0,1,1,0));
 assert(!marble_wifi_scan_cached(90000,0,10000,1,0,0,1));
 assert(!marble_wifi_scan_cached(1000,2000,2000,0,0,0,1));
}
''')
subprocess.run(['xcrun','clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(repo/'package'),str(out/'test.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
print('PASS: wildcard burst bound, directed and channel-subset discovery reach the radio, connected/disconnected freshness, handshake protection and invalidated cache')
