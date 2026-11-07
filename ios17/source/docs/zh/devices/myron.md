# Redmi K90 Pro Max / Poco F8 Ultra（myron）

myron 是搭载 SM8850 的 Redmi K90 Pro Max 和 Poco F8 Ultra。下面的项目是独立的社区工作，UniXNU 组织不维护它。

| 仓库 | 维护者 | 内容 |
| --- | --- | --- |
| [UniXNU-myron](https://github.com/zg7681/UniXNU-myron) | [zg7681](https://github.com/zg7681) | 核心分支；`myron-next` 分支包含 A8XX Adreno 和 CPU affinity 改动 |
| [NativeAdreno-myron](https://github.com/zg7681/NativeAdreno-myron) | [zg7681](https://github.com/zg7681) | Adreno 驱动分支，包含 A8XX 寄存器表、continuation thread 和 user-thread 选择 |
| [NativeUfs-myron](https://github.com/zg7681/NativeUfs-myron) | [zg7681](https://github.com/zg7681) | UFS 驱动分支 |

## 状态

这些仓库是核心和驱动分支。尚未发布设备仓库、板级文件、defconfig 或真机结果。`myron-next` 分支包含 A8XX Adreno 寄存器表和 CPU affinity 匹配改动。在设备仓库和真机日志出现前，把这个移植视为进行中。

## 维护

维护者拥有仓库和发布决定。UniXNU 文档记录项目和状态，不接管维护。
