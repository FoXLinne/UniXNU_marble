# 小米 MIX 2S（polaris）

polaris 是搭载 SDM845（骁龙 845、Adreno 630）的小米 MIX 2S。下面的移植是独立社区项目，UniXNU 组织不维护它。

| 仓库 | 维护者 | 状态 |
| --- | --- | --- |
| [UniXNU-polaris](https://github.com/luuu20309/UniXNU-polaris) | [luuu20309](https://github.com/luuu20309) | 平台移植阶段 |

## 状态

平台阶段**已在真机验证**（2026-10-24，经用户批准的临时引导）：Renegade
edk2-msm polaris 固件启动、UniXNU 从刷入 userdata 的 ESP 运行、DarwinBoot
以真板 1080×2160 GOP 和上 bank 内存窗完成交接，源码内核依次执行过
arm_init、调度器与 timer 中断、corecrypto FIPS 自检、bsd_init 与 IOKit，
到达设计内的无根边界（`Failed to mount root device`）。移植包含 polaris
平台 ADT（GICv3 0x17a00000/0x17a60000、PPI 19、19.2 MHz、PSCI SMC、
6/8 GiB 上 bank、4×A55+4×A75 前缀）、从 ADT 校验定时器 PPI 的 ArmCpu
override、上 bank 分配窗的 DarwinBoot override、fail-closed CPU 前缀检
查；主机/EFI/内核/QEMU 的 CI 全绿。移植过程中还修复了两个 UniXNU 通
用 bug：BCM2837 固定 panic-log 地址（0x3c0fc000）在任何非树莓派真机上
的总线错误（改走设备树探测），以及内核链接后修改 `__text` 导致的 FIPS
POST HMAC 失配（修改后自动重新封印）。本树尚无用户态、根文件系统、触
摸、显示或 GPU。

### 承自退役 ABL-shim 链的成果

同一维护者的早期项目
（[ios16_on_polaris_mix2s](https://github.com/luuu20309/ios16_on_polaris_mix2s)，基于
dipa-studio/ios-sweet 的 XNU 8796 / iOS 16 树）曾临时启动到 iOS 16 锁屏，
并经用户确认实现单指、双指触摸：Synaptics S3330 挂在 GPIO33/34 位拆
I²C 上，GPIO99 复位释放、有界连续 F12 帧读取加保守状态机；`cpus=4` 下
CPU1-3 的 PSCI CPU_ON/SGI 亦已验证。这些证据属于退役的树，不随内核基线
转移；该链测得的板级常数是本移植的起点。完整迭代史，包括每一阶段触摸诊断、
被撤回的 GPU command-db 映射和每项结果的证据分级，保存在设备仓库的
[HISTORY.md](https://github.com/luuu20309/UniXNU-polaris/blob/main/docs/HISTORY.md)。

## 构建

主机检查（Python 3.11+、clang、lld）：

```sh
python3 scripts/check-host.py
python3 scripts/make-polaris-adt.py --output out/polaris.adt
python3 scripts/check-cpu-prefix.py out/polaris.adt --cmdline "-v serial=3 cpus=1"
```

设备仓库集成：

```sh
python3 ../UniXNU/unixnu defconfig polaris_defconfig -D . --out out/driver
python3 ../UniXNU/unixnu status -D . --out out/driver
```

源码内核平台里程碑在 macOS + Xcode 上通过
`scripts/build-platform-kernel.py` 构建，或使用仓库的 *Darwin 23
platform kernel* workflow。完整 prelink、固件、payload 与 userdata 阶段
有意未接线：polaris UEFI 移植尚不存在，原厂组件 prelink 目标未定。
