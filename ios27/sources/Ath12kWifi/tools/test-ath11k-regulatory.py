#!/usr/bin/env python3
"""Exercise the prepared regulatory worker with the production dispatcher."""
from pathlib import Path
import subprocess
import sys
from source_fragments import function
root = Path(__file__).resolve().parents[1]
out = root / 'out/ath11k-regulatory-test'
out.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(root / 'tools/prepare-ath11k.py')], check=True)
base = (root / 'tests/runtime_test.c').read_text()
fragment = function((root / 'out/ath11k-src/reg.c').read_text(), 'ath11k_regd_update_chan_list_work')
fixture = r'''
#include "lx_pci.h"
#define ATH11K_SCAN_TIMEOUT_HZ 20000
#define ATH11K_11D_IDLE 0
#define ATH11K_11D_PREPARING 1
#define ATH11K_11D_RUNNING 2
#define ATH11K_SCAN_STARTING 1
#define ATH11K_SCAN_RUNNING 2
struct scan_chan_list_params { struct list_head list; unsigned sequence; };
struct ath11k_base { struct workqueue_struct *workqueue; };
struct ath11k {
 struct ath11k_base *ab;
 spinlock_t data_lock;
 struct delayed_work channel_update_work;
 unsigned long channel_update_11d_deadline, channel_update_scan_deadline;
 struct list_head channel_update_queue;
 unsigned state_11d;
 struct { unsigned state; } scan;
 struct completion completed_11d_scan;
};
static unsigned reg_sent, reg_probe_runs;
static int ath11k_wmi_send_scan_chan_list_cmd(struct ath11k *ar, struct scan_chan_list_params *params) {
 (void)ar; assert(params->sequence == ++reg_sent); return 0;
}
''' + fragment + r'''
static void reg_probe(struct work_struct *work) { (void)work; reg_probe_runs++; }
static void reg_append(struct ath11k *ar, unsigned sequence) {
 struct scan_chan_list_params *params = kzalloc(sizeof(*params), GFP_KERNEL);
 assert(params); params->sequence = sequence;
 list_add_tail(&params->list, &ar->channel_update_queue);
}
static void regulatory_defer_contract(void) {
 u64 saved_ticks = ticks;
 struct ath11k_base ab = {.workqueue=system_wq};
 struct ath11k ar = {.ab=&ab, .state_11d=ATH11K_11D_PREPARING};
 struct work_struct probe;
 spin_lock_init(&ar.data_lock); init_completion(&ar.completed_11d_scan);
 INIT_LIST_HEAD(&ar.channel_update_queue);
 INIT_DELAYED_WORK(&ar.channel_update_work, ath11k_regd_update_chan_list_work);
 INIT_WORK(&probe, reg_probe);
 reg_append(&ar, 1);
 queue_delayed_work(system_wq, &ar.channel_update_work, 0);
 queue_work(system_wq, &probe);
 u64 before = ticks; lx_pump();
 assert(ticks == before && reg_probe_runs == 1 && reg_sent == 0);
 assert(ar.channel_update_work.armed && !list_empty(&ar.channel_update_queue));
 reg_append(&ar, 2);
 ar.state_11d = ATH11K_11D_IDLE; ar.scan.state = ATH11K_SCAN_RUNNING;
 ticks += 20; before=ticks; lx_pump();
 assert(ticks == before && !reg_sent && ar.channel_update_scan_deadline);
 ar.scan.state = 0; ticks += 20; before=ticks; lx_pump();
 assert(ticks == before && reg_sent == 2 && list_empty(&ar.channel_update_queue));
 assert(!ar.channel_update_work.armed && !ar.channel_update_scan_deadline && !ar.channel_update_11d_deadline);
 ar.state_11d = ATH11K_11D_PREPARING; reg_append(&ar, 3);
 queue_delayed_work(system_wq, &ar.channel_update_work, 0); before=ticks; lx_pump();
 assert(ticks == before && reg_sent == 2);
 ticks += ATH11K_SCAN_TIMEOUT_HZ; before=ticks; lx_pump();
 assert(ticks == before && reg_sent == 3 && ar.state_11d == ATH11K_11D_IDLE);
 ar.scan.state = ATH11K_SCAN_RUNNING; reg_append(&ar, 4);
 queue_delayed_work(system_wq, &ar.channel_update_work, 0); before=ticks; lx_pump();
 assert(ticks == before && reg_sent == 3 && ar.channel_update_scan_deadline);
 ticks += ATH11K_SCAN_TIMEOUT_HZ; before=ticks; lx_pump();
 assert(ticks == before && reg_sent == 4 && ar.scan.state == ATH11K_SCAN_RUNNING);
 ar.scan.state = 0;
 ar.state_11d = ATH11K_11D_PREPARING; reg_append(&ar, 5);
 queue_delayed_work(system_wq, &ar.channel_update_work, 0); lx_pump();
 assert(cancel_delayed_work_sync(&ar.channel_update_work));
 struct scan_chan_list_params *params = list_first_entry(&ar.channel_update_queue, struct scan_chan_list_params, list);
 list_del(&params->list); kfree(params);
 ticks += 1000; lx_pump(); assert(reg_sent == 4);
 ticks = saved_ticks;
}
'''
source = base.replace('int main(', fixture+'\nint main(', 1)
source = source.replace('lx_plat = &platform; lx_root_init();', 'lx_plat = &platform; lx_root_init(); regulatory_defer_contract();', 1)
p = out / 'test.c'
p.write_text(source)
command = ['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
 '-I', str(root/'shim/include'), '-I', str(root/'include'), '-I', str(root/'out/pinned'), str(p),
 str(root/'shim/src/lx_rt.c'), str(root/'src/nw_rfkill.c'), str(root/'src/nw_device.c'), '-o', str(out/'test')]
subprocess.run(command, check=True)
subprocess.run([str(out/'test')], check=True)
print('ath11k regulatory worker: immediate host progress, FIFO, independent scan waits, timeout and cancellation passed')
