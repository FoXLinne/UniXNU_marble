#!/usr/bin/env python3
"""Read wireless/battery evidence from the existing observer without UI actions."""
from pathlib import Path
import json,struct,sys,time
root=Path(sys.argv[1]);report=root/'wifi-observation-status.json'
state={'status':'waiting-for-wireless','mode':'passive; screen and network selection controlled by user','samples':[]};seen=set()
while True:
 for p in sorted(root.glob('wifi-power-[0-9]*.bin')):
  if p.name in seen:continue
  suffix=p.name.removeprefix('wifi-power-');battery=root/('battery-'+suffix);panel=root/('backlight-'+suffix)
  if not battery.exists() or not panel.exists():continue
  try:w=struct.unpack('<16Q',p.read_bytes());b=struct.unpack('<16i',battery.read_bytes());l=struct.unpack('<8I',panel.read_bytes())
  except struct.error:continue
  seen.add(p.name);state['samples'].append({'file':p.name,'time':p.stat().st_mtime,'percent':b[1],'current_ma':b[3],'temperature_c':b[4]/100,'panel_on':l[7],'steps':w[1],'busy_steps':w[2],'waits':w[3],'wait_us':w[4],'irqs':w[5],'empty_irqs':w[6],'ps_requested':w[7],'ps_applied':w[8],'ps_pending':w[9],'ps_error':w[10]-(1<<32) if w[10]>>31 else w[10],'ps_attempts':w[11],'station_state':w[12],'scans':w[13],'frames':w[14]})
  link=root/('wifi-link-'+suffix)
  if link.exists():
   try:
    values=struct.unpack('<10I',link.read_bytes())
    state['samples'][-1].update(dict(zip(['link_version','supported_phy_modes','active_phy_mode','width_mhz','spatial_streams','ath11k_ps','mac80211_ps','mhi_device_state','mhi_pm_state','link_updates'],values)))
   except struct.error:pass
 if state['samples']:
  state['status']='observing';state['latest']=state['samples'][-1]
 if (root/'channel-error.json').exists():state['status']='channel-ended'
 state['observed_at']=time.time();temporary=report.with_suffix('.tmp');temporary.write_text(json.dumps(state,indent=2)+'\n');temporary.replace(report)
 if state['status']=='channel-ended':break
 time.sleep(60)
