# UEFI 驱动

## 操作流程

1. 准备 package DEC 和协议声明，用 `uuidgen` 分别生成文件和协议 GUID，填写下方示例标识符。
2. 加入驱动源码和 INF，在目标 DSC 注册。公共协议头放在 package 的 `Include/Protocol/`。
3. 通过固件派发时，将 INF 加入目标 FDF，从 UniXNU 执行 `./unixnu build firmware`，
   步骤见[固件集成](/zh/porting/uefi-firmware)。
4. 从文件加载时，构建 EFI 模块并按下文把路径加入 payload 配置，再打包 payload 和启动镜像。
5. 启动消费方前，确认驱动启动日志和协议发布。



UEFI 驱动是构建为 `.efi` 的 EDK II 模块，运行在 boot services，可以发布协议、绑定设备，为下一阶段准备硬件。

共享模块 `UniXNURamDiskDxe` 位于 `package/UniXNUPkg/Driver/`，把 Mu-Silicium FDT 中的 initrd 发布为只读 Block I/O 设备。将它与 DiskIoDxe、FAT 支持一起加入固件 DSC 和 FDF。详见[固件集成](/zh/porting/uefi-firmware#required-integration)。

## 模块布局

```
MyDriver/
  MyDriver.c
  MyDriver.inf
  # public header: MyPkg/Include/Protocol/MyProtocol.h
```

`MyDriver.inf`：

```ini
[Defines]
  INF_VERSION    = 0x00010019
  BASE_NAME      = MyDriver
  FILE_GUID      = 00000000-0000-0000-0000-000000000000
  MODULE_TYPE    = UEFI_DRIVER
  VERSION_STRING = 0.1
  ENTRY_POINT    = MyDriverEntry

[Sources]
  MyDriver.c

[Packages]
  MdePkg/MdePkg.dec
  UniXNUPkg/UniXNUPkg.dec

[LibraryClasses]
  MemoryAllocationLib
  UefiBootServicesTableLib
  UefiDriverEntryPoint

[Protocols]
  gMyProtocolGuid   ## PRODUCES
  gEfiLoadedImageProtocolGuid ## CONSUMES
```

`MyDriver.c`：

```c
#include <Uefi.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/MyProtocol.h>

STATIC MY_PROTOCOL mProtocol = { MY_SIGNATURE, NULL };

EFI_STATUS
EFIAPI
MyDriverEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_HANDLE                Handle = NULL;

  if (!EFI_ERROR (gBS->HandleProtocol (
                    ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded)) &&
      (Loaded->LoadOptions != NULL))
  {
    mProtocol.Options = AllocateCopyPool (
                          Loaded->LoadOptionsSize, Loaded->LoadOptions);
  }

  return gBS->InstallMultipleProtocolInterfaces (
                &Handle, &gMyProtocolGuid, &mProtocol, NULL);
}
```

驱动通过 UniXNU 的 `args` 字段获得 `LoadOptions`。

## 构建

把 INF 加入 DSC：

```
[Components]
  MyPkg/MyDriver/MyDriver.inf
```

```sh
build -p MyPkg/MyPkg.dsc -a AARCH64 -b DEBUG -t CLANGPDB -n 8
```

输出为 `Build/MyPkg/DEBUG_CLANGPDB/AARCH64/MyDriver.efi`。

UniXNU package 的独立构建 DSC 是：

```
package/UniXNUPkg/UniXNUPkg.dsc
```

`package/build.sh` 把 EFI 程序复制到 `Build/Out/AARCH64-DEBUG`。

## 从 UniXNU 加载

把驱动复制到负载卷，并在 `unixnu.toml` 中加入：

```toml
[[driver]]
path = "Drivers/MyDriver.efi"
args = "board=1"
connect = true
enabled = true
```

| 键 | 含义 |
| --- | --- |
| `path` | 相对 `\EFI\UniXNU` 的路径，或 `fv:<GUID>` |
| `args` | load options 字符串 |
| `connect` | 驱动启动后对所有 handle 运行 `ConnectController` |
| `enabled` | `false` 跳过 |

kit 可以加入驱动：

```toml
[[driver]]
file = "MyDriver.efi"
args = "from-kit"
```

kit 路径相对 kit 目录解析。

## 固件卷路径

`fv:<GUID>` 按文件 GUID 从任意固件卷加载：

```toml
[[driver]]
path = "fv:00000000-0000-0000-0000-000000000000"
```

这样无需把驱动复制到负载即可加载固件中的驱动。

## 加入固件卷

1. 把 INF 加入平台 DSC 的 `[Components]`。
2. 把 INF 加入 FDF 的 `[FV.<name>] INF`。
3. 重新构建固件。

固件 dispatcher 启动 DXE 驱动。FV 中的 UEFI 应用由 UniXNUDxe 或 `Boot####` 选项启动。

## Driver model

遵循 driver model 的 UEFI 驱动实现 `EFI_DRIVER_BINDING_PROTOCOL`：

| 函数 | 作用 |
| --- | --- |
| `Supported` | 报告能否处理控制器 |
| `Start` | 安装子 handle 或协议 |
| `Stop` | 移除 |

`connect = true` 且 `[boot] connect_all` 为 true 时，UniXNU 对所有 handle 调用 `ConnectController`。

## 协议

协议是 GUID 加函数表：

```c
typedef struct {
  UINT64 Signature;
  EFI_STATUS (EFIAPI *DoWork)(IN struct _MY_PROTOCOL *This);
} MY_PROTOCOL;

extern EFI_GUID gMyProtocolGuid;
```

生产者用 `InstallMultipleProtocolInterfaces` 安装，消费者用 `LocateProtocol` 或 `HandleProtocol` 查找。

## 示例模块

| 模块 | 用途 |
| --- | --- |
| `UniXNUPkg/Test/SampleDriver` | 安装 `UNIXNU_SAMPLE_PROTOCOL` 并记录 load options |
| `UniXNUPkg/Test/HelloPayload` | 注入测试应用 |
| `UniXNUPkg/Driver/UniXNUDxe` | 把 UniXNU 注册为启动选项 |
| `drivers/DarwinBoot` | 安装 `UNIXNU_LOADER_PROTOCOL` |

## 日志

UniXNU 记录每个驱动结果：

```
UNIXNU: driver <path> (<origin>): started
UNIXNU: driver <path> (<origin>): load failed: <status>
UNIXNU: driver <path> (<origin>): start failed: <status>
```

菜单显示加载和失败数量，以及第一条失败原因。

## 下一步

- [XNU 驱动](/zh/drivers/xnu-drivers)
- [kext 包](/zh/drivers/kexts)
- [UEFI 固件](/zh/porting/uefi-firmware)
