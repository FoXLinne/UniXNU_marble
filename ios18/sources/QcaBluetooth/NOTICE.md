# Source attribution

The locked `btqca.c` and `btqca.h` originate from
[ianchb/sm8550-mainline](https://github.com/ianchb/sm8550-mainline), commit
`1fb56e66091e71160d7c7f8e13b065d5b6ab8b8c`. The generated copies retain their
original notices and GPL-2.0-only license.

GENI register and DMA sequences follow `qcom_geni_serial.c`, `qcom-geni-se.c`
and `geni-se.h` at that same revision. Their identities are recorded in
`tools/source-lock.json`.

`src/qbt_transport.c` derives from
[Renoir XNU Bluetooth](https://github.com/daimile2/renoir_xnu_bluethooth/tree/2b9d59c921c9ec803462b2be8deb6980f7297c22),
`kernel/files/bsd/dev/renoir_bluetooth_transport.inc`, by daimile2 and
contributors. Its APSL-2.0 notice accompanies the file. The WCN7850 adapter
adds channel lifetime coordination, power completion requests, range checks,
queue copies and nonfatal packet allocation handling.

The sheng board references are
[sm8550-mainline](https://github.com/ianchb/sm8550-mainline) and
[debian-sheng](https://github.com/ianchb/debian-sheng).
