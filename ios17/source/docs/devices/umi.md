# Xiaomi Mi 10 (umi)

umi is the Xiaomi Mi 10 with the SM8250 SoC. This is an independent community
port maintained by [reAAAq](https://github.com/reAAAq). The UniXNU organization
does not maintain it. The tested 12 GiB / 256 GB phone reaches iPadOS 17.7.11
Hello / Setup Assistant, with physical ST FTS touch under XNU confirmed working
by the device owner in attempt 80.

| Repository | Contents |
| --- | --- |
| [UniXNU-umi](https://github.com/reAAAq/UniXNU-umi) | Board, device drivers, XNU patches, diagnostics and host tools |
| [uefi-umi](https://github.com/reAAAq/uefi-umi) | Mu-Silicium UEFI port derived from uefi-sheng |

## State

The following state is reported by the maintainer through hardware attempt 80
on 2026-10-23. A [validation summary](https://github.com/reAAAq/UniXNU-umi/blob/main/docs/validation.json)
records selected measurements and image hashes; original device dumps remain local.

| Area | State |
| --- | --- |
| UEFI menu and DarwinBoot | Works with inherited framebuffer, physical menu buttons and a RAM FAT payload |
| XNU, GIC and timers | Runs on the phone; eight CPUs online with per-core timer and IPI activity recorded |
| Native UFS and root volumes | OS and SystemOS mounted; launchd and system services run |
| Writable Data | Persistent across reboots on the tested partition layout |
| Display and userland | Apple logo and Hello / Setup Assistant via CPU framebuffer presentation |
| Touch | Working under XNU; the device owner confirmed usable physical ST FTS input in attempt 80 |
| Setup completion / home screen | Not verified |
| GPU acceleration | Not implemented |
| Wi-Fi, Bluetooth, cellular, audio, camera and sensors | Not implemented or not qualified |
| XNU USB, battery management, suspend and resume | Not qualified; Recovery ADB is not XNU USB support |

## Board and system inputs

| Item | Value |
| --- | --- |
| Board / kit | `umi` / `umi` |
| SoC | `SM8250` |
| Donor | `iPad7,1`, iPadOS `17.7.11`, build `21H461`, `j120ap` |
| Donor chip / board ID | `0x8011` / `12` |
| Source XNU | `10063.141.1`, Darwin `23.6.0` |
| Original donor kernel | `10063.142.1.703.8`; distinct from the source version |
| Measured userdata | First LBA `0x2c0000`, last LBA `0x3b51ffa`, 4096-byte blocks |
| Firmware config / DSC | `umi` / `Platforms/Xiaomi/umiPkg/umi.dsc` |
| Firmware volume | `Build/umiPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd` |
| Android boot image | Header v1, 4096-byte pages |
| Tested root payload | 124 MiB FAT; `0x130000000` owned-memory window |
| CPU affinities / PSCI | `0,0x100,0x200,0x300,0x400,0x500,0x600,0x700` / `smc` |

The partition geometry and memory window are observations from one handset, not
installation defaults for every Mi 10. The installation replaces Android
userdata. The port keeps Recovery available for collecting retained RAM logs.

## Source and build

The device repository pins the tested framework and firmware in `source-lock.json`.
Its default `board.toml` and `umi_defconfig` select a minimal source-only kernel;
the full Hello profile is assembled by the device's bring-up tools. Do not treat
the baseline board values as the final root profile or substitute latest upstream
`main` for the recorded framework pin.

```sh
git clone https://github.com/reAAAq/UniXNU-umi
git clone https://github.com/reAAAq/uefi-umi
git clone https://github.com/UniXNU/UniXNU
git clone https://github.com/UniXNU/UniXNU-sheng
```

Check out each dependency at the commit in `source-lock.json`, then use the
[workspace setup and build instructions](https://github.com/reAAAq/UniXNU-umi/blob/main/docs/BUILD.md).
Full image reproduction still needs local ABI/provenance records, the measured
phone DTB and verified firmware imports, matching Apple components, and a packed
Data seed. These inputs are not distributed. This is a development source
snapshot, not a one-command installer or a downloadable boot-image release.

The touch implementation uses ST FTM5 over native GPI DMA I²C. Attempt 80
[enables the native always-on PM8150B GPIO5 rail](https://github.com/reAAAq/UniXNU-umi/commit/138d6cb)
before the FTS startup sequence, with identity/ownership guards and readback.
Physical touch was confirmed usable after this change. The sheng Novatek/SPI
firmware and matrix-processing algorithm are not used for this controller.

The attempt-80 coordinate/HID counters have not yet been collected; the current
touch status is based on the device owner's interaction report. Multi-touch,
suspend/resume, UEFI menu touch and completing setup remain unverified.

## Maintenance

The maintainer owns the repositories and release decisions. This page records
the community port's reported state; it does not transfer maintenance to UniXNU.
