# marble 系统音频插件

目标为 iOS 18.7.11 / 22H384。板级设备 `Speaker`、`Actuator` 通过字符端点与内核交换版本化 PCM 包，VirtualAudio 负责系统路由。Hawking、Codec 为 bring-up 依赖端点。

| 文件 | 职责 |
| --- | --- |
| `MarbleHapticsPlugIn.c` | AudioServerPlugIn 属性、启停、时间戳、PCM 和音量 |
| `MarbleHapticsProbe.m` | 诊断候选的系统播放、HAL 与 CoreHaptics 探针 |
| `MarbleAudioRouteSnapshots.inc` | 独立队列的只读 HAL 路由查询与循环记录 |
| `MarbleHapticServerDiagnostics.inc` | 诊断构建中的 AVHapticServer 调用观测 |
| `MarbleAudioRoutingTrace.inc`、`MarbleAudioRoutingImports.inc` | 旧导入追踪实现，boot-lock3 触发崩溃，弃用 |

构建入口为 `scripts/build_marble_haptics.py`；运行时清单固定二进制、签名和来源摘要。真机终态及已知故障见 [音频阶段记录](../records/audio-2026-11-11/README.md)。

当前实现属于实验阶段。扬声器锁屏后失声，末轮锁屏崩溃；系统触觉待接通。主机模拟端点的检查范围见 `scripts/tests/marble_haptics_hal.c`。
