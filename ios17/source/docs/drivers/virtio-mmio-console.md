# VirtioMmioConsole

VirtIO 1.2 MMIO console transport using split queues.

[Source repository](https://github.com/UniXNU/VirtioMmioConsole).

The component transports logs and diagnostic bytes through a compatible MMIO console. Device configuration supplies the MMIO range and interrupt route.

## Integration

The catalog records implementation scope, hardware and OS requirements, board dependencies, revision, build stage, outputs, and runtime identifiers. The selected profile orders build commands by dependency.

```sh
./unixnu build kernel -D <device-repository>
```

[Component catalog](/reference/components)
