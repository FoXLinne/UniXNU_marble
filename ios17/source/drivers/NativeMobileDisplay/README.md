# NativeMobileDisplay

Mobile display driver and GPU scanout hand-off. Validates swaps, retires frames and drives the firmware framebuffer.

Current state: the CPU and GPU scanout paths work. The GOP scanout has no vblank interrupt; with `sheng-vsync=<Hz>` a work-loop timer sends the original family's vsync message to the compositor.

Linked into XNU by `unixnu build kernel`.
