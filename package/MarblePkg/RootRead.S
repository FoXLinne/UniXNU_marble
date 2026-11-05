// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroLib.h>
.text
.balign 64
ASM_FUNC(MarbleRootReadMemory)
  mrs x9, daif
  msr daifset, #15
  mrs x10, vbar_el1
  adrp x11, MarbleRootReadVectors
  add x11, x11, :lo12:MarbleRootReadVectors
  msr vbar_el1, x11
  isb
  mov x8, #1
  cbz x2, MarbleRootReadDone
MarbleRootReadLoop:
  ldrb w4, [x1]
  strb w4, [x0]
  add x1, x1, #1
  add x0, x0, #1
  subs x2, x2, #1
  b.ne MarbleRootReadLoop
MarbleRootReadDone:
  msr vbar_el1, x10
  isb
  msr daif, x9
  mov x0, x8
  ret
.balign 64
ASM_FUNC(MarbleRootReadRegister)
  mrs x9, daif
  msr daifset, #15
  mrs x10, vbar_el1
  adrp x11, MarbleRootReadVectors
  add x11, x11, :lo12:MarbleRootReadVectors
  msr vbar_el1, x11
  isb
  mov x8, #1
  ldr w4, [x1]
  str w4, [x0]
  b MarbleRootReadDone
.balign 2048
MarbleRootReadVectors:
.rept 16
  b MarbleRootReadFault
  .balign 128
.endr
MarbleRootReadFault:
  mrs x16, esr_el1
  str x16, [x3]
  mrs x16, far_el1
  str x16, [x3, #8]
  mov x8, #0
  adr x16, MarbleRootReadDone
  msr elr_el1, x16
  eret
