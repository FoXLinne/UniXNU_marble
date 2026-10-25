# UniXNU_marble

POCO F5／Redmi Note 12 Turbo（marble，SM7475）的 Goodix GT9916R 触控源码。
目标环境为 UniXNU 与 iPadOS 17.7.11（21H461）。

| 文件 | 内容 |
| --- | --- |
| [MarbleTouch.cpp](touch/MarbleTouch.cpp) | 板级供电、复位、SPI DMA、Goodix 协议、触点读取 |
| [GeniSpi.c](spi/GeniSpi.c) | Qualcomm QUP GENI 硬件 SPI 控制器 |
| [SmmuBypass.h](spi/SmmuBypass.h) | QUP DMA stream 与固件管理的 SMMU 配置 |
| [NativeHomeInput.cpp](hid/NativeHomeInput.cpp) | HID 设备、坐标换算、点击、边缘手势 |
| [OriginalHidDeviceABI.h](hid/OriginalHidDeviceABI.h) | 21H461 IOHIDDevice ABI 声明 |
| [HidHomeProbe.cpp](integration/HidHomeProbe.cpp) | 原工程中的 HID 初始化调用示例 |
| [触控移植](docs/触控移植.md) | 硬件参数、接口和接入条件 |
| [实机记录](docs/实机记录.md) | 物理操作验证范围 |
| [sources.json](sources.json) | 来源版本及文件摘要 |

物理点击打开设置、底部上滑返回桌面有 marble 实机记录。当前 HID 转发使用首个触点。

这些文件供接入现有 XNU 工程；构建依赖目标内核头文件、IOHIDFamily 和匹配的 OS ABI。

许可证为 BSD_2_Clause_Patent，正文见 [LICENSE](LICENSE)。SPI 组件保留 [上游许可证](spi/LICENSE)。
