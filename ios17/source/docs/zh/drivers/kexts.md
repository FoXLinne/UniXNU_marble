# kext 包

随项目提供的 DarwinBoot 启动预链接内核，驱动集成路径见[内置 XNU 驱动](/zh/drivers/xnu-drivers)。本页说明为实现 revision 2 并接受 kext 请求的加载器准备 bundle。准备前先选择并确认该加载器。

kext 包是标准 macOS 或 iOS 内核扩展目录。UniXNU 暂存包文件，并通过 `UNIXNU_LOADER_PROTOCOL` 交给 XNU 加载器。

## 操作步骤 {#procedure}

1. 确认所选加载器实现 revision 2 kext 请求和所需信任策略。随项目提供的 DarwinBoot 使用[内置驱动](/zh/drivers/xnu-drivers)集成。
2. 按[构建 kext](#构建-kext)准备[包布局](#包布局)、[Info.plist](#info-plist)和匹配二进制，核对依赖与目标 OS ABI。
3. 完成该加载器要求的[签名与信任](#签名与信任)，再将包[注册到目标启动项或 kit](#注册到-unixnu)。
4. 核对[暂存校验](#unixnu-校验)和[依赖顺序](#顺序与依赖)，重新构建包含它的 payload 和启动镜像。
5. 使用该加载器启动，分别记录暂存、加载器接受和驱动初始化结果。XNU 使用暂存包之前，需要加载器完成注入。

## 包布局

```
MyDriver.kext/
  Contents/
    Info.plist
    MacOS/
      MyDriver
```

包名以 `.kext` 结尾。plist 默认路径为 `Contents/Info.plist`，可执行文件路径默认取 `CFBundleExecutable`，相对包目录。

无代码 kext 只有 plist，可以声明依赖或引用其他二进制的 personality。

## Info.plist

```xml
<dict>
  <key>CFBundleIdentifier</key><string>org.example.MyDriver</string>
  <key>CFBundleName</key><string>MyDriver</string>
  <key>CFBundleExecutable</key><string>MyDriver</string>
  <key>CFBundlePackageType</key><string>KEXT</string>
  <key>CFBundleVersion</key><string>1.0.0</string>
  <key>OSBundleRequired</key><string>Root</string>
  <key>OSBundleLibraries</key>
  <dict>
    <key>com.apple.kpi.iokit</key><string>23.6.0</string>
  </dict>
  <key>IOKitPersonalities</key>
  <dict>
    <key>MyDevice</key>
    <dict>
      <key>IOClass</key><string>MyDriver</string>
      <key>IOProviderClass</key><string>IOPlatformDevice</string>
      <key>IONameMatch</key><string>my-device</string>
      <key>IOProbeScore</key><integer>1000</integer>
    </dict>
  </dict>
</dict>
```

## 注册到 UniXNU

菜单项级 kext：

```toml
[[entry]]
id = "xnu"
type = "xnu"
path = "Payloads/kernel.macho"

[[entry.kext]]
bundle = "Kexts/MyDriver.kext"
```

kit 级 kext 指定菜单项：

```toml
[[kext]]
bundle = "MyDriver.kext"
entry = "xnu"
plist = "Contents/Info.plist"
executable = "Contents/MacOS/MyDriver"
enabled = true
```

| 键 | 含义 |
| --- | --- |
| `bundle` | 相对配置或 kit 的包目录 |
| `plist` | 包内 plist 路径 |
| `executable` | 包内可执行文件路径，无代码 kext 留空 |
| `entry` | 菜单项 id，kit kext 必需 |
| `enabled` | `false` 跳过 |

菜单项 kext 和 kit kext 按配置顺序暂存，kit kext 在前，菜单项 kext 在后。重复包路径会被拒绝。

## UniXNU 校验

UniXNU 暂存前校验：

- 包名以 `.kext` 结尾。
- 所有路径相对且不含 `.` 或 `..` 段。
- plist 非空。
- 可执行文件存在时为 thin AArch64 `MH_KEXT_BUNDLE`：
  - Magic `0xfeedfacf`。
  - CPU type `0x0100000c`。
  - File type `11`。
  - Subtype `0` 或 `2`（arm64 或 arm64e）。
  - load commands 位于文件内。
- kit kext 指定菜单项。
- 菜单项 kext 不指定菜单项。

暂存的是未修改的 plist 和可执行文件。加载器校验签名、解析依赖并注入代码。

## 加载器协议

加载器收到：

```c
typedef struct {
  CONST CHAR16  *BundlePath;
  CONST CHAR16  *PlistPath;
  CONST CHAR16  *ExecutablePath;
  CONST VOID    *InfoPlist;
  UINTN         InfoPlistSize;
  CONST VOID    *Executable;
  UINTN         ExecutableSize;
} UNIXNU_LOADER_KEXT;
```

缓冲区属于 UniXNU，在 `Boot()` 期间有效。

接受 kext 的加载器实现协议 revision 2。发布版 DarwinBoot 接受内核、ADT sidecar、trust cache 和可选 ramdisk，拒绝携带暂存 kext 的请求。

## 构建 kext

用 Xcode 和 iOS kernel SDK 构建：

```sh
xcodebuild -target MyDriver -sdk iphoneos -configuration Release \
  ARCHS=arm64 ONLY_ACTIVE_ARCH=YES
```

目标使用 `MODULE_TYPE = KEXT` 并链接 kernel frameworks：

| 设置 | 值 |
| --- | --- |
| `MACH_O_TYPE` | `mh_kext_bundle` |
| `PRODUCT_TYPE` | `com.apple.product-type.kernel-extension` |
| `SDKROOT` | `iphoneos` |
| `ARCHS` | `arm64` 或 `arm64e` |
| `KERNEL_FRAMEWORKS` | `Kernel` |

可执行文件必须匹配 `xnu-10063.141.1` 的内核 ABI 和 iOS 17 deployment target。

## 签名与信任

AMFI 按 trust cache 和代码签名校验 kext。不受信任的 kext 加载失败并给出代码签名错误。

负载的 trust cache 包必须包含 kext 的 CD hash。负载步骤只接受 SHA-256 位于 `[payload] trust_cache_sha256` 的包。

## 顺序与依赖

UniXNU 按配置顺序暂存。加载器解析 `OSBundleLibraries`，按依赖顺序启动 kext。

无代码 kext 可以用于表达依赖，或把 personality 附到已有二进制上。

## 下一步

- [XNU 驱动](/zh/drivers/xnu-drivers)
- [完整性与启动策略](/zh/concepts/security)
- [启动配置](/zh/reference/boot-configuration)
