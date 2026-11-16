# ShengUfs

SM8550 UFS transport and sheng persistent storage.

The historical native UFS root qualification applies to the iOS 17 path.

Build and interface details: [documentation](https://unixnu.com/drivers/sheng-ufs). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).

UniXNU's `scripts/build-sheng-source-drivers.py --with-storage-family`
compiles this driver and public IOStorageFamily 337.100.1 against XNU 12377
for arm64e iOS 26. Verified-root read policy remains enabled. These archives
still require family/APFS linkage and device qualification; they do not establish
an iPadOS 26 root boot. See the [migration procedure](https://unixnu.com/porting/migration#source-drivers).

The arm64e `build-arm-driver.py ShengUfs` entry now selects
`build-sheng-source-storage.py`. It builds a deferred public-source
IOBlockStorageDevice adapter and the board's SourceStorage runtime. The runtime
loads the original IOStorageFamily and APFS, checks the original base class
size, authenticates the 23F84 policies, registers the source classes, and starts
NativeUfs on the platform root. Its runtime metadata has a separate name and
inherits the original storage metaclass; its C++ class name is preserved for
compiler-generated arm64e pointer authentication.

`native-source-storage=1 native-ufs-root=1` selects this path. The root policy
tables travel in `kernel.macho.vroot`; only the owner public anchor is linked
into the kernel. On sheng it serves the iPadOS 27.0.1 (24A446) root and Data.

## Command path

Up to 32 commands run at once, one per command slot. The request list sits on
a write-combined page of its own, so slot s uses list entry s; without that page
16 slots use the even entries of the DMA region, so no two entries share a cache
line on this non-coherent stream. Region A holds slots 0 to 15 (slot 0 keeps the
original layout), region B slots 16 to 31. The last slot serves the panic path;
a full pool hands a freed slot to its oldest waiter.

Completion uses the controller interrupt (GIC SPI 265) with UTRCE and the UTRD
interrupt bit, as Linux ufshcd. The handler wakes the waiters of the commands
whose doorbell bits cleared. A wait polls for its first 200 microseconds
without blocking, one WFE between two polls, and sleeps on the interrupt after
that or as soon as a preemption is pending on its CPU. Bring-up and the panic
path poll with `IODelay`.

| Boot argument | Effect |
| --- | --- |
| `native-ufs-wait=1` | Default: spin for the budget, then sleep |
| `native-ufs-wait=0` | Pause with `IODelay(2)` for the budget, then sleep (the earlier behaviour) |
| `native-ufs-wait=2` | Sleep on the interrupt from the first pause |
| `native-ufs-spin-us=N` | Spin budget in microseconds (default 200) |

A command takes about 100 us on sheng, and a blocked thread takes 30 to 37 us
on average to run again after a timer or the completion interrupt. `IODelay(2)`
blocks on a timer on these cores (they register no idle exit latency), so the
paced mode blocks two to three times per command.

Page-aligned buffers are read and written by DMA straight into their pages
through a PRDT of up to 64 entries and 1 MiB per command; other buffers use the
slot's 128 KiB data area. Data is cleaned to memory before the doorbell; after
the command only data the device wrote is invalidated, so the source of a write
stays as the CPU has it. A command answered with BUSY or TASK SET FULL is sent
again after a pause. Root window reads each take their own bounce buffer.

A window device is read-only and reports a solid state medium (`Device
Characteristics`, `Medium Type`), which XNU reads through DKIOCISSOLIDSTATE for
its I/O throttle periods, read-ahead and page-in cluster sizes.

## Verified-root tables

Every window block is checked against its SHA-256 in the owner-signed table
before it is served. A header-only policy keeps its table in the root image
object `<window>.vroot`. At bring-up the whole table is read once and checked
against the digest in the signed header; only a SHA-256 of each 4096-byte table
page stays in memory, read-only (32 bytes per 512 KiB of image). A block's table
page then comes from a wired cache shared by all windows, or is read from the
object on LUN 0 through the transport (never through a window), checked against
its kept page hash and cached. A table page that does not match is not cached and
every block it covers is refused. The cache is allocated before the windows are
published, so a page-in under memory pressure fills it without allocating.

| Boot argument | Effect |
| --- | --- |
| `native-vroot-cache=N` | Table page cache in MiB (default 8: 2048 pages, 1 GiB of image) |
| `native-vroot-cache=0` | Keep every window's whole table wired, no cache |

`native_ufs_dme_get` reads UniPro attributes. The sheng link runs HS-G5 Rate A
on two lanes in FAST mode, as the boot firmware leaves it.

## Statistics

`native_ufs_stats_text` fills one row for the board's log:

| Field | Meaning |
| --- | --- |
| `direct`, `staged` | Data commands on the caller's pages, and through the slot's data area |
| `busy` | Commands answered with BUSY or TASK SET FULL and sent again |
| `long k/n` | The longest store call: `r` read, `w` write, `s` cache flush, and its blocks |
| `pauses n/us` | `IODelay(2)` poll pauses of completion waits and their total time |
| `spins n/us` | Spinning poll pauses (one WFE each) and their total time |
| `yields` | Spins that ended in a sleep because a preemption was pending |
| `done a/b/c/d/e` | Waits whose command completed under 50, 100, 200, 400 us, and later |
| `old ms/op/slot` | The oldest command in flight now: age, opcode (hex), slot; 0 when none |
| `takers` | Threads waiting for a free command slot now |
| `poison p/r/op` | 1 after a timeout or fatal status (later commands are refused), with its IOReturn and opcode |
| `irqs`, `sleeps` | Completion interrupts, and waits that slept for one |
| `wakes n/us`, `max` | Sleeps the interrupt ended, total and longest time until the waiter ran |
| `timeouts` | Sleeps that ended on their 1 ms timeout |
| `win n/MiB/ms` | Root window requests, their bytes and their time |
| `wait n/ms` | Root window requests that waited for a bounce buffer |
| `vb` | Integrity blocks verified |
| `vt h/m/r/e/w/f` | Table cache hits, misses, table page reads, evictions, waits for a page or entry being filled, refused table pages |
| `vtc n/KiB` | Table cache pages (0 with `native-vroot-cache=0`) and wired verified-root memory in KiB |

A paced pause is `IODelay(2)`. XNU spins for it only on a CPU registered with
an idle exit latency above the delay; otherwise the thread blocks on a timer,
and `pauses` shows the cost. Every command wait ends by its deadline (1 s read,
2 s write, 10 s cache flush); one that runs out poisons the host.

## Tests

`tests/CompletionWait.cpp` drives the completion wait policy through the
production poll loop. `tests/RootCommands.cpp` runs the production command code against a host model:
per-slot entries, descriptors, tags and data, the pool, busy retries, the panic
slot and the one-slot fallback. `tests/check_arm64_root_commands.py` runs it on
QEMU's UFS controller at EL1 with real cache maintenance, including eight
outstanding commands and a 512 KiB zero-copy read.
`tests/VerifiedTableCache.cpp` checks the table cache against `vr_verify_block`
on the whole table, refusal of tampered and unreadable table pages, LRU eviction
and concurrent fills; `tests/check_root_path_host.py --verified-root
<UniXNU>/kits/VerifiedRoot` builds and runs it.

## Wi-Fi DMA window

The optional WCN7850 window maps 128 MiB at IOVA 0x10000000 using 64 two-MiB
blocks in its own SMMU context bank. The Wi-Fi allocator reports current use,
peak use and failures. sheng device checks reach interface readiness and
network traffic with zero DMA allocation failures.

The consuming kernel build compiles the driver. The board's
`scripts/check-wifi-dma.py` checks window capacity and allocation boundaries.
