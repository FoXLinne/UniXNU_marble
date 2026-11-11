# marble AudioReach 与 AW88263

板级入口为 `../MarbleAudio.cpp`。本目录保存 GPR、AudioReach 协议与功放配置输入。

| 文件 | 用途 |
| --- | --- |
| `ShengGpr.c`、`ShengGpr.h` | GPR 包头、编码、解码与服务查询 |
| `ShengAudioReach.c`、`ShengAudioReach.h` | APM graph 生命周期、缓冲映射和 TDM 配置 |
| `MarbleSpeakerAbi.h` | `/dev/marble-speaker` 的版本化 PCM 包约束 |
| `MarbleAmplifierProfile.inc` | AW88263 原机 Music profile 寄存器配置 |

功放与线路输入来自 marble DTS、AW88263 驱动和原机 ACF。摘要与寄存器来源见 `../../profiles/audio/aw88263-music.json`；本地参考输入位于工作区 `outputs/UniXNU_marble/audio-20261110/reference/`，来源目录由同级 `references.json` 记录。

物理功放 I2C 地址为 0x34、0x35，芯片 ID 为 0x2032。DSP 端点采用 TDM 96 kHz、双声道、PCM32；系统 HAL 48 kHz 输入经板级插件转换。具体时钟、slot 和引脚配置随当前源码保留。

主机 graph 检查入口为 `../../scripts/tests/marble_audio_graph.c`，系统播放结果见 [音频阶段记录](../../records/audio-2026-11-11/README.md)。扬声器锁屏恢复与系统触觉保持待修复状态。
