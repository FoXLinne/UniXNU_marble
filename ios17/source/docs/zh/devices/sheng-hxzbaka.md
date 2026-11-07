# Xiaomi Pad 6S Pro \(hxzbaka\)

[hxzbaka/UniXNU-sheng](https://github.com/hxzbaka/UniXNU-sheng)

<div v-pre>

Xiaomi Pad 6S Pro 12.4 uses shared SM8550 support and board-owned storage, input, firmware and power integration. This record describes implementation coverage and the evidence required for each milestone.

源记录: [porting.toml](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/porting.toml), `0e1f6e52638137db15905db8d775ad73757f39fa`.

本页按设备记录自动生成。

| 部分 | 状态 |
| --- | --- |
| [配置](#configuration) | `implemented` |
| [固件与启动](#firmware) | `implemented` |
| [CPU 与中断](#cpu) | `implemented` |
| [内存与 DMA](#memory) | `implemented` |
| [存储与根卷](#storage) | `implemented` |
| [显示](#display) | `implemented` |
| [输入](#input) | `implemented` |
| [GPU](#gpu) | `implemented` |
| [网络](#network) | `implemented` |
| [音频](#audio) | `implemented` |
| [电源](#power) | `implemented` |
| [镜像组装](#packaging) | `implemented` |
| [诊断](#diagnostics) | `implemented` |

## 配置 {#configuration}

`implemented`

### 硬件与依赖

- SM8550 profile, iPadOS 17.7.11 build 21H461, device defconfig, pinned component sources.

### 配置

- board.soc\_source = sm8550; board.os\_profile = ipados-17.7.11-21H461; schema\_version = 2.
- The device owns ShengUfs, ShengHidInput, ShengNovatekTouch and ShengPreflight submodules.

### 操作步骤

- Read https://unixnu.com/porting/device-repository and https://unixnu.com/porting/records.
- Run ./unixnu defconfig sheng\_defconfig -D ../UniXNU-sheng from UniXNU.
- Run ./unixnu status --json -D ../UniXNU-sheng and review profiles, components, source pins and patch order.

### 开发经验

- Tables merge in ARM64, SoC, OS, device order. Arrays replace inherited selections, so include every required component and fragment.
- The selected defconfig applies after the explicit configuration fragments. Keep OS ABI and IPSW pins in the OS profile.

### 完成条件

- All source pins resolve; every selected component has a record; status shows the intended overrides and patch order.

### 当前范围

- Local overrides can select a different source from the published pin; record the effective source in each verification result.

### 下一步

- Compare the effective status output whenever a profile, source or component selection changes.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/board.toml)

## 固件与启动 {#firmware}

`implemented`

组件: `ShengPreflight`.

### 硬件与依赖

- uefi-sheng firmware, stock firmware DTB, GOP handoff, PSCI SMC interface and vendor firmware inventory.

### 配置

- firmware.dsc = Platforms/Xiaomi/shengPkg/sheng.dsc; firmware.config = sheng.
- proprietary-files.txt records input paths and provenance.
- The firmware DSC and FDF embed UniXNU, UniXNUDxe, DarwinBoot, UniXNURamDiskDxe, DiskIoDxe and FAT support.

### 操作步骤

- Run ./unixnu build firmware -D ../UniXNU-sheng --uefi ../uefi-sheng.
- Record the firmware commit, output FD and handoff memory map before a boot attempt.
- Build the payload, then run ./unixnu bootimg with the firmware DTB. Inspect boot.json ramdisk\_bytes and the required firmware modules.

### 开发经验

- The firmware DTB is an input to boot-image assembly. XNU consumes the separately generated ADT.
- The sheng preflight component belongs with board integration and its firmware requirements.
- The shared UniXNURamDiskDxe reads the Mu-Silicium entry-argument HOB and FDT initrd range to publish the payload block device. ShengBootMemoryDxe owns memory expansion.
- Configured menu entries require a payload FAT filesystem containing EFI/UniXNU/unixnu.toml. Check the UNIXNURAMDISK initrd block-device status when entries are absent.

### 完成条件

- Firmware image builds, enters UniXNU, publishes the framebuffer and preserves the documented memory reservations.

### 当前范围

- Boot-image assembly requires the selected firmware DTB and proprietary input files.

### 下一步

- Capture firmware revision and memory reservations with the next image verification.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/proprietary-files.txt)
- `source`: [Shared FDT initrd block-device publication; board memory policy remains in uefi-sheng.](https://github.com/UniXNU/UniXNU/blob/8d157ee18ae9c4d518aba28cb3040a84e860dcd1/package/UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.c)

## CPU 与中断 {#cpu}

`implemented`

组件: `ArmCpu`, `ArmDTPlatform`.

### 硬件与依赖

- SM8550 CPU affinities, GIC distributor at 0x17100000, redistributors at 0x17180000, 0x40000 stride and 19.2 MHz timer.
- PSCI uses SMC. The board disables cpu1 and cpu2 in its DTS.

### 配置

- dts/sheng.dts includes sm8550.dtsi; shared CPU and interrupt definitions come from the SoC source.
- Kernel arguments select sheng-smp=1 and armcpu-spi-ipi=1.

### 操作步骤

- Compile the board DTS and inspect enabled CPUs, GIC ranges and timer properties in the ADT.
- Record per-CPU entry, timer interrupts and inter-processor interrupt delivery.

### 开发经验

- CPU affinity values are hardware identifiers. Enabled CPU count and MPIDR affinities must be checked together.
- GIC register ranges must use translated physical addresses from the hardware tree.

### 完成条件

- Every enabled CPU reaches its entry checkpoint and timer/IPI handling works on each enabled CPU.

### 当前范围

- CPU selection and frequency policy are board inputs; SoC topology describes all cores.

### 下一步

- Attach per-CPU and interrupt observations for the selected firmware and image.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/dts/sheng.dts)

## 内存与 DMA {#memory}

`implemented`

### 硬件与依赖

- Handoff DRAM at 0x80000000 with size 0x80000000; splash reservation at 0xb8000000 with size 0x2b00000.
- Payload owned-memory request is 0x130000000. Device DMA requires controller stream IDs, address widths and ownership.

### 配置

- device\_tree.source = device:dts/sheng.dts; include\_dirs = \[soc:dts\].
- payload.adt.owned\_memory\_size and firmware reservations describe separate allocations.

### 操作步骤

- Compare firmware RAM ranges, reserved regions, DTS ranges and ADT memory-map entries.
- Check DMA buffers against controller address width, cache policy and SMMU mapping before submitting work.

### 开发经验

- Physical register translation and CPU virtual mappings have different purposes. Preserve physical resource ranges in hardware descriptions.
- GPU and UFS use different SMMU instances and stream assignments.

### 完成条件

- All handoff regions are accounted for and each enabled DMA engine accesses its allocated range with the documented cache policy.

### 当前范围

- The DTS declares the current handoff window and splash region; memory sizing requires the selected firmware map.

### 下一步

- Record the firmware memory map and each DMA consumer when changing reserved memory.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/dts/sheng.dts)

## 存储与根卷 {#storage}

`implemented`

组件: `ShengUfs`, `VirtioMmioBlock`, `RamBlock`, `VerifiedRoot`.

### 硬件与依赖

- UFS host 0x01d84000/0x3000; PHY 0x01d80000/0x2000; GCC base 0x00100000.
- GCC AXI, AHB, UniPro, symbol and PHY auxiliary branches; power domains and firmware link state.
- Apps SMMU at 0x15000000, UFS stream 0x60, context-bank ownership, DMA width and cache maintenance.
- LUN 0 uses 4096-byte blocks; userdata spans LBAs 3094592 through 64007162.

### 配置

- Select ShengUfs and CONFIG\_XNU\_NATIVE\_UFS=y; native-ufs-root=1 enables the root path.
- userdata defines OS and SystemOS windows. payload.adt.root\_match\_volume\_uuid = 04769D26-2CA0-4825-B443-E2E5F88300F2.

### 操作步骤

- Read https://unixnu.com/porting/ufs with this storage record.
- Inspect clock enables, power state, HCE/HCS, link state, request-list base and SMMU mappings before changing controller ownership.
- Validate SCSI capacity and GPT reads, then publish root windows and mount the configured root UUID.
- Verify persistent Data writes on the documented Data allocation with a recovery image available.

### 开发经验

- An idle AHB branch may report CLK\_OFF with hardware gating enabled. Check enable and gating bits together.
- SMMU bypass still has address-width constraints. Validate every DMA range and context-bank assignment.
- ShengUfs contains controller access, GPT/window parsing and sheng Data/log policy. Reuse each layer according to its inputs.

### 完成条件

- Host: register, clock, link and SMMU facts match the board inventory.
- Reads: SCSI capacity, GPT and known blocks are correct; OS/SystemOS windows stay within userdata.
- Root: the configured volume mounts and launchd starts. Persistence: writes survive reboot within the allocated Data region.

### 当前范围

- The SoC DTSI exports host and PHY ranges. GCC and UFS SMMU handling currently use constants and operations in ShengUfs.
- The root transport uses a polled request slot. Its firmware adoption path requires a usable existing request list.

### 下一步

- Record clock/PHY/reset initialization ownership and the SMMU stream before adapting this driver to another board.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/UniXNU/ShengUfs/blob/e2b32cc429e8cf1501161c1d8c517f8994ceb9b2/NativeUfs.cpp)

## 显示 {#display}

`implemented`

组件: `GopFramebuffer`.

### 硬件与依赖

- UEFI GOP framebuffer, stride, pixel format, splash reservation and board panel/backlight controls.

### 配置

- Select GopFramebuffer; CONFIG\_XNU\_NATIVE\_LEGACY\_DISPLAY=y; artwork-display-gamut = sRGB.
- sheng-vsync=120 requests board display timing.

### 操作步骤

- Verify GOP address, dimensions, stride and pixel layout before enabling presentation.
- Build the late display archive with the selected OS ABI records, then check display publication and frame presentation.

### 开发经验

- The display source and ABI binding are selected independently from the physical panel configuration.
- The board configuration declares sRGB for the current 8-bit framebuffer path.
- scripts/build-gop-legacy-display.py also builds the board driver objects in the display archive, including the ADSP, IPC router, FAT reader and battery manager.

### 完成条件

- A known frame appears with correct geometry and colors, then userland presentation and backlight controls work.

### 当前范围

- The late display builder needs qualified prelink candidates and ABI records. A timing request requires a measured presentation result.

### 下一步

- Record frame timing, pixel format and presentation evidence for each image used to qualify display.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/package/LegacyDisplayProbe.cpp)

## 输入 {#input}

`implemented`

组件: `ShengHidInput`, `ShengNovatekTouch`, `QcomGeniSpi`.

### 硬件与依赖

- Novatek NT36532 sensor, GENI SPI controller, pin configuration, reset/interrupt lines, firmware and touch coordinate geometry.

### 配置

- Select ShengHidInput, ShengNovatekTouch and QcomGeniSpi; sheng-touch=4 and sheng-touch-hid=1 select board integration.

### 操作步骤

- Read TouchBoard.h and verify the SPI clock and mode against the controller setup.
- Validate protocol reports, contact tracking and coordinate orientation before publishing HID events.

### 开发经验

- Controller transfer logic, sensor protocol and board sensor geometry are separate inputs.
- Track contact IDs across movement and lift events; a register read alone covers only transport access.

### 完成条件

- Touches map to the display, simultaneous contacts keep stable IDs, release events arrive, and buttons produce expected HID reports.

### 当前范围

- Sensor firmware and board dimensions remain sheng inputs.

### 下一步

- Attach contact and orientation observations when firmware or geometry changes.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/package/TouchBoard.h)

## GPU {#gpu}

`implemented`

组件: `Adreno740`, `AdrenoMetal`, `AirNir`, `Sm8550Platform`, `AdrenoPM4`, `AdrenoFirmware`, `ArmSmmu4K`.

### 硬件与依赖

- A740 GPU/GMU registers, GPU SMMU streams 0/1 and GMU stream 5, signed ZAP firmware and board power votes.
- Turnip, metal2vulkan, AIR translation and the selected OS GPU/Metal ABI.

### 配置

- Select Adreno740, AdrenoMetal and AirNir; shared SM8550 operations arrive through component dependencies.
- Board scripts combine shared GMU code with package/ShengGmuVotes.c.

### 操作步骤

- Qualify power, firmware, SMMU and GMU startup before command submission.
- Check memory mapping, command completion and a known rendered image before staging the Metal plugin.

### 开发经验

- GPU generation and the frontend ABI define component scope. Firmware, votes and display integration remain board inputs.
- Shared platform code and board votes require separate include paths during archive assembly.

### 完成条件

- GPU submissions complete, mapping/fence handling is correct, a known image renders, and the selected Metal client presents it.

### 当前范围

- A740 coverage depends on the selected firmware and OS ABI; board qualification requires its own memory and power evidence.

### 下一步

- Record the exact firmware, compiler inputs, driver revision and rendered output for the next qualification.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/scripts/build-adreno740-kits.py)

## 网络 {#network}

`implemented`

组件: `Ath12kWifi`.

### 硬件与依赖

- Ath12k device on PCIe, BAR windows, MSI routing, DMA callbacks, firmware, MHI and QRTR services.

### 配置

- Select Ath12kWifi. build-gop-legacy-display.py compiles its objects through build-ath12k-wifi.py.

### 操作步骤

- Verify PCIe enumeration and MSI delivery before MHI startup.
- Record firmware startup, scan, association, address configuration and bidirectional packet delivery separately.

### 开发经验

- The reusable Wi-Fi stack consumes board callbacks; PCIe and the OS network interface are owned by the device integration.
- MHI startup and network traffic are separate completion points.

### 完成条件

- Scanning, association, address configuration and sustained receive/transmit traffic pass with the selected firmware.

### 当前范围

- The source implements the station path through WifiNet.cpp and board PCIe integration. Device evidence must identify the firmware and access point.

### 下一步

- Attach association and receive/transmit observations for the current firmware and image.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/package/WifiNet.cpp)

## 音频 {#audio}

`implemented`

### 硬件与依赖

- ADSP firmware, SMEM, SMP2P, GLINK, GPR services, board codec and audio routing.

### 配置

- The board display archive includes ADSP and audio probe objects.

### 操作步骤

- Check firmware loading and ADSP service readiness before configuring audio endpoints.
- Record route, format and playback/capture observations for each enabled endpoint.

### 开发经验

- The ADSP image loads in place from the split adsp and adsp\_dtb files in /image of the modem\_a FAT volume \(modem\_b as fallback\); sheng-adsp-fw=DIR reads adsp.mbn and adsp\_dtb.mbn from DIR instead.
- The DSP reaches GLINK and opens the battery manager and IPCRTR channels. The IPC router answers the DSP's service locator requests.
- Firmware startup, service registration and audible playback require separate evidence.
- The GPR client and firmware loading helpers can be inspected independently from codec routing.

### 完成条件

- Each declared route carries the selected audio format through playback or capture and handles start/stop correctly.

### 当前范围

- Current source contains ADSP bring-up and audio framework probes. Endpoint playback qualification needs route-specific evidence.

### 下一步

- Document the codec, clocks and endpoint routing before claiming playback completion.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/package/AudioProbe.cpp)

## 电源 {#power}

`implemented`

组件: `AppleKeyStoreState`.

### 硬件与依赖

- Board PMGR integration, clocks/regulators and per-peripheral power requirements.
- ADSP battery manager: PMIC GLINK owner 32778 on PMIC\_RTR\_ADSP\_APPS; board properties use opcode 0x50; fuel gauges 0x8a/0x8b and 0xa6/0xa7.

### 配置

- CONFIG\_XNU\_SHENG\_PMGR=y; sheng-pmgr=2 selects the board integration.
- AppleKeyStoreState is selected with the OS event integration.
- sheng-adsp=2 sheng-battery=1 publish the battery as an IOPMPowerSource.
- sheng-usb-sdp-ma sets the input limit on a standard computer port \(default 900; 0 keeps the firmware limit\).

### 操作步骤

- Document each clock, regulator and firmware-owned state before enabling power transitions.
- Check battery units and update behavior separately from suspend/resume and peripheral power.
- Read the \[battery\], \[battery-raw\] and \[charger\] log lines with scripts/read-sheng-log.py.

### 开发经验

- The ADSP firmware runs charging and PD/PPS negotiation without host requests.
- Battery properties after 11 are shifted by one: charge current 12, temperature 13, charge counter 15, cycles 16, design 17, full 18, time to full 20, time to empty 21.
- Charge is the sum of both fuel gauges' remaining over full capacity. Battery current is reported discharge-positive.
- USB property 6 reads SDP on every port. Property 7 carries the adapter type \(6 PD, 8 PD PPS\).
- A PPS adapter gets a 3 A input limit at about 8.7 V; the battery charges at the 2000 mA charge current with the charge pump off.
- Both cells and the PD adapter read unauthenticated.
- A Mac USB-C port negotiates PD 5 V with a 1.2 A input limit, below the load during boot.
- Power policy belongs to the board even when controller operations are shared.

### 完成条件

- Declared power controls and battery readings match observations; each claimed sleep/resume transition restores its dependent peripherals.

### 当前范围

- Adapter details shown to iOS come from USB property 6, so every adapter reads as a computer port.
- Charge current stays at 2 A without fuel gauge and adapter authentication.
- A low-battery shutdown panics in vnode\_rele\_ext during unmount.
- Per-peripheral controls and battery publication have separate coverage from system suspend.

### 下一步

- Take adapter details from property 7.
- Authenticate both fuel gauges, then qualify PPS charge pump charging; Xiaomi adapter authentication follows.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/package/ShengBattery.cpp)
- `host`: [PASS battery manager: 4074 checks](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/scripts/check-battery-host.py)
  Command: <code>python3 scripts/check-battery-host.py</code>. Revision: `8719a19294bc79eed72366a19070acfd8e131737`.
- `device`: [PPS adapter: 8.72 V at 1.3 A in, battery +1.99 A, 1 to 3 percent in 3 minutes. Mac: PD 5 V, 1.2 A limit, battery -0.9 to -1.2 A. Temperature 31.4 to 32.9 C; full 9353 mAh, design 9800 mAh, 172 cycles.](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/scripts/read-sheng-log.py)
  Command: <code>fastboot boot boot.img; python3 scripts/read-sheng-log.py --wait 240</code>. Revision: `8719a19294bc79eed72366a19070acfd8e131737`.

## 镜像组装 {#packaging}

`implemented`

组件: `DarwinBoot`, `XnuPrelink`, `SoftwareAES`.

### 硬件与依赖

- Prelinked kernel, hardware DTB, generated ADT, trust cache, Data seed, platform identity and firmware DTB.

### 配置

- Payload volume is 208 MiB. kernel.patch\_series selects ARM64, SoC, OS and device series in order.

### 操作步骤

- Build kernel and package, then payload with --unique-chip-id and the selected trust-cache/Data inputs.
- Run ./unixnu bootimg -D ../UniXNU-sheng --uefi ../uefi-sheng --dtb &lt;firmware.dtb&gt; and inspect the assembly report.

### 开发经验

- The OS boot-progress integration follows the SoC hooks in patch order.
- The payload carries kernel.macho.adt; the boot image carries the firmware FDT. Keep the two inputs named explicitly.

### 完成条件

- Kernel and prelink succeed, payload contains all declared inputs, and the boot image matches the bootloader format and size limit.

### 当前范围

- Image output depends on the selected ABI audits, firmware and volume policy inputs.

### 下一步

- Attach artifact paths and exact source revisions to the next image build record.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/patches/xnu/series)

## 诊断 {#diagnostics}

`implemented`

### 硬件与依赖

- Serial or framebuffer diagnostics, panic region, boot checkpoints and a recovery path.

### 配置

- boot-progress=2 enables checkpoints; source and image revisions accompany every log.

### 操作步骤

- Run the existing check for the changed area and keep its command, output and input revision.
- Record the last completed checkpoint and the first failing operation; link the captured output.

### 开发经验

- A source implementation, a host check and a device observation establish different completion levels.
- Keep an unresolved cause explicit and record the next measurement that can distinguish it.
- The UFS log carries \[adsp\], \[qrtr\], \[battery\], \[battery-raw\] and \[charger\] lines for the DSP, the IPC router and the charger.
- Host checks for the ADSP path: check-adsp-host.py, check-modem-fw-host.py, check-qrtr-host.py and check-battery-host.py.

### 完成条件

- Each claimed milestone has a reproducible command, a matching source/image revision and an accessible result.

### 当前范围

- Source references in this record describe implemented behavior. Host and device status require their respective evidence entries.

### 下一步

- Add captured host or device evidence when repeating a checkpoint on a known image.

### 证据

- `source`: [Implementation and configuration described in this section](https://github.com/hxzbaka/UniXNU-sheng/blob/0e1f6e52638137db15905db8d775ad73757f39fa/scripts/read-sheng-log.py)

</div>
