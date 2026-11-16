# ArmCpu

ARM generic interrupt controller and timer.

starts the CPU, GIC and timer on sheng and on the QEMU virt machine.

`armcpu_spi_attach` registers an exclusive source. `armcpu_spi_share` registers
a level source whose handlers acknowledge their own peripheral status.
Registration is serialized; handlers remain installed for the platform lifetime.
IRQ dispatch reads the interrupt's route mask and visits only its handlers.
`armcpu_spi_mask` masks or enables a registered line from primary IRQ context.

Cross-core signals are GIC SGI 0, or with `armcpu-spi-ipi=1` software-pended
SPIs 1008 to 1015 (one per logical core). A deferred signal goes out at once
and a cancel takes nothing back. Idle is the kernel's WFI; no PSCI
CPU_SUSPEND state is entered. `armcpu-edge=1` calls `ml_cpu_init_completed`
after the last core started, which lets the Edge scheduler build its cluster
matrix; `armcpu_cpu_init_completed` reports it.

`armcpu_stats_row(index, row, size)` formats wake and idle statistics for a
board log. Row 0 is always present: IPIs sent and taken, deferred and cancel
requests, the scheduler state and the firmware's PSCI answers. With `diag=1`
the driver registers the kernel's idle callback, times every IPI and asks the
firmware for PSCI_VERSION and PSCI_FEATURES once; one row per core then gives
its idle periods by length (under 100 us, under 1 ms, under 8 ms, longer) and
the time in each, what ended them (timer, IPI, device interrupt, no interrupt),
the cost of a send, and the delay from a send to the target's handler (under
50 us, under 200 us, under 1 ms, longer) for an awake and for an idle target.
The start-up IPI a core sends itself (`armcpu-self-ipi=1`) is counted as sent
and taken but is in neither the send cost nor the delays.
After the core rows comes one row per core with its timer interrupts by the
delay after their deadline (same ranges), again awake and out of idle. The
timer is one per core and one-shot; the delay out of idle is the time the
core takes to leave idle.

`armcpu-idle-latency-us=N` registers every core with that time as its power
gate latency (default 0: none). The kernel then spins through a delay shorter
than N instead of blocking the thread on a timer, sets an idle core's timer N
ahead of the deadline, and keeps a core out of WFI for a deadline closer than
N. Row 0 shows the value.

The sheng `scripts/check-bluetooth-shared.py` fixture checks the production
registration, dispatch, masking and concurrent registration paths. The source
also compiles with the selected iPadOS 27 arm64e kernel command.

Build and interface details: [documentation](https://unixnu.com/reference/components). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).
