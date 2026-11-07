# AdrenoPM4

先按[适配流程](/zh/porting/gpu)准备板级输入、修改源码、接入构建并逐步验证。

Adreno PM4 数据包编码 kit。构建 Adreno740 提交的命令流。

| 仓库 | 维护者 | 范围 |
| --- | --- | --- |
| [AdrenoPM4](https://github.com/UniXNU/AdrenoPM4) | [UniXNU](https://github.com/UniXNU) | Adreno PM4 数据包编码 kit。构建 Adreno740 提交的命令流。 |

## 状态

Adreno740 使用它。

## 构建

板级 GPU 归档配方将 `AdrenoPM4.c` 与选定 GPU 驱动一起编译。按 [GPU 适配](/zh/porting/gpu)匹配命令包和提交路径，再启用前端。
