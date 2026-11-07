# Kext bundles

The shipped DarwinBoot starts prelinked kernels. Its driver integration path is [built-in XNU drivers](/drivers/xnu-drivers). This page describes bundle staging for a loader that implements revision 2 and accepts kext requests. Select and verify that loader before preparing bundles.

A kext bundle is a standard macOS or iOS kernel extension directory. UniXNU stages the bundle files and passes them to an XNU loader through `UNIXNU_LOADER_PROTOCOL`.

## Procedure {#procedure}

1. Verify that the selected loader implements revision 2 kext requests and the required trust policy. The shipped DarwinBoot integration uses [built-in drivers](/drivers/xnu-drivers).
2. Prepare the [bundle](#bundle-layout), [Info.plist](#info-plist) and matching binary using [Build a kext](#build-a-kext). Check dependencies and the target OS ABI.
3. Complete [signing and trust](#signing-and-trust) for that loader, then [register the bundle](#register-with-unixnu) in the intended entry or kit.
4. Review [staging validation](#validation-by-unixnu) and [dependency order](#order-and-dependencies). Rebuild the containing payload and boot image.
5. Boot with that loader and record staging, loader acceptance and driver initialization separately. A staged bundle requires the loader's injection step before XNU can use it.

## Bundle layout

```
MyDriver.kext/
  Contents/
    Info.plist
    MacOS/
      MyDriver
```

The bundle name ends with `.kext`. The plist path defaults to `Contents/Info.plist`. The executable path defaults to the value in `CFBundleExecutable`, relative to the bundle.

A codeless kext has a plist and no executable. It can declare dependencies or personalities that reference another binary.

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

## Register with UniXNU

An entry scoped kext:

```toml
[[entry]]
id = "xnu"
type = "xnu"
path = "Payloads/kernel.macho"

[[entry.kext]]
bundle = "Kexts/MyDriver.kext"
```

A kit scoped kext names the entry:

```toml
[[kext]]
bundle = "MyDriver.kext"
entry = "xnu"
plist = "Contents/Info.plist"
executable = "Contents/MacOS/MyDriver"
enabled = true
```

| Key | Meaning |
| --- | --- |
| `bundle` | Bundle directory, relative to the configuration or kit |
| `plist` | Plist path inside the bundle |
| `executable` | Executable path inside the bundle. Empty for a codeless kext |
| `entry` | Entry id. Required for kit kexts |
| `enabled` | `false` skips the bundle |

Entry kexts and kit kexts are staged in configuration order, with kit kexts first, then entry kexts. A duplicate bundle path is refused.

## Validation by UniXNU

UniXNU validates the bundle before staging:

- The bundle name ends with `.kext`.
- Every path is relative and contains no `.` or `..` segment.
- The plist is not empty.
- The executable, when present, is a thin AArch64 `MH_KEXT_BUNDLE`:
  - Magic `0xfeedfacf`.
  - CPU type `0x0100000c`.
  - File type `11`.
  - Subtype `0` or `2` (arm64 or arm64e).
  - Load commands fit inside the file.
- A kit kext names an entry.
- An entry kext does not name an entry.

The staged data is the unmodified plist and executable. The loader validates signatures, resolves dependencies and injects the code.

## Loader protocol

The loader receives:

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

The buffers belong to UniXNU and stay valid for the duration of `Boot()`.

A loader that accepts kexts implements protocol revision 2. The shipped DarwinBoot accepts kernels, the ADT sidecar, the trust cache and the optional ramdisk, and refuses a request with staged kexts.

## Build a kext

Build the bundle with Xcode against the iOS kernel SDK:

```sh
xcodebuild -target MyDriver -sdk iphoneos -configuration Release \
  ARCHS=arm64 ONLY_ACTIVE_ARCH=YES
```

The target uses `MODULE_TYPE = KEXT` and links the kernel frameworks:

| Setting | Value |
| --- | --- |
| `MACH_O_TYPE` | `mh_kext_bundle` |
| `PRODUCT_TYPE` | `com.apple.product-type.kernel-extension` |
| `SDKROOT` | `iphoneos` |
| `ARCHS` | `arm64` or `arm64e` |
| `KERNEL_FRAMEWORKS` | `Kernel` |

The executable must match the kernel ABI of `xnu-10063.141.1` and the iOS 17 deployment target.

## Signing and trust

AMFI validates the kext against the trust cache and the code signature. A kext that is not trusted fails to load with a code signing error.

The trust cache bundle passed to the payload must include the kext CD hash. The payload step accepts the bundle only when its hash is listed in `[payload] trust_cache_sha256`.

## Order and dependencies

UniXNU stages bundles in configuration order. The loader resolves `OSBundleLibraries` and starts the kexts in dependency order.

A codeless kext can sit in the list to express a dependency or to attach personalities to an existing binary.

## Next steps

- [XNU drivers](/drivers/xnu-drivers)
- [Integrity and boot policy](/concepts/security)
- [Boot configuration](/reference/boot-configuration)
