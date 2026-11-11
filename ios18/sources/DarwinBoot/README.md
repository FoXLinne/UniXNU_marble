# DarwinBoot

UEFI XNU loader with packed RAM disk handoff support.

The Mach-O parser accepts arm64 and arm64e MH_EXECUTE kernels, including
the capability bits in the arm64e CPU subtype. XNU initializes pointer
authentication and applies its own chained fixups. MH_FILESET input is
still unsupported by this loader.

UniXNU passes the selected boot entry's arguments to DarwinBoot, which copies
them into XNU's command line and prints `DARWINBOOT: command line:` before
handoff. The top green bar marks `DarwinEnter` before cache maintenance,
MMU shutdown and the branch to XNU. Follow the
[boot argument workflow](https://unixnu.com/xnu/boot-arguments#apply-arguments)
and [green-bar diagnosis](https://unixnu.com/guide/debugging#green-bar).

The memory handed to XNU is one window of `/chosen/darwinboot-memory-size` bytes (256 MiB by default). Without
further properties it is a single run of free memory. With `/chosen/darwinboot-memory-excluded`, a property of
empty 16-byte pairs, the window may span gaps between DRAM banks and memory the firmware keeps: the loader picks
the start with the most usable memory, allocates only the head (the loaded image plus the larger of 1/64 of the
window and 32 MiB for the tables XNU places behind it), and writes the ranges XNU must not use into the property
as ascending `(base, size)` pairs from the memory map that `ExitBootServices` accepts. Free and loader memory are
usable; boot services memory stays excluded. `boot_args.memSize` is the window and `memSizeActual` the usable
bytes. If the window has more excluded ranges than the property holds, neighbours with the least usable memory
between them are joined. The kernel must read the property: only an OS profile whose XNU carries that support
may request it. Before the hand-off every usable range leaves the data cache, as the single-run window does, so
the firmware must keep free memory mapped; excluded ranges are never touched. With 4 KiB pages XNU's bootstrap
tables map at most 6 GiB, which bounds the window. The excluded-range hand-off has not run on hardware.

With `/chosen/darwinboot-loader-inputs`, two empty 32-byte records, the loader records where its own inputs lie
in the firmware's memory map: the payload RAM disk, which is the range the first-stage boot loader's device tree
names as the initrd (found through the entry argument HOB of the firmware's SEC phase, and verified when the boot
volume's block device has that size and reads its first and last sector from it), and the boot entry's initrd
buffer. A record holds base, size, UEFI memory type (-1: none or mixed), state bits (1 located, 2 verified,
4 inside the window, 8 usable) and usable bytes; the loader prints both as `DARWINBOOT: payload disk` and
`DARWINBOOT: initrd buffer`. Both inputs are read for the last time before `ExitBootServices` and XNU is told
neither address, so with the excluded-range table the whole pages of a verified input that lie inside the window
count as usable memory when boot services data, reserved memory or a usable type describes them. Other types and
unverified ranges stay excluded. Without the property the loader does not look for its inputs. Not run on
hardware.

Host checks: `Test/test-memory-window.py` (window selection and the table on synthetic memory maps),
`Test/test-loader-inputs.py` (the HOB and device tree readers, the input records and their release into the
table), `Test/test-adt-window.py` (the device tree properties), `Test/test-kernel-parser.py`,
`Test/test-packed-ramdisk.py` and `Test/window-size.c`.

Build and interface details: [documentation](https://unixnu.com/reference/components).
Sources and dependencies: [catalog](https://unixnu.com/reference/components).

`ROOT_STAGE_SERVICE_GUID` supplies resident transport enter and resume hooks.
DarwinBoot invokes enter after firmware memory writeback and invokes resume
before firmware services resume. The board reserves the service CPU outside
the OS CPU topology and retains controller ownership and USB descriptors.

A RootStage table with `WaitForHost == 0` supplies disk and kernel memory
handoff data for a normal boot. DarwinBoot exits boot services and enters
the kernel; resident return hooks are used when `WaitForHost` is populated.

The optional `kernel.macho.vroot` sidecar carries up to 128 MiB of signed root
policies. The loader checks its envelope and supplies its physical range as
`/chosen/memory-map/RootPolicies`. XNU authenticates the policies using an owner
anchor compiled into the source storage runtime. This handoff is independent
of the Data RAM disk, GPU addon and Apple trust cache. The new handoff has not
yet been built or exercised.
