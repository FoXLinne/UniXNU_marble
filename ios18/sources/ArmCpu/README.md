# ArmCpu

ARM generic interrupt controller and timer.

starts the CPU, GIC and timer on sheng and on the QEMU virt machine.

`armcpu_spi_attach` registers an exclusive source. `armcpu_spi_share` registers
a level source whose handlers acknowledge their own peripheral status.
Registration is serialized; handlers remain installed for the platform lifetime.
IRQ dispatch reads the interrupt's route mask and visits only its handlers.
`armcpu_spi_mask` masks or enables a registered line from primary IRQ context.

The sheng `scripts/check-bluetooth-shared.py` fixture checks the production
registration, dispatch, masking and concurrent registration paths. The source
also compiles with the selected iPadOS 27 arm64e kernel command.

Build and interface details: [documentation](https://unixnu.com/reference/components). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).
