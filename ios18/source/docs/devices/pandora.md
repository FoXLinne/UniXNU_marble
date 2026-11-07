# Xiaomi 17 Pro (pandora)

A community-maintained port for SM8850. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [data-a-live](https://github.com/ShueiTsuki/data-a-live) | [ShueiTsuki](https://github.com/ShueiTsuki) |

## State

The device reaches the iOS 16.7 home screen and runs apps with touch, power/volume keys and brightness control. All eight CPUs run, with load-based frequency changes, idle sleep and thermal throttling recorded.

The maintainer records GPU composition of the system UI through Adreno 840, Turnip Vulkan and the Metal translation layer. The panel scans out the rendered buffers directly; the reported animation rate is about 110 frames/s on a 120 Hz panel. Audio DSP startup and communication work; speaker playback, real battery reporting, Wi-Fi, cellular and camera support remain pending.

## Sources

- [README: ShueiTsuki/data-a-live](https://github.com/ShueiTsuki/data-a-live/blob/91f1f2468f30841c39123c7ba20ae520ecd82c84/README.md)

## Build

The kernel is built by the `xnu.yml` GitHub Actions workflow with `machine_config=PANDORA`. The scripts in the repository build the ADT and the payload in WSL and the loader with the Android NDK:

```sh
python3 tools/mkprelink.py <kernel> <kernelcache> <out>
KERNEL=<out> tools/boot_ios.sh --file /data/local/tmp/iosroot.img --run <N> --rd ufsroot0
```

`tools/boot_ios.sh` resolves the extents of the pinned root file, builds the payload and the loader image, and starts it with `fastboot boot`. The kernel, the kernelcache, the root image and the trust caches are external inputs and are not stored in the repository.

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
