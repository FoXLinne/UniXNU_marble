# 音频与触觉阶段记录：2026-11-11

## 终态

扬声器锁屏后失声，马达系统入口待接通。末轮 `boot-lock-state0-dsp` 收到用户报告：锁屏一次触发崩溃，候选验收失败。手机恢复 Android 17、槽位 A、启动完成；设备操作停止。

## 保存内容

| 文件 | 内容 |
| --- | --- |
| [stage.json](stage.json) | 最后候选、镜像与内核摘要、修改范围、硬件结果、安卓恢复 |
| [captures.json](captures.json) | 四轮原始采集摘要及路由状态转折点，包含原文件 SHA-256 |
| [analysis.json](analysis.json) | 离线分析过程及当时的候选验证状态 |
| [lock-state-evidence.json](lock-state-evidence.json) | 锁定缓存与会话观测，保留关联待验证状态 |

`analysis.json` 与 `lock-state-evidence.json` 保存形成时的记录，最终结果采用 `stage.json`。原始日志、线程寄存器、截图与二进制位于工作区 `outputs/UniXNU_marble/audio-20261110/`。

## 源码改动

- `package/MarbleAudio.cpp`：AW88263、AudioReach TDM、DSP 缓冲与字符设备端点；包含工作线程唤醒及启停代际处理。
- `package/MarbleHapticsRuntime.cpp` 与 `MarbleHapticsAbi.h`：触觉 PCM 传输与内核马达驱动入口。
- `hal/MarbleHapticsPlugIn.c`：Speaker、Actuator 和诊断 companion 设备，按流活动状态与重启时间线。
- `hal/MarbleAudioRouteSnapshots.inc`：独立串行队列每五秒保存路由状态，64 条循环快照。
- `package/QcomGlink.c`：保留客户端登记前到达的远端 OPEN，登记后匹配并确认，CLOSE 清理待处理项。
- 构建、打包、端点访问补丁和采集脚本随源码保存。

## 结果与边界

boot-lock2 故障现场中 StartIO/StopIO 为 7/7；boot-lock-offline 复现时为 3/3。路由保持 `pspk`，Speaker 输出流活动值为 1，设备处于停止状态。流通知补充后的故障继续存在。

系统音频服务读取 MobileKeyBag 的锁定状态。`sls=3` 候选缓存为 1，`sls=0` 对照缓存为 0。该变化与锁屏故障的因果关系待定位。末轮候选同时包含 GLINK 时序修复，锁屏崩溃结果作为失败样本保留。

GLINK 注册时序故障具有独立证据：`adsp_apps` 提前 OPEN 被消费，客户端随后登记却等待通道。修复后主机 4249 项通过，真机记录 DSP up / ready，系统 HAL 532 包、DSP 完成 566 缓冲。

原始 Actuator 设备存在且被 audiomxd 接管。虚拟 Actuator ID 为 0，CoreHaptics -4810、系统 HAL IO 计数为 0。马达系统振动保持待修复。
