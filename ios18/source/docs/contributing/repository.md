# Repository layout

UniXNU assembles independent SoC, device, and component repositories.

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

| Repository | Responsibility |
| --- | --- |
| `UniXNU` | Build tools, component catalog, ARM64 support, OS profiles, template, UEFI package, documentation site |
| `UniXNU-<soc>-common` | Shared SoC configuration, DTSI, platform operations, patch series |
| `UniXNU-<device>` | Board DTS, peripherals, selected components, partitions, packaging |
| Component repositories | Drivers, libraries, loaders, tools |
| `uefi-<device>` | UEFI platform integration |

The site source, navigation, translations, assets, configuration, and deployment
workflow are maintained in `UniXNU`. Each maintained repository has a concise
README and root `AGENTS.md` linking to its relevant pages.

## Sources

The component catalog records canonical IDs, source revisions, dependencies,
build commands, outputs, and runtime identifiers. The resolver selects sources
from the catalog, device feeds, submodules, and local overrides. See the
[configuration reference](/reference/configuration) for resolution precedence.

| Kind | Assembled path | Contents |
| --- | --- | --- |
| `driver` | `drivers/<ID>` | Driver source and reference plist |
| `kit` | `kits/<ID>` | Library or tool |
| `patch` | `patches/<name>` | Patch set |
| `script` | `scripts/` | Build or verification scripts |
| `package` | `package/` | UEFI package |
| `refer` | `refer/<name>` | External source |
| `soc` | Selected profile root | SoC profile and shared hardware support |

Format 2 applies ARM64, SoC, OS, and device profiles. The assembled target is
`out/<device>/<os_profile>/<config>/tree`. Modules are linked, scripts are copied,
and device `package/` appears as `kits/<board.kit>`. Patched sources and generated
files stay in the target output tree.

## Component work

1. Read `AGENTS.md`, search `components.toml`, and inspect existing interfaces.
2. Put shared controller or protocol code in its component repository.
3. Put SoC operations and resources in the SoC repository and board policy in the device repository.
4. Record source identity, revision, dependencies, build command, outputs, and runtime identifiers.
5. Update consumers, README, root `AGENTS.md`, and English/Chinese site pages together.
6. Run the affected build commands and record host and hardware results separately.

Canonical names use hardware scope, protocol, or function. Repository renames
preserve history and pinned revisions. Compatibility aliases use official source
identity; community sources keep their own identity.

## Commit scope

| Prefix | Subject |
| --- | --- |
| `feat(<area>)` | New capability |
| `fix(<area>)` | Defect repair |
| `docs` | Documentation |
| `build(<area>)` | Build system |
| `chore` | Maintenance |

Commit messages describe changes. Documentation describes current behavior.

- [Device repository](/porting/device-repository)
- [Component catalog](/reference/components)
- [Documentation site](/contributing/documentation)
- [Build](/building)

## Device records and source ownership

Main-repository submodules provide shared components. Device repositories own
board driver submodules. The catalog supports discovery and selection. Official
devices maintain `porting.toml` for maintainers and agents; the site renders the
revision pinned in `devices.toml`. Code changes update the corresponding findings
or evidence. See the [record guide](/porting/records).
