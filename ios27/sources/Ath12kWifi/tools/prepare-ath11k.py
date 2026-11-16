#!/usr/bin/env python3
"""Stage the locked ath11k sources for the marble WCN6855 transport."""
from pathlib import Path
import re
from source_fragments import function

root = Path(__file__).resolve().parents[1]
source = root / 'deps/linux/drivers/net/wireless/ath/ath11k'
output = root / 'out/ath11k-src'
output.mkdir(parents=True, exist_ok=True)
traces = set()
for path in sorted(source.glob('*.[ch]')):
    if path.name == 'trace.h':
        continue
    text = path.read_text()
    traces.update(re.findall(r'\b(trace_ath11k_[a-zA-Z0-9_]+)\s*\(', text))
    if path.name == 'pci.c':
        text = text.replace('static int ath11k_pci_init(', 'int ath11k_pci_init(')
        text = text.replace('static void ath11k_pci_exit(', 'void ath11k_pci_exit(')
    elif path.name == 'core.h':
        old = '\tstruct work_struct channel_update_work;'
        if text.count(old) != 1:
            raise SystemExit('ath11k regulatory work declaration differs')
        text = text.replace(old, '\tstruct delayed_work channel_update_work;\n\tunsigned long channel_update_11d_deadline, channel_update_scan_deadline;')
    elif path.name == 'core.c':
        text = text.replace('cancel_work_sync(&ar->channel_update_work)',
                            'cancel_delayed_work_sync(&ar->channel_update_work)')
    elif path.name == 'reg.c':
        text = text.replace('queue_work(ar->ab->workqueue, &ar->channel_update_work)',
                            'queue_delayed_work(ar->ab->workqueue, &ar->channel_update_work, 0)')
        old = function(text, 'ath11k_regd_update_chan_list_work')
        text = text.replace(old, (root / 'tools/patches/ath11k-regd-deferred.c').read_text())
    elif path.name == 'mac.c':
        # mac80211 calls flush while its wiphy mutex is held. The cooperative
        # executor defers ordinary work under mutexes; explicitly flush the
        # management submission worker before waiting for firmware completions.
        old = "\tif (drop)\n\t\treturn;\n\n\tath11k_mac_flush_tx_complete(ar);"
        new = "\tif (drop)\n\t\treturn;\n\n\tflush_work(&ar->wmi_mgmt_tx_work);\n\tath11k_mac_flush_tx_complete(ar);"
        if text.count(old) != 1:
            raise SystemExit('ath11k management flush source differs')
        text = text.replace(old, new)
        text = text.replace('cancel_work_sync(&ar->channel_update_work)',
                            'cancel_delayed_work_sync(&ar->channel_update_work)')
        text = text.replace('INIT_WORK(&ar->channel_update_work, ath11k_regd_update_chan_list_work)',
                            'INIT_DELAYED_WORK(&ar->channel_update_work, ath11k_regd_update_chan_list_work)')
    elif path.name == 'wmi.c':
        text = text.replace('wait_event_timeout(', 'wait_event_timeout_once(')
    destination = output / path.name
    if destination.is_symlink():
        destination.unlink()
    destination.write_text(text)
(output / 'trace.h').write_text('/* Kernel tracepoints use the native board log. */\n' +
    '\n'.join(f'#define {name}(...) 0' for name in sorted(traces)) + '\n')
spectral = root / 'out/spectral_common.h'
if spectral.is_symlink():
    spectral.unlink()
spectral.symlink_to(source.parent / 'spectral_common.h')
