# iOS 18 实体按键调研

日期：2026-11-06。设备：marble / SM7475。目标：iOS 18.7.11 / 22H384。
范围：源码与硬件描述调研。设备读写、镜像构建及按键驱动修改留待实现阶段。

## 硬件来源

来源：`../marble_hackios/marble_live.dts`。

| 按键 | 输入 | 状态与中断 | HID Consumer Usage |
| --- | --- | --- | --- |
| 电源 | PMK8350，SPMI SID 0，PON peripheral 0x13 | RT_STS 0x10 bit 7；IRQ 7 | Power 0x30 |
| 音量减 | PMK8350，SPMI SID 0，PON RESIN | RT_STS 0x10 bit 6；IRQ 6 | Volume Decrement 0xea |
| 音量加 | PM8350，SPMI SID 1，GPIO6，peripheral 0x8d | RT_STS bit 0，低电平按下；上拉；DTS 消抖 32 ms | Volume Increment 0xe9 |

PON 节点：9701–9730 行；GPIO6 配置：10190–10202 行；gpio-keys：32115–32130 行。
GPIO peripheral 计算：0x8800 + (6 - 1) * 0x100 = 0x8d00，PPID 0x18d。

SPMI 窗口：cnfg 0x0c42d000 / 0x4000，core 0x0c400000 / 0x3000，
chnls 0x0c500000 / 0x400000，obsrvr 0x0c440000 / 0x80000，intr 0x0c4c0000 / 0x10000。
EE 0，bus 0。SPMI periph_irq -> PDC pin 1 -> GIC SPI 481 -> INTID 513。
PDC 基址 0x0b220000；DTS 路由区间起点 pin 0 -> SPI 480。

Linux 主线源码交叉核对：

- `drivers/input/misc/pm8941-pwrkey.c`：pmk8350 使用 PON Gen3，状态位 7、6。
- `drivers/pinctrl/qcom/pinctrl-spmi-gpio.c`：GPIO 编号从 1 起始，peripheral 间距 0x100。
- `drivers/spmi/spmi-pmic-arb.c`：按版本选择寄存器布局，按 IRQ EE 与写入 EE 确认 APID 所有权。

## 上游参考

2026-11-06 执行 git fetch，读取远端 HEAD：

- UniXNU-sheng：`460179565a018c741b93e0b1efa07c55d4de497a`。
- ShengHidInput：`71a40e76daeae1bb4127546f96ffa7339c7a06ff`。

`UniXNU-sheng/package/ButtonProbe.cpp` 提供 SPMI APID 查找、双边沿中断配置、
PDC 配置、GIC 连接、线程采样和 `native_buttons_set_sink` 回调。
其目标 PMIC 为 PMK8550 / PM8550；marble 使用 PMK8350 / PM8350，需按板级运行值确认版本及 APID。

`ShengHidInput/NativeHidInput.cpp` 的 report 2 使用四个位：Menu bit 0、Power bit 1、
Volume Increment bit 2、Volume Decrement bit 3。硬件线程通过工作循环发送状态变更。

固件的 `marblePkg/Include/DXE.inc:76` 与 `APRIORI.inc:95` 加载 ButtonsDxe；
其 INF 指向 `ButtonsDxe.patched.efi` 二进制。实现阶段需核对固件定时器与 SPMI 中断的交接所有权。

## 当前接入缺口

- `package/MarbleHid18.cpp` 注册并启动触控设备。
- `package/NativeHomeInput.cpp` 含按键设备工厂，当前 report 2 描述 Menu；
  电源、音量 Usage、硬件 sink 和实际设备实例化待接入。
- `/buttons` 节点描述 hold、voldown、volup，作为设备身份输入。
- `offline-key-events=1` 消费方为 `KeyStoreEvents.cpp`，用于 AppleKeyStore 状态事件。
- `scripts/build_ios18_hid.py` 绑定 22H384 IOHIDDevice 的 207 个虚表槽与 PAC discriminator。
  Consumer 扩展可沿用该 ABI，HID 设备发布还需覆盖 Keyboard/Consumer 的驱动匹配。
- `scripts/build_ios18_arm64e.py` 的归档源列表需要加入板级按键输入实现。
- ADT 的 `sleep-disabled=1` 保持现有电源策略；系统休眠唤醒列为独立验收。

## 实现顺序与验收

1. 读取 SPMI 版本、PDC 版本、APID 映射和 EE 所有权，确认按键输入的空闲状态及按下状态。
2. 按板级输入适配中断驱动，保留按下与释放边沿；初始状态、短按、消抖和 sink 就绪前的状态交接纳入实现。
3. 实例化 Consumer HID 设备，发送 report 2 状态，接入 iOS 原版事件驱动。
4. 记录每键按下/释放次数、报告返回值、中断次数及错误；验证音量提示、长按重复、锁屏、亮屏、长按菜单及组合键。
5. 保存并恢复涉及的固件中断配置，验证驻留返回与 USB 连通性。

共享 SPMI 处理按键所拥有的 APID。上游清理其他 APID 中断的逻辑需按本机固件、充电与电源服务的所有权调整。
运行时待核对项：SPMI/PDC 版本、APID 数值、IRQ owner、固件按键定时器活动、iOS Consumer 匹配与锁屏后的面板行为。

GPU 线程继续拥有设备交互；本调研通过文件与源码读取完成。

## 实现与基础验收记录

2026-11-06：`MarbleButtons.cpp` 接入 PMIC 按键中断，`NativeHomeInput.cpp` 的 report 2 扩展 Power 与音量位，`MarbleHid18.cpp` 实例化按键设备并补充 Keyboard/Consumer 匹配。

真机读取 SPMI `0x70000000`、PDC `0x20700`、APID 数量 768；PON APID 407、GPIO6 APID 434，写入与中断所有权为 EE 0。空闲 PON RT_STS 为 `0x10`，GPIO6 为 `0x01`。

基础验收采样记录电源 15 次、音量加 9 次、音量减 9 次按下与释放；HID 报告 64 份，返回值与错误计数为 0。用户反馈：基本验收通过。细节见 `ios18_buttons_2026-11-06.json`。

固件 `ButtonsDxe.patched.efi` 发布 TextIn/TextInEx 接口，Keypad Device 协议查询返回 `EFI_NOT_FOUND`。二进制的 TextIn 表位于 RVA `0x8128`，TextInEx 表位于 `0x8140`，通知定时器事件位于 `0x84e8`，活动标志位于 `0x8324`，周期为 50 ms。固件交接实现按函数地址与定时器创建指令签名核对该镜像，内核持有按键期间暂停读取入口和活动定时器，驻留返回时恢复定时器。完整驻留返回与系统休眠唤醒列入待验收项。

收尾镜像通过 Fastboot 启动。真机回读固件交接返回成功，通知定时器处于未活动状态；物理按键边沿计数为 0 时，HID 事件驱动完成匹配并提交初始释放报告，报告错误为 0。启动匹配使用退避重试，匹配完成后停止计时器。
