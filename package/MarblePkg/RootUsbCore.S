// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroLib.h>
.text
.balign 64
ASM_FUNC(MarbleRootUsbCore)
  msr daifset, #15
  msr spsel, #1
  adrp x19, MarbleRootUsbContext
  add x19, x19, :lo12:MarbleRootUsbContext
  ldr x9, [x19, #8]
  msr ttbr0_el1, x9
  ldr x9, [x19, #16]
  msr ttbr1_el1, x9
  ldr x9, [x19, #24]
  msr tcr_el1, x9
  ldr x9, [x19, #32]
  msr mair_el1, x9
  ldr x9, [x19, #40]
  msr vbar_el1, x9
  ldr x9, [x19, #48]
  msr cpacr_el1, x9
  ldr x9, [x19, #80]
  msr tpidr_el1, x9
  ldr x9, [x19, #88]
  msr tpidr_el0, x9
  ldr x9, [x19, #96]
  msr tpidrro_el0, x9
  ldr x18, [x19, #72]
  ldr x9, [x19, #56]
  mov sp, x9
  dsb sy
  isb
  tlbi vmalle1
  dsb sy
  isb
  ldr x9, [x19]
  msr sctlr_el1, x9
  isb
  ldr x9, [x19, #64]
  br x9
