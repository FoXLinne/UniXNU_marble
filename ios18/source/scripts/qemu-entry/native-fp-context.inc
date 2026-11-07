// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real EL0 SIMD preservation across BSD syscalls and blocking Mach traps.
  adrp x26, _fp_context_results@PAGE
  add x26, x26, _fp_context_results@PAGEOFF
  adrp x27, _fp_context_expected@PAGE
  add x27, x27, _fp_context_expected@PAGEOFF
  adrp x28, _fp_context_actual@PAGE
  add x28, x28, _fp_context_actual@PAGEOFF
  mrs x9, cntvct_el0
  str x9, [x26, #56]
  mrs x9, cntfrq_el0
  str x9, [x26, #72]
  mov x20, #0
  ldp q0, q1, [x27, #0]
  ldp q2, q3, [x27, #32]
  ldp q4, q5, [x27, #64]
  ldp q6, q7, [x27, #96]
  ldp q8, q9, [x27, #128]
  ldp q10, q11, [x27, #160]
  ldp q12, q13, [x27, #192]
  ldp q14, q15, [x27, #224]
  ldp q16, q17, [x27, #256]
  ldp q18, q19, [x27, #288]
  ldp q20, q21, [x27, #320]
  ldp q22, q23, [x27, #352]
  ldp q24, q25, [x27, #384]
  ldp q26, q27, [x27, #416]
  ldp q28, q29, [x27, #448]
  ldp q30, q31, [x27, #480]
Lfp_loop:
  mov x21, #1
  mov x16, #20
  svc #0x80
  bl Lfp_compare
  mov x0, #0
  mov x1, #2 // SWITCH_OPTION_WAIT from pinned mach/thread_switch.h
  mov x2, #1 // Actual one-millisecond blocking timeout
  mov x16, #-61 // Pinned Darwin thread_switch trap table index
  mov x21, #2
  svc #0x80
  str x0, [x26, #16]
  cbnz x0, Lfp_trap_failed
  bl Lfp_compare
  add x20, x20, #1
  str x20, [x26, #8]
  cmp x20, #32
  b.lo Lfp_loop
  str xzr, [x26]
  b Lfp_done
Lfp_trap_failed:
  mov x9, #2
  str x9, [x26]
  b Lfp_done
Lfp_mismatch:
  mov x9, #1
  str x9, [x26]
  str x21, [x26, #24]
  str x10, [x26, #32]
  str x11, [x26, #40]
  str x12, [x26, #48]
  b Lfp_done
Lfp_compare:
  stp q0, q1, [x28, #0]
  stp q2, q3, [x28, #32]
  stp q4, q5, [x28, #64]
  stp q6, q7, [x28, #96]
  stp q8, q9, [x28, #128]
  stp q10, q11, [x28, #160]
  stp q12, q13, [x28, #192]
  stp q14, q15, [x28, #224]
  stp q16, q17, [x28, #256]
  stp q18, q19, [x28, #288]
  stp q20, q21, [x28, #320]
  stp q22, q23, [x28, #352]
  stp q24, q25, [x28, #384]
  stp q26, q27, [x28, #416]
  stp q28, q29, [x28, #448]
  stp q30, q31, [x28, #480]
  mov x10, #0
Lfp_compare_word:
  ldr x11, [x27, x10, lsl #3]
  ldr x12, [x28, x10, lsl #3]
  cmp x11, x12
  b.ne Lfp_mismatch
  add x10, x10, #1
  cmp x10, #64
  b.lo Lfp_compare_word
  ret
Lfp_done:
  mrs x9, cntvct_el0
  str x9, [x26, #64]
  b _checkpoint
.data
.balign 16
.global _fp_context_results,_fp_context_expected,_fp_context_actual
_fp_context_results:
  .quad 0xaaaaaaaa,0,0,0,0,0,0,0,0,0
_fp_context_expected:
  .quad 0x3ff0000000000000, 0x4000000000000000
  .quad 0x3ff0000000000001, 0x4000000000000001
  .quad 0x3ff0000000000002, 0x4000000000000002
  .quad 0x3ff0000000000003, 0x4000000000000003
  .quad 0x3ff0000000000004, 0x4000000000000004
  .quad 0x3ff0000000000005, 0x4000000000000005
  .quad 0x3ff0000000000006, 0x4000000000000006
  .quad 0x3ff0000000000007, 0x4000000000000007
  .quad 0x3ff0000000000008, 0x4000000000000008
  .quad 0x3ff0000000000009, 0x4000000000000009
  .quad 0x3ff000000000000a, 0x400000000000000a
  .quad 0x3ff000000000000b, 0x400000000000000b
  .quad 0x3ff000000000000c, 0x400000000000000c
  .quad 0x3ff000000000000d, 0x400000000000000d
  .quad 0x3ff000000000000e, 0x400000000000000e
  .quad 0x3ff000000000000f, 0x400000000000000f
  .quad 0x3ff0000000000010, 0x4000000000000010
  .quad 0x3ff0000000000011, 0x4000000000000011
  .quad 0x3ff0000000000012, 0x4000000000000012
  .quad 0x3ff0000000000013, 0x4000000000000013
  .quad 0x3ff0000000000014, 0x4000000000000014
  .quad 0x3ff0000000000015, 0x4000000000000015
  .quad 0x3ff0000000000016, 0x4000000000000016
  .quad 0x3ff0000000000017, 0x4000000000000017
  .quad 0x3ff0000000000018, 0x4000000000000018
  .quad 0x3ff0000000000019, 0x4000000000000019
  .quad 0x3ff000000000001a, 0x400000000000001a
  .quad 0x3ff000000000001b, 0x400000000000001b
  .quad 0x3ff000000000001c, 0x400000000000001c
  .quad 0x3ff000000000001d, 0x400000000000001d
  .quad 0x3ff000000000001e, 0x400000000000001e
  .quad 0x3ff000000000001f, 0x400000000000001f
_fp_context_actual:
  .space 512
.text
