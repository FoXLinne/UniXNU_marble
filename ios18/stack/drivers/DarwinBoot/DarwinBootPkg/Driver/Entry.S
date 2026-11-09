// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroLib.h>

ASM_FUNC(DarwinCurrentEl)
  mrs x0, CurrentEL
  ret

// x0=boot_args PA, x1=entry PA, x2=loaded PA, x3=loaded size, x4=frame buffer
// PA (0 for none), x5=its size in bytes. EL1 only.
ASM_FUNC(DarwinEnter)
  msr daifset, #15
  // The kernel installs its IRQ handler after early VM initialization.
  msr icc_igrpen1_el1, xzr
  msr cntv_ctl_el0, xzr
  dsb sy
  isb
  // Paint the top 1/16 of the framebuffer green (BGRA 0xFF00FF00)
  // before cache maintenance, MMU shutdown and the branch to the kernel.
  cbz x4, 2f
  lsr x9, x5, #4
  bic x9, x9, #7
  mov w10, #0xFF00
  movk w10, #0xFF00, lsl #16
  mov x11, x4
3:
  stp w10, w10, [x11], #8
  subs x9, x9, #8
  b.hi 3b
  dsb sy
2:
  add x5, x2, x3
  mrs x6, ctr_el0
  ubfx x6, x6, #16, #4
  mov x7, #4
  lsl x6, x7, x6
  sub x7, x6, #1
  bic x4, x2, x7
  dsb sy
1:
  dc civac, x4
  add x4, x4, x6
  cmp x4, x5
  b.lo 1b
  dsb sy
  ic iallu
  dsb sy
  isb
  // Preserve the final return frame, matching the marble HackiOS handoff.
  mov x4, sp
  sub x4, x4, #0x400
  bic x4, x4, #63
  add x5, x4, #0x4000
4:
  dc cvac, x4
  add x4, x4, #64
  cmp x4, x5
  b.lo 4b
  dsb sy
  mrs x4, sctlr_el1
  bic x4, x4, #1
  bic x4, x4, #4
  bic x4, x4, #0x1000
  msr sctlr_el1, x4
  isb
  tlbi vmalle1
  dsb sy
  isb
  br x1
