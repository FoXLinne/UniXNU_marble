# Glossary

| Term | Meaning |
| --- | --- |
| ADT | Apple device tree. The compact node encoding XNU reads from the `.adt` sidecar |
| AMFI | Apple Mobile File Integrity. The original code signing and entitlement policy |
| ABL | Android bootloader. The stock bootloader that loads the boot image |
| BootShim | The small AArch64 shim that starts the UEFI firmware volume |
| UniXNU | The UEFI boot manager |
| DarwinBoot | The UniXNU XNU loader |
| Defconfig | The `CONFIG_*` file that selects kernel, package and driver options |
| Device repository | The repository with `board.toml` and the device modules |
| ADT sidecar | `kernel.macho.adt`, the device tree next to the kernel |
| Feed | A git source declared in `feeds.toml` |
| Firmware volume | The EDK II `SILICIUM_UEFI.fd` image |
| IOMedia | The IOKit object that represents a disk or volume |
| IPSW | Apple restore image. The source of the kernelcache, device tree, disk images and trust caches |
| Kernelcache | The original prelinked Apple kernel with its kexts |
| Kext | A kernel extension bundle |
| Kit | A UniXNU bundle with drivers, patches, overlays, ACPI tables and kexts |
| Module | A driver, kit, patch, package or script selected by name |
| Personality | The IOKit match dictionary for a driver |
| PGZ | The zone allocator secondary page |
| Prelink | The step that combines the native kernel with the original kernelcache |
| SMMU | The ARM System Memory Management Unit |
| Trust cache | The Apple list of trusted code hashes |
| Userdata | The partition image with the OS and SystemOS windows |
| SHNGROOT | The manifest at the start of the userdata image |
| SystemOS | The cryptex system volume from the IPSW |
| DSC | The EDK II platform description file |
| FDF | The EDK II firmware description file |
| INF | The EDK II module description file |
| DXE | The UEFI driver execution environment |
| BDS | The UEFI boot device selection phase |
| GOP | The UEFI graphics output protocol |
| CHRP | The NVRAM bank format used by the Apple device tree |
| PGZ probe | The zone allocator alignment instrument |

## Next steps

- [Port a device](/porting/steps)
- [Command line](/reference/cli)
- [Devices](/devices/)
