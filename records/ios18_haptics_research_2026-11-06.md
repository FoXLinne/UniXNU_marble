# Marble 震动马达适配调研

日期：2026-11-06（Asia/Shanghai）。设备：marble / SM7475。目标：iOS 18.7.11 / 22H384，iPhone11,8 / N841。

调研结论：采用 PM8350B / HAP520 的 SPMI 控制路径，按“硬件探测、定时震动、板级波形、iOS Actuator、FIFO 流”推进。硬件协议依据小米 marble 原厂源码；Pagani 提供 SPMI 分层与 Actuator 接入参考，HakiOS 提供系统触觉到马达效果的映射参考。

本次证据覆盖远端版本查询、源码阅读、系统资源检查与参考实现主机验证。马达驱动、镜像构建与设备播放验收属于后续实现阶段。

## 文档与上游版本

阅读 UniXNU 中文移植文档的[欢迎](https://unixnu.com/zh/porting/)、[平台](https://unixnu.com/zh/porting/platform)、[驱动](https://unixnu.com/zh/porting/drivers)、[输入](https://unixnu.com/zh/porting/input)、[DSP 与电源](https://unixnu.com/zh/porting/power)、[记录](https://unixnu.com/zh/porting/records)正文。

2026-11-06 通过 GitHub API 和 git ls-remote 查询以下版本。版本表记录调研快照；构建集成基线由 upstream_revisions.json 管理。

| 来源 | 调研提交 | 用途与观察 |
| --- | --- | --- |
| [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU/commit/7307c26ea5053a9b7a3e58b81a12b6f1d5a2b226) | 7307c26ea5053a9b7a3e58b81a12b6f1d5a2b226 | HEAD 提交时间为北京时间 2026-11-06 01:14:43。相对本地构建树 c740efb，新增四个文档提交。完整树与 components.toml 检索结果：haptics 命中脚本 check-original-backboard-haptic-wait.py；震动实现参考来自设备仓库。 |
| [FoXLinne/UniXNU_marble](https://github.com/FoXLinne/UniXNU_marble/commit/0b6b6106d49a28c1f8c55972fc433cc012e20def) | 0b6b6106d49a28c1f8c55972fc433cc012e20def | 远端 HEAD。工作区设备代码基于本地 fa7e067，包含实体按键 SPMI 实现。 |
| [UniXNU/UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng/commit/460179565a018c741b93e0b1efa07c55d4de497a) | 460179565a018c741b93e0b1efa07c55d4de497a | 平台、按键与音频服务参考。 |
| [IssacL04/UniXNU-Pagani-Drivers](https://github.com/IssacL04/UniXNU-Pagani-Drivers/commit/c888efd3ce1b817d13f28783a77cf689cb2255c9) | c888efd3ce1b817d13f28783a77cf689cb2255c9 | PaganiHaptics、PaganiAudioHaptics 与 iOS 能力节点参考。目标为 SID 7 / PMIH010x / HAP530，README 的设备观察停在识别和初始化阶段，马达播放验收列入该项目后续工作。 |
| [oosexgang/hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel/commit/2631f5d7d4f9d7322aca60991b34ba465dec343b) | 2631f5d7d4f9d7322aca60991b34ba465dec343b | drivers/haptic/hakios.haptic.c、userland/src/audio_plugin.c。AW8697 硬件，Actuator 样本转换为短效果和持续震动。 |
| [MiCode/Xiaomi_Kernel_OpenSource，marble-s-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/48952ed36228217531482b39d5bef13e7fd808ec/drivers/input/misc/qcom-hv-haptics.c) | 48952ed36228217531482b39d5bef13e7fd808ec | marble 原厂分支 HEAD；提交日期 2023-06-02。HAP520 寄存器、校准、升压和播放顺序的板型依据。 |
| [LineageOS/android_kernel_xiaomi_sm8450](https://github.com/LineageOS/android_kernel_xiaomi_sm8450/blob/eff4b40407a1b6f1b1ab84554cbb540bcb495175/drivers/input/misc/qcom-hv-haptics.c) | eff4b40407a1b6f1b1ab84554cbb540bcb495175 | HEAD，默认分支 lineage-24.0；交叉核对 HAP520 与 SPMI arbiter。 |
| [torvalds/linux](https://github.com/torvalds/linux/tree/05441d697f7f9a927264f4058cbbc6a03dfcac9d/drivers/input/misc) | 05441d697f7f9a927264f4058cbbc6a03dfcac9d | HEAD 的 input/misc 目录列出 arizona、max77693、max8997、regulator 触觉驱动；本次采用的 qcom-hv-haptics.c 位于厂商内核树。 |

## 硬件事实

实机描述来源：../marble_hackios/marble_live.dts:10464、10522。文件 SHA-256：2a7c6a3b3e1ce9cc29167429120de21c14cbe6b622f576328f16d2286a26b210。

| 项目 | 实机 DTS 输入 |
| --- | --- |
| PMIC 与总线 | qcom,pm8350b@3；SPMI bus 0、SID 3、EE 0 |
| 触觉模块 | qcom,hv-haptics；CFG 0xf000、PATTERN 0xf100、HBOOST 0xf200；PPID 0x3f0、0x3f1、0x3f2 |
| 中断 | SID 3、peripheral 0xf0、IRQ 1、rising edge，名称 fifo-empty |
| LRA 周期 | 0x16fa = 5882 us，约 170 Hz；f0 范围 160–180 Hz；f0-cal-count 592 |
| 驱动与刹车 | sine / sine；brake-mode 1；brake-pattern ff 3f 1f |
| 电压 | 默认 qcom,vmax-mv = 1450 mV；effect_0 至 effect_5 的 wf-vmax-mv = 3600 mV |
| 效果 | 六组 effect；每组八个幅度/周期/频率调整三元组，wf-pattern-period-us = 6667；effect_0 带 preload |
| 校准存储 | PMK8350 SID 0，SDAM 0x9d00，hap_cfg_sdam；cl_brake 位于 SID 0 / SDAM 0x7100 + 0x7c |
| PBS | qcom,pbs-client 指向 PM8350B SID 3 / qcom,pbs@1900 |

SPMI MMIO：cnfg 0x0c42d000/0x4000，core 0x0c400000/0x3000，chnls 0x0c500000/0x400000，observer 0x0c440000/0x80000，intr 0x0c4c0000/0x10000。

实体按键验收记录 ios18_buttons_2026-11-06.json 给出 arbiter 0x70000000、768 APID、PDC 0x20700。触觉外设的 APID、写入 owner、IRQ owner、revision、TYPE/SUBTYPE 需要由触觉探测读取。PPID 与 APID 的映射由硬件表解析。

## HAP520 实现要点

小米 qcom-hv-haptics.c 的 enum haptics_hw_type 标注 HAP520 = 2 / PM8350B；haptics_get_revision() 使用 CFG revision 2 与 PATTERN revision 2 选择 HAP520。探测读取模块身份与升压 revision，记录固件交接状态。

| 控制环节 | 原厂源码依据 | Marble 实现要求 |
| --- | --- | --- |
| FIFO 容量 | get_max_fifo_samples() | 640 样本；empty threshold 280，步长 40。Pagani 的 HAP530 配置为 8192 样本，Marble 需提供 HAP520 参数与状态位布局。 |
| FIFO 写入 | haptics_update_fifo_samples() | 四字节写入；尾部填零到四字节。源码记录单字节写入与硬件读取冲突的 HAP520 缺陷。 |
| 升压控制 | haptics_boost_vreg_enable() | SID 0 的 hap_cfg_sdam +0x42 写入 1/2 表示开/关，+0xe5 写入 1 触发 PBS。写入范围需覆盖该 SDAM 的目标偏移并核对 owner。 |
| 升压所有权 | is_boost_vreg_enabled_in_open_loop()、haptics_wait_hboost_ready() | 检查充电固件使用升压器的状态、5 V clamp、VREG_EN 与 STATUS4；使用有界等待和资源状态回读。 |
| 校准 | haptics_get_closeloop_lra_period() | 启动读取 SDAM +0x46 两字节，按 RC 校准模式和 SLEEP_CLK_32K_SCALE 换算周期；保留 160–180 Hz 板级检查。 |
| 硬件规避项 | haptics_config_wa() | TOGGLE_CAL_RC_CLK、SW_CTRL_HBST、SLEEP_CLK_32K_SCALE、TOGGLE_EN_TO_FLUSH_FIFO、RECOVER_SWR_SLAVE。 |
| 初始化顺序 | haptics_hw_init() | 保存刹车校准，配置 VMAX、波形、供电、LRA 周期、预载效果与 FIFO。 |
| 播放与停止 | haptics_enable_play()、haptics_load_constant_effect() | 核对 FAULT_CLR、供电配置、SPMI_PLAY、升压保持和停止延时。原厂 constant 路径实际调用 haptics_enable_autores(chip, false)，函数上方注释写 enable；以执行路径与设备采样确定自动谐振策略。 |

Pagani 可参考部分：SPMI observer/channel 分离、APID/owner 检查、有界事务、队列状态机、错误停止和主机模型。HAP520 的升压写入、FIFO 几何、校准换算及规避项按上表实现。

小米驱动采用 GPL-2.0-only；Pagani 采用 BSD-2-Clause-Patent。后续实现需记录代码来源及对应许可证。

## iOS 18 触觉入口

参考通路：CoreHaptics / UIKit / 系统震动 → 系统音频与触觉服务 → VirtualAudio 的 Actuator → HAL 插件 → 内核播放队列 → HAP520。

目标 22H384 系统镜像检查结果：

- /Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/VirtualAudio 包含 Actuator、VirtualAudioDevice_Actuator、ActuatorSettingsFactory_Aspen、actuator_general 字符串。
- /Library/Audio/Tunings/N841/Haptics/hapticengineconfig.plist：23588 字节，SHA-256 acc7e2703a024ef17e1ed571b0215c149daf9f5a507e7210b9f8ed7a896a3aea；plist 解析成功，包含 Haptic 事件与频率/包络参数。
- /Library/Audio/Tunings/N841/VAD/n841_actuator_general.austrip：838 字节，SHA-256 c861634c9b5eff01a7821c3d3d37d1da6973ee4f37c6620595237ce3effa1601。
- Haptics 目录包含 FullTap、MiniTap、MicroTap 等波形资源。

Pagani 的 HapticsIos.h 记录 21H461 上的 /product/haptics、supports-3rd-party-haptics 和 closed-loop 能力输入；PaganiAudioHaptics.c 提供 48 kHz int16 Actuator 环形缓冲到低采样率 int8 FIFO 的桥接。HakiOS audio_plugin.c 则将 Actuator 样本写入字符设备，由内核按峰值、持续时间与静音状态调度硬件效果。

22H384 待核对输入：MobileGestalt 能力读取位置、N841 产品设置、HAL 接口与时间基准、插件签名/信任、服务路由与实际样本格式。能力节点发布安排在马达驱动就绪、Actuator 可见及目标调音资源可访问之后。Pagani 的 J120 私有地址与 VirtualAudio 修补属于 21H461 / J120 的具体输入，22H384 / N841 使用本版本符号、指令与运行日志核对。

## 实现顺序与验收

1. **硬件探测**：增加 MarbleHaptics 探测入口，复用 MarbleButtons 的 MMIO 与 APID 查询经验，读取 CFG/PTN/HBOOST、SDAM、owner 和故障状态。输出版本、PPID/APID、寄存器快照与错误。该阶段以寄存器读取为工作范围。
2. **定时震动**：实现 HAP520 初始化、SDAM/PBS 升压保持、DIRECT_PLAY、定时停止、故障停止与驻留返回清理。以 1450 mV 板级配置开始，执行 50–150 ms 播放，采集用户触感反馈与播放/停止/升压状态。
3. **板级效果**：加载 DTS 六组效果，验证时长、强度、连续调用、故障恢复、按键与 USB 并发。写入序列与寄存器状态保存在阶段记录。
4. **系统入口**：实现 Actuator HAL 与内核队列，核对 22H384 的能力输入、插件加载、路由和时间戳；用系统提示震动、UIKit feedback、CoreHaptics transient/continuous 与桌面长按验收。计数包含请求数、样本数、硬件开始/停止数和丢弃原因。
5. **FIFO 流**：采用 640 样本缓冲、四字节补齐和原厂规避项，实现有界流与补充数据。FIFO IRQ 与按键共用 INTID 513；接入前扩展 SPMI 中断分发，使按键线程识别触觉 APID 并完成分工。短震动阶段可使用定时停止，连续流按 FIFO 消耗安排补充数据。
6. **生命周期**：验证静音、客户端退出、服务重启、FIFO 耗尽、所有权变化、故障、驻留返回和重复启动。记录停止回读与升压归属，保存镜像及源码版本。

下一实现入口：package/MarbleHaptics.cpp、板级参数文件，以及 scripts/build_ios18_arm64e.py 的归档/初始化链。硬件短震动验收通过后推进 Actuator 服务。硬件播放采用 SPMI；ADSP 音频图与 SoundWire 可作为后续扩展方向。

## 本次验证与证据

参考实现验证命令：python3 sources/pagani/drivers/PaganiHaptics/Test/check-pagani-haptics-host.py。

结果：strict compile 通过；SPMI 14613 项、core 94939 项、iOS 节点与提交逻辑 191 项、8 项启动参数检查通过。该命令运行 Pagani 原版 HAP530 模型；Marble HAP520 硬件验收依据上述阶段的设备日志与触感反馈。

证据目录：../../../outputs/UniXNU_marble/haptics_research_2026-11-06。包含 manifest.json、远端 commit/tree 快照、UniXNU 比较结果、小米与参考实现源码快照、pagani-host-checks.log、主线目录查询结果。

提交前执行 python3 scripts/snapshot_upstream.py，快照导出结果与现有受跟踪基线一致。调研提交保存本记录。
