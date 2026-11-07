# 一加 10 Pro 5G（negroni）

一加 10 Pro 5G（negroni、SM8450）的社区移植。pawlickerr 的记录使用 NE2213，独立 iOSPlus10 工作区使用 NE2211。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-negroni](https://github.com/pawlickerr/UniXNU-negroni) | [pawlickerr](https://github.com/pawlickerr) |
| [Mu-Silicum-negroni](https://github.com/pawlickerr/Mu-Silicum-negroni) | [pawlickerr](https://github.com/pawlickerr) |
| [iOSPlus10](https://github.com/akshaynexus/iOSPlus10) | [akshaynexus](https://github.com/akshaynexus) |

## 当前状态

pawlickerr 移植记录了 iOS 16.7.16 SpringBoard、触控和 CPU 渲染。

独立的 iOSPlus10 工作区记录了 `backboardd` 的 Metal 合成：一次 150 秒实机运行未崩溃，命令缓冲耗时为 4.5 到 8 ms，GPU 呈现速率最高为 83 次/秒。这些测量适用于该工作区选择的配置。QCA6490 PCIe 链路和设备响应已验证，Wi-Fi 联网、音频播放和充电改动仍需实机结果。

## 维护

维护者拥有仓库和发布决定。UniXNU 文档记录移植及其状态，不接管维护。

## 来源

- [移植 README: pawlickerr/UniXNU-negroni](https://github.com/pawlickerr/UniXNU-negroni/blob/f812002ae3e6b861d23007efdfd9f1c394934ba9/README.md)
- [工作区状态: akshaynexus/iOSPlus10](https://github.com/akshaynexus/iOSPlus10/blob/c0a451fbe774b44a47ce91795d1f8b5b1c4ac351/docs/plan.md)
