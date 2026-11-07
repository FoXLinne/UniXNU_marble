# NativePrelink

Kernel prelink tooling. Reads a Mach-O kernel and emits the prelinked image and boot metadata.

Current state: used by the kernel build and the sheng payload step.

Run by `unixnu build kernel` and `unixnu build payload`.
