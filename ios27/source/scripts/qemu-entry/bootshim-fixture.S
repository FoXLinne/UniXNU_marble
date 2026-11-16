// SPDX-License-Identifier: BSD-2-Clause-Patent
.section .text, "ax"
.global _start
_start:
#ifndef PAYLOAD
  msr daifset, #15
  ldr x0, =0x40010000
  ldr x19, =(SHIM_BASE + SHIM_SIZE)
  ldr x20, =FD_BASE
  ldr x1, =SHIM_BASE
  br x1
#else
  ldr x1, =0x40010000
  cmp x0, x1
  b.ne failed
  adr x1, _start
  cmp x1, x20
  b.ne failed
  mov x1, x19
  mov x2, x20
  ldr x3, =FD_SIZE
1:
  ldp x4, x5, [x1], #16
  ldp x6, x7, [x2], #16
  eor x4, x4, x6
  eor x5, x5, x7
  orr x4, x4, x5
  cbnz x4, failed
  subs x3, x3, #16
  b.ne 1b
  adr x0, passed
  b puts
failed:
  adr x0, failure
puts:
  mov x1, #0x09000000
2:
  ldrb w2, [x0], #1
  cbz w2, done
3:
  ldr w3, [x1, #0x18]
  tbnz w3, #5, 3b
  str w2, [x1]
  b 2b
done:
  wfe
  b done
passed:
  .asciz "BOOTSHIM: full copy, destination entry and x0 PASS\n"
failure:
  .asciz "BOOTSHIM: FAIL\n"
#endif
