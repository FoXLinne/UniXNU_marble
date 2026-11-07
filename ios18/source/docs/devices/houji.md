# Xiaomi 14 (houji)

A community device port for SM8650.

- [lonzzi/UniXNU-houji](https://github.com/lonzzi/UniXNU-houji)

## State

The device reaches the iOS 16.7.16 desktop on eight CPUs through native UFS and an HFSX root. Hardware records confirm full 1200x2670 display coverage, aligned touch and recent-app cards. Display composition uses the CPU. The About page can leave the interface unresponsive while kernel logging continues.

Adreno 750 diagnostics verify GMU/HFI startup, memory permissions, allocation responses, HWSCHED start and power/microcode preparation. GPU command submission, fences and Metal UI acceleration remain pending. Recovery log capture and run identification are documented.

## Use this port

1. Read the pinned instructions and records below. Prepare the firmware, system inputs and build tools they specify.
2. Clone the device sources and prepare the matching core and firmware checkouts described by its README:

```sh
git clone --branch main https://github.com/lonzzi/UniXNU-houji.git
cd UniXNU-houji
git checkout d4fb1472c86210303e36f311c07411dd486a753f
git submodule update --init --recursive
```

3. Run the build from the working directory specified by that revision's instructions. Check its documented outputs and remaining inputs.
4. Follow the [bring-up order](/porting/bring-up), save [logs](/porting/log-capture) and complete the [porting record](/porting/records). See [porting steps](/porting/steps) for board structure and component selection.

## Sources

- [README: lonzzi/UniXNU-houji](https://github.com/lonzzi/UniXNU-houji/blob/d4fb1472c86210303e36f311c07411dd486a753f/README.md)
- [Hardware record: lonzzi/UniXNU-houji](https://github.com/lonzzi/UniXNU-houji/blob/d4fb1472c86210303e36f311c07411dd486a753f/docs/bringup.md)
