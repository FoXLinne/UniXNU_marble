# Contributing to UniXNU

Read the [documentation](https://unixnu.com) before changing the build driver, the kernel patches or a device port. The source is in `docs/`. The [repository layout](https://unixnu.com/contributing/repository) page describes the module system.

## Repository scope

| Repository | Responsibility |
| --- | --- |
| `UniXNU` | Build tools, component catalog, ARM64 support, OS profiles, template, UEFI package, documentation site |
| `UniXNU-<soc>-common` | Shared SoC configuration, DTSI, platform operations, patch series |
| `UniXNU-<device>` | Board DTS, peripherals, selected components, partitions, packaging |
| Component repositories | Drivers, libraries, loaders, tools |
| `uefi-<device>` | UEFI platform integration |

Board policy belongs in the device repository. Shared SoC operations belong in the SoC repository. Reusable drivers and libraries belong in component repositories. The site source, navigation, translations, assets, configuration and deployment workflow belong in `UniXNU`.

## Component work

1. Search `components.toml` and inspect existing interfaces.
2. Put shared controller or protocol code in its component repository.
3. Put SoC operations and resources in the SoC repository and board policy in the device repository.
4. Record source identity, revision, dependencies, build command, outputs and runtime identifiers.
5. Update consumers, README and the English and Chinese site pages together.
6. Run the affected build commands and record host and hardware results separately.

Canonical names use hardware scope, protocol or function. Repository renames preserve history and pinned revisions. Compatibility aliases use official source identity; community sources keep their own identity.

## Repository topics

Every maintained driver repository and every published device repository carries the `unixnu` GitHub topic. See [driver repositories](https://unixnu.com/drivers/repositories) and [device repository](https://unixnu.com/porting/device-repository).

## Private blobs

Do not commit Apple IPSW files, kernelcaches, disk images, trust caches, Data seeds, vendor firmware or other proprietary binaries. The device repository lists its inputs in `proprietary-files.txt`; the files stay in `.cache/firmware`.

## Development

```sh
git clone https://github.com/UniXNU/UniXNU
cd UniXNU
./unixnu defconfig <name> -D ../UniXNU-<device>
./unixnu fetch
./unixnu status --json
./unixnu build kernel
./unixnu build payload
./unixnu bootimg --uefi ../uefi-<device> --dtb <board>.dtb
```

Use `--local-module` or `--local` to test a module from a working directory without committing it.

## Kernel patches

Follow [source and patches](https://unixnu.com/xnu/source-and-patches#procedure):

1. Build the selected kernel once to prepare `out/<device>/<os_profile>/<config>/tree/.cache/xnu-work`.
2. Copy the files to edit into a temporary Git directory with their XNU-relative paths and stage them.
3. Edit the copy and export the patch with `git diff`.
4. Add the patch to the owning series (ARM64, SoC, OS, device or component) in dependency order.
5. Check the order with `./unixnu status --json` and rebuild the kernel.

## Commit messages

| Prefix | Subject |
| --- | --- |
| `feat(<area>)` | New capability |
| `fix(<area>)` | Defect repair |
| `docs` | Documentation |
| `build(<area>)` | Build system |
| `chore` | Maintenance |

Commit messages describe changes. Documentation describes current behavior. Keep one subject per commit.

## Documentation

The site is a bilingual VitePress project in `docs/`, published to https://unixnu.com.

| Locale | Path |
| --- | --- |
| English | `docs/` |
| Simplified Chinese | `docs/zh/` |

```sh
cd docs
npm install
npm run dev
npm run build
```

Follow the [documentation procedure](https://unixnu.com/contributing/documentation#procedure). A page change updates both locales in the same pull request. Add a new page to both sidebars in `docs/.vitepress/config.mts` and link it from a page reachable from [Port a device](https://unixnu.com/porting/). `npm run build` fails on a broken internal link.

## Device records

Official devices maintain `porting.toml`. Update the affected areas' findings or evidence with each code change. The site renders the revision pinned in `devices.toml`. See the [record guide](https://unixnu.com/porting/records).

## Security

Report a security issue privately to the maintainers. Do not open a public issue for a trust cache, code signing or boot policy defect.

## License

Contributions are licensed under BSD-2-Clause-Patent. The `LICENSE` file in each directory applies to that directory.
