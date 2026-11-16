/* SPDX-License-Identifier: BSD-2-Clause-Patent */
ENTRY(_start)
SECTIONS {
  . = 0x40080000;
  .text : { *(.text.boot) *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.comment) *(.note*) }
}
