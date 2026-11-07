// SPDX-License-Identifier: BSD-2-Clause-Patent
.section .text.boot, "ax"
.global _start
_start:
  msr daifset, #15
  adr x0, vectors
  msr vbar_el1, x0
  isb
  adr x0, banner
  bl puts
  mrs x0, CurrentEL
  bl hex
  adr x0, granules
  bl puts
  mrs x0, id_aa64mmfr0_el1
  bl hex
  adr x0, frequency
  bl puts
  mrs x0, cntfrq_el0
  bl hex
  adr x0, entering
  bl puts
  ldr x0, =BOOT_ARGS_PA
  ldr x1, =KERNEL_ENTRY_PA
  br x1

exception:
  adr x0, trapped
  bl puts
  mrs x0, esr_el1
  bl hex
  adr x0, faultpc
  bl puts
  mrs x0, elr_el1
  bl hex
  adr x0, faultaddress
  bl puts
  mrs x0, far_el1
  bl hex
  adr x0, complete
  bl puts
1:
  wfe
  b 1b

puts:
  mov x1, #0x09000000
1:
  ldrb w2, [x0], #1
  cbz w2, 3f
2:
  ldr w3, [x1, #0x18]
  tbnz w3, #5, 2b
  str w2, [x1]
  b 1b
3:
  ret

hex:
  mov x1, #0x09000000
  mov x2, #60
  adr x5, digits
1:
  lsr x3, x0, x2
  and x3, x3, #15
  ldrb w4, [x5, x3]
2:
  ldr w6, [x1, #0x18]
  tbnz w6, #5, 2b
  str w4, [x1]
  subs x2, x2, #4
  b.ge 1b
  mov w4, #13
  str w4, [x1]
  mov w4, #10
  str w4, [x1]
  ret

.balign 2048
vectors:
.rept 16
  b exception
  .balign 128
.endr

banner: .asciz "ENTRY-PROBE: CurrentEL=0x"
granules: .asciz "ENTRY-PROBE: MMFR0=0x"
frequency: .asciz "ENTRY-PROBE: CNTFRQ=0x"
entering: .asciz "ENTRY-PROBE: entering unchanged kernel\r\n"
trapped: .asciz "ENTRY-PROBE: ESR_EL1=0x"
faultpc: .asciz "ENTRY-PROBE: ELR_EL1=0x"
faultaddress: .asciz "ENTRY-PROBE: FAR_EL1=0x"
complete: .asciz "ENTRY-PROBE: exception captured\r\n"
digits: .ascii "0123456789abcdef"
