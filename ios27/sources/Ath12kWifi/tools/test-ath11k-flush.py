#!/usr/bin/env python3
"""Exercise ath11k's generated flush against the cooperative work dispatcher."""
from pathlib import Path
import subprocess
import sys
from source_fragments import function
root=Path(__file__).resolve().parents[1]
out=root/'out/ath11k-flush-test';out.mkdir(parents=True,exist_ok=True)
subprocess.run([sys.executable,str(root/'tools/prepare-ath11k.py')],check=True)
base=(root/'tests/runtime_test.c').read_text()
fragment=function((root/'out/ath11k-src/mac.c').read_text(),'ath11k_mac_op_flush')
fixture='''
struct ath11k { struct work_struct wmi_mgmt_tx_work; struct mutex conf_mutex; unsigned pending; };
struct ieee80211_hw { void *priv; };
struct ieee80211_vif { int unused; };
static int ath11k_mac_flush_tx_complete(struct ath11k *ar) { assert(ar->pending == 0); return 0; }
'''+fragment+'''
static void management_submit(struct work_struct *work) {
 struct ath11k *ar=container_of(work,struct ath11k,wmi_mgmt_tx_work);
 mutex_lock(&ar->conf_mutex); assert(ar->pending == 1); ar->pending--; mutex_unlock(&ar->conf_mutex);
}
static void management_flush_contract(void) {
 struct ath11k ar={0}; struct ieee80211_hw hw={.priv=&ar}; struct mutex wiphy;
 mutex_init(&wiphy); mutex_init(&ar.conf_mutex); INIT_WORK(&ar.wmi_mgmt_tx_work,management_submit);
 mutex_lock(&wiphy); ar.pending=1; queue_work(system_wq,&ar.wmi_mgmt_tx_work);
 lx_pump(); assert(ar.pending == 1);
 ath11k_mac_op_flush(&hw,NULL,0,false); assert(ar.pending == 0);
 mutex_unlock(&wiphy);
}
'''
assert 'int main(' in base
source=base.replace('int main(',fixture+'\nint main(',1)
source=source.replace('precise_timers();','management_flush_contract(); precise_timers();',1)
p=out/'test.c';p.write_text(source)
command=['clang','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(root/'shim/include'),'-I',str(root/'include'),'-I',str(root/'out/pinned'),str(p),str(root/'shim/src/lx_rt.c'),str(root/'src/nw_rfkill.c'),str(root/'src/nw_device.c'),'-o',str(out/'test')]
subprocess.run(command,check=True);subprocess.run([str(out/'test')],check=True)
print('ath11k management flush progresses while mac80211 holds wiphy mutex')
