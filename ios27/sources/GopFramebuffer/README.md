# GopFramebuffer

UEFI GOP framebuffer and MobileFramebuffer integration.

CPU and GPU scanout are supported. With `vsync=<Hz>`, a work-loop timer sends the MobileFramebuffer vsync message to the compositor.

Swaps carry one full-screen surface: 8-bit BGRA, plain or in the one-plane compressed spelling, or half-float RGBA. With `dpu-scanout=2` the display pipes fetch an 8-bit surface themselves. A half-float surface cannot be fetched: the GPU client converts it into the scanout memory (`fb-gpu=1`), or the CPU does when no client takes it. `fb-half=2` reads half-float values as already encoded. A render server that takes the device for a virtual machine presents half float. `native_legacy_formats` counts the swaps of each format, the swaps refused for their format and the frames on each path.

Direct display keeps the first successful GPU fence snapshot for each swap. Busy or unavailable queries retry after 200 microseconds. Reaching 250 milliseconds records a slow wait; the current frame stays on screen until the target completes or the display family cancels the swap. Cancellation clears the pending fence timer and snapshot.

Run the fence-wait regression check with `python3 Test/check-fence-wait.py`. It compiles the display's fence-wait and cancellation methods against a simulated GPU with AddressSanitizer and UndefinedBehaviorSanitizer.

Build and interface details: [documentation](https://unixnu.com/drivers/gop-framebuffer). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).

## Frame diagnostics

`gpu-trace=1` records display arrival, fence and latch in the kernel GPU ring. Repeated pending display queries are coalesced. See [GPU frame diagnostics](https://unixnu.com/porting/gpu-diagnostics) for fields and decoding.

Display traces obtain physical addresses from prepared direct-display slots. Validation records use address zero until a slot is mapped; the flip record supplies the mapped address. Observers do not prepare memory or query unwired descriptors.

The macOS display attributes, the timing and color elements and the hot plug state follow the display driver of [macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM), the DroidVM port booted with UniXNU.
