// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroLib.h>
.text
.balign 64
ASM_FUNC(DarwinResidentSave)
  adrp x10, DarwinResidentContext
  add x10, x10, :lo12:DarwinResidentContext
  mrs x9, ttbr0_el1
  str x9, [x10, #0]
  mrs x9, ttbr1_el1
  str x9, [x10, #8]
  mrs x9, tcr_el1
  str x9, [x10, #16]
  mrs x9, mair_el1
  str x9, [x10, #24]
  mrs x9, sctlr_el1
  str x9, [x10, #32]
  mrs x9, vbar_el1
  str x9, [x10, #40]
  mrs x9, cpacr_el1
  str x9, [x10, #48]
  mrs x9, spsel
  str x9, [x10, #56]
  mrs x9, daif
  str x9, [x10, #64]
  mrs x9, cntv_ctl_el0
  str x9, [x10, #72]
  mrs x9, cntv_cval_el0
  str x9, [x10, #80]
  mrs x9, cntkctl_el1
  str x9, [x10, #88]
  mrs x9, tpidr_el1
  str x9, [x10, #96]
  mrs x9, tpidr_el0
  str x9, [x10, #104]
  mrs x9, tpidrro_el0
  str x9, [x10, #112]
  mrs x9, contextidr_el1
  str x9, [x10, #120]
  mrs x9, mdscr_el1
  str x9, [x10, #128]
  mrs x9, fpcr
  str x9, [x10, #136]
  mrs x9, fpsr
  str x9, [x10, #144]
  mrs x9, icc_pmr_el1
  str x9, [x10, #152]
  mrs x9, icc_igrpen1_el1
  str x9, [x10, #160]
  mrs x9, icc_bpr1_el1
  str x9, [x10, #168]
  mrs x9, icc_ctlr_el1
  str x9, [x10, #368]
  mov x9, sp
  str x9, [x10, #176]
  mrs x9, spsel
  cbz x9, 1f
  mrs x9, sp_el0
  str x9, [x10, #184]
1:
  str x18, [x10, #192]
  str x19, [x10, #200]
  str x20, [x10, #208]
  str x21, [x10, #216]
  str x22, [x10, #224]
  str x23, [x10, #232]
  str x24, [x10, #240]
  str x25, [x10, #248]
  str x26, [x10, #256]
  str x27, [x10, #264]
  str x28, [x10, #272]
  str x29, [x10, #280]
  str x30, [x10, #288]
  str d8, [x10, #304]
  str d9, [x10, #312]
  str d10, [x10, #320]
  str d11, [x10, #328]
  str d12, [x10, #336]
  str d13, [x10, #344]
  str d14, [x10, #352]
  str d15, [x10, #360]
  mov x0, #0
  ret
.balign 64
ASM_FUNC(DarwinResidentReturn)
  msr daifset, #15
  adrp x16, DarwinResidentFaultInfo
  add x16, x16, :lo12:DarwinResidentFaultInfo
  str x0, [x16, #24]
  mrs x17, ttbr1_el1
  str x17, [x16, #112]
  mrs x17, tcr_el1
  str x17, [x16, #120]
  dc cvac, x16
  add x16, x16, #64
  dc cvac, x16
  dsb sy
  .inst 0xd500409f // PAN=0
  .inst 0xd500407f // UAO=0
  mrs x9, sctlr_el1
  bic x9, x9, #1
  bic x9, x9, #4
  bic x9, x9, #0x1000
  msr sctlr_el1, x9
  isb
  adrp x10, DarwinResidentContext
  add x10, x10, :lo12:DarwinResidentContext
  ldr x9, [x10, #0]
  msr ttbr0_el1, x9
  ldr x9, [x10, #8]
  msr ttbr1_el1, x9
  ldr x9, [x10, #16]
  msr tcr_el1, x9
  ldr x9, [x10, #24]
  msr mair_el1, x9
  ldr x9, [x10, #40]
  msr vbar_el1, x9
  ldr x9, [x10, #48]
  msr cpacr_el1, x9
  isb
  tlbi vmalle1
  dsb sy
  ic iallu
  isb
  ldr x9, [x10, #32]
  msr sctlr_el1, x9
  isb
  ldr x9, [x10, #56]
  cbz x9, 2f
  msr spsel, #1
  ldr x9, [x10, #184]
  msr sp_el0, x9
  b 3f
2:
  msr spsel, #0
3:
  ldr x9, [x10, #176]
  mov sp, x9
  ldr x9, [x10, #96]
  msr tpidr_el1, x9
  ldr x9, [x10, #104]
  msr tpidr_el0, x9
  ldr x9, [x10, #112]
  msr tpidrro_el0, x9
  ldr x9, [x10, #120]
  msr contextidr_el1, x9
  ldr x9, [x10, #128]
  msr mdscr_el1, x9
  ldr x9, [x10, #136]
  msr fpcr, x9
  ldr x9, [x10, #144]
  msr fpsr, x9
  msr icc_igrpen1_el1, xzr
  isb
  ldr x9, [x10, #368]
  msr icc_ctlr_el1, x9
  isb
  ldr x9, [x10, #152]
  msr icc_pmr_el1, x9
  ldr x9, [x10, #168]
  msr icc_bpr1_el1, x9
  ldr x9, [x10, #160]
  msr icc_igrpen1_el1, x9
  ldr x9, [x10, #88]
  msr cntkctl_el1, x9
  ldr x9, [x10, #80]
  msr cntv_cval_el0, x9
  ldr x9, [x10, #72]
  msr cntv_ctl_el0, x9
  ldr x18, [x10, #192]
  ldr x19, [x10, #200]
  ldr x20, [x10, #208]
  ldr x21, [x10, #216]
  ldr x22, [x10, #224]
  ldr x23, [x10, #232]
  ldr x24, [x10, #240]
  ldr x25, [x10, #248]
  ldr x26, [x10, #256]
  ldr x27, [x10, #264]
  ldr x28, [x10, #272]
  ldr x29, [x10, #280]
  ldr x30, [x10, #288]
  ldr d8, [x10, #304]
  ldr d9, [x10, #312]
  ldr d10, [x10, #320]
  ldr d11, [x10, #328]
  ldr d12, [x10, #336]
  ldr d13, [x10, #344]
  ldr d14, [x10, #352]
  ldr d15, [x10, #360]
  mov x0, #1
  ret

// The handoff runs with interrupts masked. Recover faults through the saved frame.
.balign 2048
ASM_FUNC(DarwinResidentVectors)
.rept 16
  b DarwinResidentFault
  .balign 128
.endr
ASM_FUNC(DarwinResidentFault)
  msr daifset, #15
  adrp x16, DarwinResidentFaultInfo
  add x16, x16, :lo12:DarwinResidentFaultInfo
  stp x0, x1, [x16, #24]
  stp x2, x3, [x16, #40]
  stp x4, x5, [x16, #56]
  stp x6, x7, [x16, #72]
  stp x29, x30, [x16, #88]
  mov x17, sp
  str x17, [x16, #104]
  mrs x17, ttbr1_el1
  str x17, [x16, #112]
  mrs x17, tcr_el1
  str x17, [x16, #120]
  adrp x10, DarwinResidentFaultInfo
  add x10, x10, :lo12:DarwinResidentFaultInfo
  mrs x9, esr_el1
  str x9, [x10]
  mrs x9, elr_el1
  str x9, [x10, #8]
  mrs x9, far_el1
  str x9, [x10, #16]
  dc cvac, x10
  add x17, x10, #64
  dc cvac, x17
  dsb sy
  b DarwinResidentReturn
