# Technical reference

Use this page to find the file, procedure and verification for a change.
For a complete workflow, follow [Port a device](/porting/steps) or
[Build and boot](/building). Device values and current results are listed in
[Devices](/devices/).

## 1. Find the setting or implementation

| Change | File or implementation | Procedure |
| --- | --- | --- |
| Local input paths | Main checkout `unixnu.local.toml` | [Configure build inputs](/reference/configuration#local-build-inputs) |
| Device, SoC or component selection | Device `board.toml`, `feeds.toml`, `configs/` | [Resolve the port](/porting/steps#_4-resolve-and-inspect-the-build) |
| Hardware resources | SoC DTSI and board DTS | [Device tree](/porting/device-tree#procedure) |
| Driver behavior | Selected component or board package | [Adapt hardware](/porting/drivers), [develop a driver](/drivers/overview#procedure) |
| Kernel behavior | Owning profile or component patch series | [Change XNU](/xnu/source-and-patches#procedure) |
| XNU command line | `[payload] kernel_args` or local `[build] kernel_args` | [Apply boot arguments](/xnu/boot-arguments#apply-arguments) |
| Boot menu or kit | Payload `EFI/UniXNU/unixnu.toml`, kit `Kit.toml` | [Boot configuration](/reference/boot-configuration#procedure) |
| Device support record | Device `porting.toml` and main `devices.toml` | [Update and publish the record](/porting/records#procedure) |
| Documentation | Main checkout `docs/`, English and Chinese | [Edit and publish documentation](/contributing/documentation#procedure) |

**Result:** identify the owning file and the procedure before editing it.

## 2. Read the matching reference

| Subject | References |
| --- | --- |
| Build driver | [Commands and options](/reference/cli), [configuration fields](/reference/configuration), [output paths and reports](/reference/output-layout) |
| Components | [Catalog and source identity](/reference/components), [repositories and component pages](/drivers/repositories) |
| Driver interfaces | [UEFI drivers](/drivers/uefi-drivers), [built-in XNU drivers](/drivers/xnu-drivers), [kext bundles](/drivers/kexts), [device-tree matching](/drivers/device-tree-matching) |
| XNU | [Source and patch ownership](/xnu/source-and-patches), [patch catalog](/xnu/patch-reference), [4 KiB page tables](/xnu/four-k-page-tables) |
| Boot and image formats | [Boot chain](/concepts/boot-chain), [prelink](/concepts/prelink), [payload](/concepts/payload), [userdata layout](/concepts/userdata), [boot configuration](/reference/boot-configuration) |
| Integrity | [Trust caches and boot policy](/concepts/security) |
| Project structure | [Repository ownership](/contributing/repository), [terms](/reference/glossary) |

The field tables give accepted values and paths. Follow the linked procedure
for the required order and build commands.

## 3. Verify the affected output

Run commands from the main checkout with the intended device selected:

```sh
./unixnu status --json
```

Check the device, OS profile, configuration, module origins and output directory.
Then use the verification for the change:

| Change | Verification | Expected result |
| --- | --- | --- |
| Kernel, driver or patches | [Kernel build](/guide/build-kernel), then [bring-up](/porting/bring-up) | Kernel and prelink succeed; record the affected device checkpoint separately |
| Firmware or payload | [Build and boot](/building) | Updated FD, payload and boot image use the intended inputs |
| Boot arguments | [Inspect payload configuration](/xnu/boot-arguments#apply-arguments) | Packaged `args` match the intended command line |
| Common UEFI behavior | [QEMU procedure](/reference/qemu#procedure) | Selected scenario reaches its expected log markers |
| Porting record or documentation | [Record check](/porting/records#procedure), [site build](/contributing/documentation#procedure) | Record validation and documentation build pass |

For a failed step, follow [troubleshooting](/reference/troubleshooting#procedure)
and save the command, input revisions and relevant [logs](/guide/debugging#procedure).
