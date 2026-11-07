# 术语表

| 术语 | 含义 |
| --- | --- |
| ADT | Apple 设备树，XNU 从 `.adt` sidecar 读取的紧凑节点编码 |
| AMFI | Apple Mobile File Integrity，原始代码签名和 entitlement 策略 |
| ABL | Android bootloader，加载启动镜像的原有 bootloader |
| BootShim | 启动 UEFI 固件卷的小型 AArch64 shim |
| UniXNU | UEFI 启动管理器 |
| DarwinBoot | UniXNU 的 XNU 加载器 |
| defconfig | 选择内核、package 和驱动选项的 `CONFIG_*` 文件 |
| 设备仓库 | 含 `board.toml` 和设备模块的仓库 |
| ADT sidecar | `kernel.macho.adt`，内核旁边的设备树 |
| feed | `feeds.toml` 中声明的 git 源码 |
| 固件卷 | EDK II `SILICIUM_UEFI.fd` 镜像 |
| IOMedia | IOKit 中表示磁盘或卷的对象 |
| IPSW | Apple 恢复镜像，提供 kernelcache、设备树、磁盘镜像和 trust cache |
| kernelcache | 原始预链接 Apple 内核及其 kext |
| kext | 内核扩展包 |
| kit | 含驱动、补丁、overlay、ACPI 表和 kext 的 UniXNU 包 |
| 模块 | 按名称选择的驱动、kit、补丁、package 或脚本 |
| personality | 驱动的 IOKit 匹配字典 |
| PGZ | zone 分配器的 secondary page |
| 预链接 | 把原生内核与原始 kernelcache 合并的步骤 |
| SMMU | ARM System Memory Management Unit |
| trust cache | Apple 的可信代码哈希列表 |
| userdata | 含 OS 和 SystemOS 窗口的分区镜像 |
| SHNGROOT | userdata 镜像开头的 manifest |
| SystemOS | IPSW 中的 cryptex 系统卷 |
| DSC | EDK II 平台描述文件 |
| FDF | EDK II 固件描述文件 |
| INF | EDK II 模块描述文件 |
| DXE | UEFI driver execution environment |
| BDS | UEFI boot device selection 阶段 |
| GOP | UEFI graphics output protocol |
| CHRP | Apple 设备树使用的 NVRAM bank 格式 |
| PGZ 探针 | zone 分配器对齐探针 |

## 下一步

- [移植设备](/zh/porting/steps)
- [命令行](/zh/reference/cli)
- [设备](/zh/devices/)
