# UEFI drivers

## Procedure

1. Prepare the package DEC and its protocol declarations. Generate distinct file
   and protocol GUIDs with `uuidgen`; fill the example identifiers below.
2. Add the driver source and INF, then register it in the target DSC. Place public
   protocol headers under the package's `Include/Protocol/` directory.
3. For firmware dispatch, add the INF to the target FDF and run
   `./unixnu build firmware` from UniXNU. Follow [firmware integration](/porting/uefi-firmware).
4. For file loading, build the EFI module and add its path to the payload configuration
   as shown below. Repack the payload and boot image.
5. Check driver-start logs and protocol publication before starting its consumer.



A UEFI driver is an EDK II module built as a `.efi` image. It runs in boot services and can publish protocols, bind devices and prepare hardware for the next stage.

The shared `UniXNURamDiskDxe` module in `package/UniXNUPkg/Driver/` exposes a Mu-Silicium FDT initrd as a read-only Block I/O device. Include it in the firmware DSC and FDF with DiskIoDxe and FAT support. See [firmware integration](/porting/uefi-firmware#required-integration).

## Module layout

```
MyDriver/
  MyDriver.c
  MyDriver.inf
  # public header: MyPkg/Include/Protocol/MyProtocol.h
```

`MyDriver.inf`:

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

`MyDriver.c`:

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

The driver receives its `LoadOptions` from the UniXNU `args` field.

## Build

Add the INF to a DSC:

```
[Components]
  MyPkg/MyDriver/MyDriver.inf
```

Build with EDK II:

```sh
build -p MyPkg/MyPkg.dsc -a AARCH64 -b DEBUG -t CLANGPDB -n 8
```

The result is `Build/MyPkg/DEBUG_CLANGPDB/AARCH64/MyDriver.efi`.

The UniXNU package DSC contains the standalone build:

```
package/UniXNUPkg/UniXNUPkg.dsc
```

`package/build.sh` copies the EFI binaries to `Build/Out/AARCH64-DEBUG`.

## Load from UniXNU

Copy the driver to the payload volume and add an entry to `unixnu.toml`:

```toml
[[driver]]
path = "Drivers/MyDriver.efi"
args = "board=1"
connect = true
enabled = true
```

| Key | Meaning |
| --- | --- |
| `path` | Path relative to `\EFI\UniXNU`, or `fv:<GUID>` |
| `args` | Load options string |
| `connect` | Run `ConnectController` on every handle after the driver starts |
| `enabled` | `false` skips the entry |

A kit can add the driver:

```toml
[[driver]]
file = "MyDriver.efi"
args = "from-kit"
```

The kit path resolves relative to the kit directory.

## Firmware volume path

`fv:<GUID>` loads a file from any firmware volume by file GUID:

```toml
[[driver]]
path = "fv:00000000-0000-0000-0000-000000000000"
```

This loads a driver already embedded in the firmware without copying it to the payload.

## Firmware volume inclusion

To embed a driver in the firmware:

1. Add the INF to the platform DSC under `[Components]`.
2. Add the INF to the FDF under `[FV.<name>] INF`.
3. Rebuild the firmware.

The firmware dispatcher starts DXE drivers during dispatch. A UEFI application in the FV is started by UniXNUDxe or by a `Boot####` option.

## Driver model

A UEFI driver that follows the driver model implements `EFI_DRIVER_BINDING_PROTOCOL`:

| Function | Role |
| --- | --- |
| `Supported` | Reports whether the driver can handle a controller |
| `Start` | Installs child handles or protocols |
| `Stop` | Removes them |

UniXNU calls `ConnectController` on every handle when `connect = true` and `[boot] connect_all` is true. This binds driver-model drivers to new devices.

## Protocols

A protocol is a GUID and a function table:

```c
typedef struct {
  UINT64 Signature;
  EFI_STATUS (EFIAPI *DoWork)(IN struct _MY_PROTOCOL *This);
} MY_PROTOCOL;

extern EFI_GUID gMyProtocolGuid;
```

The producer installs it with `InstallMultipleProtocolInterfaces`. A consumer locates it with `LocateProtocol` or `HandleProtocol`.

## Example modules

| Module | Purpose |
| --- | --- |
| `UniXNUPkg/Test/SampleDriver` | Installs `UNIXNU_SAMPLE_PROTOCOL` and records its load options |
| `UniXNUPkg/Test/HelloPayload` | Test application for injection |
| `UniXNUPkg/Driver/UniXNUDxe` | Registers the UniXNU application as a boot option |
| `drivers/DarwinBoot` | Installs `UNIXNU_LOADER_PROTOCOL` |

## Logging

UniXNU logs every driver result:

```
UNIXNU: driver <path> (<origin>): started
UNIXNU: driver <path> (<origin>): load failed: <status>
UNIXNU: driver <path> (<origin>): start failed: <status>
```

The menu shows the loaded and failed counts and the first failure reason.

## Next steps

- [XNU drivers](/drivers/xnu-drivers)
- [Kext bundles](/drivers/kexts)
- [UEFI firmware](/porting/uefi-firmware)
