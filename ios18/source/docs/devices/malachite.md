# POCO X7 / Redmi Note 14 Pro 5G (malachite)

malachite is the Xiaomi POCO X7 and Redmi Note 14 Pro 5G with the MediaTek MT6878 SoC (Dimensity 7300-Ultra). The port below is an independent community project. The UniXNU organization does not maintain it. The kernel and root file system come from HackiOS; uefi-malachite, a port of uefi-sheng (Mu-Silicium) to MT6878, provides the UEFI firmware.

| Repository | Maintainer | State |
| --- | --- | --- |
| [hackios-xnu-kernel (malachite branch)](https://github.com/ghidgdsr/hackios-xnu-kernel/tree/malachite) | [ghidgdsr](https://github.com/ghidgdsr) | XNU rootfs loaded; the iOS userland does not start properly yet |
| [uefi-malachite](https://github.com/ghidgdsr/uefi-malachite) | [ghidgdsr](https://github.com/ghidgdsr) | UEFI works; UFS works |

## State

| Stage | State |
| --- | --- |
| UEFI firmware | works; boots straight into the XNU loader, with internal storage and the one key the device tree exposes (no boot menu) |
| UFS | works; the iOS 16.5 system image on `userdata` is the root (`md0`) |
| XNU | boots on core 0 (PSCI starts other cores at EL2) |
| XNU rootfs | loaded; launchd and lsd run |
| iOS userland | does not start properly; lsd and usermanagerd log errors frequently and the cause is under investigation; the loader's log monitor and saved-log slots are the bring-up aids |
| Display | loader text on the LK framebuffer only |
| USB | not implemented |
| Touch | not implemented |
| GPU | not implemented |
| Wi-Fi, Bluetooth, audio | not implemented |

UEFI works, UFS works, the XNU rootfs is loaded; USB, touch and the rest are not implemented.

## Build

The boot image is the firmware volume with the HackiOS XNUP payload appended (BootShim, the UEFI firmware volume and the payload), packed as an Android boot image v4 whose kernel is an `lz4 -l` stream. The loader and payload commands are in the kernel repository's `docs/malachite.md`. The firmware is built with:

```sh
./build_uefi.py -d malachite -r RELEASE
```

Flash it to `boot_a`, set slot a active and reboot:

```sh
fastboot flash boot_a Mu-malachite.img
fastboot --set-active=a
fastboot reboot
```

The kernel, the iOS system image and the payload are external inputs and are not stored in the repositories.

## Maintenance

The maintainer owns the repositories and their release decisions. UniXNU documentation lists the port and its state; it does not take over maintenance.
