# VerifiedRoot

先按[适配流程](/zh/porting/platform)准备板级输入、修改源码、接入构建并逐步验证。

Verified root 信任工具。构建并检查 owner 签名的根策略。

| 仓库 | 维护者 | 范围 |
| --- | --- | --- |
| [VerifiedRoot](https://github.com/UniXNU/VerifiedRoot) | [UniXNU](https://github.com/UniXNU) | Verified root 信任工具。构建并检查 owner 签名的根策略。 |

## 状态

owner 根链使用它。

## 构建

通过所选板级配方构建 owner policy 和消费它的存储归档。按[完整性与启动策略](/zh/concepts/security)检查镜像与信任输入，再完成[块设备集成](/zh/porting/platform#block-and-os-services)。
