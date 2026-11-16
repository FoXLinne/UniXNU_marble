# 24B119 IOAV adapter

`package/ios27-ioav/build.py` builds four original function adapters against the target kernel compile records.
The build checks the 24B119 kernelcache SHA-256, module ownership, original function boundary and BTI instruction,
then decodes each compiled stub and checks the full 64-bit destination. Function prefixes and the archive digest
are recorded in `ioav.json`.

| Helper | Original 24B119 address | Evidence |
| --- | --- | --- |
| timing withData | `0xfffffff0097ad798` | metaclass allocation, three-argument initializer, failure release |
| timing setColorElements | `0xfffffff0097aec88` | slot28, diversity0x828c, retain/swap/release/notify |
| color withCapabilities | `0xfffffff0097acb18` | seven capability scalars packed into the worker input |
| framebuffer hotPlug_notify | `0xfffffff009a7a4d4` | slot278, diversity0x786c, original state notification |

Build command from the workspace:

```sh
tools/marble-build/venv/bin/python work/UniXNU-marble/package/ios27-ioav/build.py
```

The default iOS27 display selects `NATIVE_LEGACY_MACOS=0` and `NATIVE_LEGACY_DEFAULT_SURFACE=1`.
This module provides optional cross-system mode enumeration and hotplug integration; its file existence
has no effect on the OS selected by the driver build. Device results remain pending.
