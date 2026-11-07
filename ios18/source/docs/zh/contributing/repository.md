# 仓库布局

UniXNU 组装独立的 SoC、设备和组件仓库。

```text
UniXNU/
  AGENTS.md
  components.toml
  unixnu
  scripts/unixnu/
  profiles/arch/arm64/
  profiles/os/
  drivers/
  kits/
  patches/
  package/
  examples/device/
  docs/
```

| 仓库 | 职责 |
| --- | --- |
| `UniXNU` | 构建工具、组件目录、ARM64 支持、OS 配置、模板、UEFI package 和文档站 |
| `UniXNU-<soc>-common` | SoC 共享配置、DTSI、平台操作和补丁系列 |
| `UniXNU-<device>` | 板级 DTS、外设、组件选择、分区和打包 |
| 组件仓库 | 驱动、库、加载器和工具 |
| `uefi-<device>` | UEFI 平台集成 |

文档源码、导航、翻译、资源、配置和部署工作流均由 `UniXNU` 维护。每个维护中的仓库
包含简洁的 README 和根目录 `AGENTS.md`，链接到相关站点页面。

## 源码

组件目录记录规范 ID、源码版本、依赖、构建命令、输出和运行时标识符。解析器从目录、
设备 feeds、子模块和本地覆盖中选择源码。解析优先级见[配置参考](/zh/reference/configuration)。

| 类型 | 组装路径 | 内容 |
| --- | --- | --- |
| `driver` | `drivers/<ID>` | 驱动源码和参考 plist |
| `kit` | `kits/<ID>` | 库或工具 |
| `patch` | `patches/<name>` | 补丁集 |
| `script` | `scripts/` | 构建或验证脚本 |
| `package` | `package/` | UEFI package |
| `refer` | `refer/<name>` | 外部源码 |
| `soc` | 所选配置根目录 | SoC 配置和共享硬件支持 |

格式 2 按 ARM64、SoC、OS、设备顺序应用配置。组装目标为
`out/<device>/<os_profile>/<config>/tree`。模块使用链接，脚本被复制，设备 `package/`
成为 `kits/<board.kit>`。补丁后的源码和生成文件保存在目标输出树中。

## 组件开发

1. 阅读 `AGENTS.md`，搜索 `components.toml` 并检查现有接口。
2. 将共享控制器或协议代码放在组件仓库。
3. 将 SoC 操作和资源放在 SoC 仓库，板级策略放在设备仓库。
4. 记录源码身份、版本、依赖、构建命令、输出和运行时标识符。
5. 同步更新使用者、README、根目录 `AGENTS.md` 和中英文站点页面。
6. 运行受影响的构建命令，分别记录主机和硬件结果。

规范名称使用硬件范围、协议或功能。仓库改名保留提交历史和固定版本。
兼容别名按官方源码身份解析，社区源码保持自己的身份。

## 提交范围

| 前缀 | 主题 |
| --- | --- |
| `feat(<area>)` | 新能力 |
| `fix(<area>)` | 缺陷修复 |
| `docs` | 文档 |
| `build(<area>)` | 构建系统 |
| `chore` | 维护 |

提交信息描述改动，文档描述当前行为。

- [设备仓库](/zh/porting/device-repository)
- [组件目录](/zh/reference/components)
- [文档站](/zh/contributing/documentation)
- [构建](/zh/building)

## 设备记录与源码所有权

主仓库的子模块提供共享组件，板级驱动子模块由设备仓库维护。组件目录用于发现和选择。
官方设备的 `porting.toml` 供维护者和 agent 共用，设备页按 `devices.toml` 固定版本生成。
代码改动需要更新对应部分的经验或证据。参见[记录指南](/zh/porting/records)。
