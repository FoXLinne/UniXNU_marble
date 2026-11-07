# Boot chain

UniXNU is a general UEFI environment and driver loading system, similar to OpenCore: a common UEFI layer loads the drivers a device selects, then boots the OS. The boot chain has four handoffs. Each handoff has a fixed input, a validation step and a failure mode that returns to the previous stage.

## 1. Bootloader to BootShim

The stock Android bootloader reads the boot image. The kernel field is a gzip stream. The first part is BootShim, the second part is the UEFI firmware volume. The device tree follows the gzip stream.

BootShim is assembled for the firmware base and size from the Mu-Silicium device configuration:

```sh
clang --target=aarch64-none-elf -c \
  -DFD_BASE=0xA7100000 -DFD_SIZE=0x300000 \
  -DREQUIRES_KERNEL_HEADER=1 BootShim.S -o shim.o
llvm-objcopy -O binary shim.o shim.bin
```

The shim contains an ARM64 image header when the device configuration requires one. The header describes the shim plus the firmware volume, so the bootloader can load the pair at the requested address.

BootShim relocates the firmware volume and enters UEFI.

## 2. UEFI to UniXNU

The firmware volume contains the UniXNU boot manager and its driver. `UniXNUDxe` registers `UniXNU.efi` as a `Boot####` option with the load option data `unixnu:firmware`. BDS starts the application, or the user starts it from the firmware menu.

The boot manager scans the volume it was started from, then every other volume, for `\EFI\UniXNU\unixnu.toml`. It parses the file and applies the configuration.

Before the menu, UniXNU:

1. Publishes every `[[block_slice]]`.
2. Loads and starts every `[[driver]]`.
3. Installs every ACPI table.
4. Merges every kit whose `[match]` section matches.
5. Builds the menu.

## 3. UniXNU to DarwinBoot

An entry of type `xnu` is passed to the loader protocol:

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

UniXNU prepares the device tree for the entry:

1. It takes the firmware DTB, a file DTB, or no DTB.
2. It copies the tree with 64 KiB of slack.
3. It applies global patches from the configuration and kits.
4. It applies the entry patches.
5. It applies `[entry.chosen]` properties last.
6. It installs the tree as `EFI_DTB_TABLE`.

The loader receives the tree, the ramdisk, the boot arguments and any staged kexts.

## 4. DarwinBoot to XNU

DarwinBoot validates and loads the kernel:

1. Read `kernel.macho` from the entry directory.
2. Parse the Mach-O header. It requires `MH_MAGIC_64`, `CPU_TYPE_ARM64`, `MH_EXECUTE`, one `LC_MAIN` and at least one `LC_SEGMENT_64`.
3. Check that the entry point lies in an executable segment.
4. Check that every segment is page aligned, non-overlapping and within the 256 MiB window.
5. Read `kernel.macho.adt`.
6. Read `kernel.macho.trustcache` when present.
7. Validate the trust cache: header count, offsets, entry count and sorted hashes.
8. Read the framebuffer information from GOP.
9. Allocate a contiguous physical window.
10. Copy the segments, the ADT, the trust cache and the ramdisk.
11. Build `DARWIN_BOOT_ARGS`.
12. Exit boot services.
13. Disable the MMU and caches.
14. Jump to the entry point in EL1.

The `DARWIN_BOOT_ARGS` layout matches Darwin 23:

| Offset | Field |
| --- | --- |
| 0x00 | revision, version, padding |
| 0x08 | virtual base |
| 0x10 | physical base |
| 0x18 | memory size |
| 0x20 | top of kernel data |
| 0x28 | video, six 64-bit values |
| 0x60 | device tree address |
| 0x68 | device tree length |
| 0x6c | command line, 1024 bytes |
| 0x478 | memory size actual |

## 5. XNU to userland

XNU starts at `start.s`. It installs the bootstrap page tables, enters the C boot path, initializes the platform expert and publishes the IOKit registry.

The platform expert matches the ADT root. The generic `ArmDTPlatform` requires `/cpus`, `/arm-io`, `/defaults`, `/options` and a CHRP NVRAM proxy in `/chosen`. It publishes CPU nubs and every `arm-io` child.

The storage driver publishes the userdata partition. The partition scheme publishes `OS` and `SystemOS`. XNU matches the root volume by UUID, mounts it, and starts `launchd` from the selected userland.

## Failure behavior

| Stage | Failure | Result |
| --- | --- | --- |
| Bootloader | Header mismatch | Image refused, device stays in bootloader |
| Firmware | No GOP or no input | UniXNU runs headless or in text mode |
| UniXNU | No config | Built-in configuration and removable loaders |
| DarwinBoot | Invalid kernel, ADT or trust cache | `EFI_COMPROMISED_DATA`, firmware retained |
| XNU | Missing platform or storage driver | Kernel panic with the missing provider in the log |

## Next steps

- [Payload](/concepts/payload) describes the FAT volume.
- [Prelink](/concepts/prelink) describes the kernel and kernelcache binding.
- [Integrity and boot policy](/concepts/security) describes trust caches and AMFI.
