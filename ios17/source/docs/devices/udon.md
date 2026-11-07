# OnePlus Ace 2 / 11R (udon)

A community device port for SM8475.

- [takasugl/UniXNU-udon](https://github.com/takasugl/UniXNU-udon)
- [takasugl/uefi-udon](https://github.com/takasugl/uefi-udon)

## State

The PHK110 reaches iPadOS 17.7.11 userland through UEFI, DarwinBoot and native UFS. OS/SystemOS and the shared cache load, with a 32 MiB writable APFS RAM Data seed. A device run records 117 processes and 176 seconds of execution before its configured diagnostic reboot.

The visible stage ends at ReportCrash, with signal exits under investigation. The full UI, persistent log retrieval, GPU, audio, Wi-Fi and suspend remain pending.

## Use this port

1. Read the pinned instructions and records below. Prepare the firmware, system inputs and build tools they specify.
2. Clone the device sources and prepare the matching core and firmware checkouts described by its README:

```sh
git clone --branch main https://github.com/takasugl/UniXNU-udon.git
cd UniXNU-udon
git checkout ad550a97f612836181dd36c6c08b1d7e90fb7b14
git submodule update --init --recursive
```

3. Run the build from the working directory specified by that revision's instructions. Check its documented outputs and remaining inputs.
4. Follow the [bring-up order](/porting/bring-up), save [logs](/porting/log-capture) and complete the [porting record](/porting/records). See [porting steps](/porting/steps) for board structure and component selection.

## Sources

- [README: takasugl/UniXNU-udon](https://github.com/takasugl/UniXNU-udon/blob/ad550a97f612836181dd36c6c08b1d7e90fb7b14/README.md)
- [Build guide: takasugl/UniXNU-udon](https://github.com/takasugl/UniXNU-udon/blob/ad550a97f612836181dd36c6c08b1d7e90fb7b14/docs/REPRODUCE.md)
