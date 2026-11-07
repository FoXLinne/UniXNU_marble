# Redmi K90 Pro Max / Poco F8 Ultra (myron)

myron is the Redmi K90 Pro Max and Poco F8 Ultra with the SM8850 SoC. The project below is an independent community effort. The UniXNU organization does not maintain it.

| Repository | Maintainer | Contents |
| --- | --- | --- |
| [UniXNU-myron](https://github.com/zg7681/UniXNU-myron) | [zg7681](https://github.com/zg7681) | Core fork; the `myron-next` branch carries the A8XX Adreno and CPU affinity changes |
| [NativeAdreno-myron](https://github.com/zg7681/NativeAdreno-myron) | [zg7681](https://github.com/zg7681) | Adreno driver fork with the A8XX register table, continuation thread and user-thread selection |
| [NativeUfs-myron](https://github.com/zg7681/NativeUfs-myron) | [zg7681](https://github.com/zg7681) | UFS driver fork |

## State

The repositories are core and driver forks. No device repository, board file, defconfig or hardware result is published yet. The `myron-next` branch carries the A8XX Adreno register table and the CPU affinity matching change. Treat the port as work in progress until a device repository and hardware log exist.

## Maintenance

The maintainer owns the repositories and their release decisions. UniXNU documentation lists the project and its state; it does not take over maintenance.
