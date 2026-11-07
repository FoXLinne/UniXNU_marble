# 启动链

UniXNU 类似 OpenCore，是通用的 UEFI 环境和驱动加载系统：统一的 UEFI 层加载设备所选的驱动，再启动系统。启动链有四次交接。每次交接都有固定输入、校验步骤和失败后的返回路径。

## 1. bootloader 到 BootShim

Android bootloader 读取启动镜像。kernel 字段是 gzip 流，前半部分是 BootShim，后半部分是 UEFI 固件卷。gzip 流之后是设备树。

BootShim 按 Mu-Silicium 设备配置中的固件基址和大小汇编：

```sh
clang --target=aarch64-none-elf -c \
  -DFD_BASE=0xA7100000 -DFD_SIZE=0x300000 \
  -DREQUIRES_KERNEL_HEADER=1 BootShim.S -o shim.o
llvm-objcopy -O binary shim.o shim.bin
```

设备配置要求时，shim 包含 ARM64 image header，描述 shim 加固件卷，bootloader 可以把两者加载到指定地址。

BootShim 重定位固件卷并进入 UEFI。

## 2. UEFI 到 UniXNU

固件卷包含 UniXNU 启动管理器和它的驱动。`UniXNUDxe` 把 `UniXNU.efi` 注册为 `Boot####` 选项，load option 数据为 `unixnu:firmware`。BDS 启动该应用，用户也可以从固件菜单启动。

启动管理器先扫描启动所在卷，再扫描其他卷，寻找 `\EFI\UniXNU\unixnu.toml`，解析并应用配置。

菜单出现前，UniXNU：

1. 发布每个 `[[block_slice]]`。
2. 加载并启动每个 `[[driver]]`。
3. 安装 ACPI 表。
4. 合并 `[match]` 条件满足的 kit。
5. 构造菜单。

## 3. UniXNU 到 DarwinBoot

类型为 `xnu` 的菜单项交给加载器协议：

```c
typedef struct {
  EFI_HANDLE    Volume;
  CONST CHAR16  *KernelPath;
  CONST CHAR16  *BootArgs;
  VOID          *DeviceTree;
  UINTN         DeviceTreeSize;
  VOID          *Ramdisk;
  UINTN         RamdiskSize;
  CONST UNIXNU_LOADER_KEXT *Kexts;
  UINTN         KextCount;
} UNIXNU_LOADER_BOOT_REQUEST;
```

UniXNU 为菜单项准备设备树：

1. 取固件 DTB、文件 DTB 或无 DTB。
2. 复制树并预留 64 KiB。
3. 应用配置和 kit 的全局补丁。
4. 应用菜单项补丁。
5. 最后应用 `[entry.chosen]` 属性。
6. 把树安装为 `EFI_DTB_TABLE`。

加载器收到树、ramdisk、启动参数和暂存的 kext。

## 4. DarwinBoot 到 XNU

DarwinBoot 校验并加载内核：

1. 从菜单项目录读取 `kernel.macho`。
2. 解析 Mach-O header，要求 `MH_MAGIC_64`、`CPU_TYPE_ARM64`、`MH_EXECUTE`、一个 `LC_MAIN` 和至少一个 `LC_SEGMENT_64`。
3. 检查入口位于可执行段。
4. 检查每个段页对齐、不重叠并位于 256 MiB 窗口内。
5. 读取 `kernel.macho.adt`。
6. 存在时读取 `kernel.macho.trustcache`。
7. 校验 trust cache：头计数、偏移、条目数和排序。
8. 从 GOP 读取 framebuffer 信息。
9. 分配连续物理窗口。
10. 复制段、ADT、trust cache 和 ramdisk。
11. 构造 `DARWIN_BOOT_ARGS`。
12. 退出 boot services。
13. 关闭 MMU 和缓存。
14. 在 EL1 跳转到入口。

`DARWIN_BOOT_ARGS` 布局匹配 Darwin 23：

| 偏移 | 字段 |
| --- | --- |
| 0x00 | revision、version、padding |
| 0x08 | 虚拟基址 |
| 0x10 | 物理基址 |
| 0x18 | 内存大小 |
| 0x20 | 内核数据末端 |
| 0x28 | video，六个 64 位值 |
| 0x60 | 设备树地址 |
| 0x68 | 设备树长度 |
| 0x6c | 命令行，1024 字节 |
| 0x478 | 实际内存大小 |

## 5. XNU 到用户态

XNU 从 `start.s` 开始，安装 bootstrap 页表，进入 C 启动路径，初始化平台专家并发布 IOKit registry。

平台专家匹配 ADT 根节点。通用 `ArmDTPlatform` 要求 `/cpus`、`/arm-io`、`/defaults`、`/options` 和 `/chosen` 中的 CHRP NVRAM proxy。它发布 CPU nub 和每个 `arm-io` 子节点。

存储驱动发布 userdata 分区，分区方案发布 `OS` 和 `SystemOS`。XNU 按 UUID 匹配根卷，挂载并启动用户态中的 `launchd`。

## 失败行为

| 阶段 | 失败 | 结果 |
| --- | --- | --- |
| bootloader | header 不匹配 | 拒绝镜像，设备留在 bootloader |
| 固件 | 无 GOP 或无输入 | UniXNU 以 headless 或文本模式运行 |
| UniXNU | 无配置 | 使用内置配置和可移动加载器 |
| DarwinBoot | 内核、ADT 或 trust cache 无效 | `EFI_COMPROMISED_DATA`，固件继续运行 |
| XNU | 缺少平台或存储驱动 | panic 日志给出缺失的 provider |

## 下一步

- [启动负载](/zh/concepts/payload)
- [预链接](/zh/concepts/prelink)
- [完整性与启动策略](/zh/concepts/security)
