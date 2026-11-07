_Head:
  adr   x1, _Payload    /* Set _Entry address */
#ifdef SHIM_BASE
  b     _Relocate       /* Keep executing code outside the payload destination */
#else
  b     _Start
#endif

#if REQUIRES_KERNEL_HEADER == 1
_StackBase:
  .quad FD_BASE         /* Text Offset */

_StackSize:
  .quad FD_SIZE         /* Image Size */
  .quad 0               /* Flags */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .ascii "ARM\x64"      /* ARM64 Magic */
  .long 0               /* Reserved */
#endif

#ifdef SHIM_BASE
_Relocate:
  adr   x4, _Head
  movz  x5, #(SHIM_BASE >> 16), lsl #16
  mov   x6, #(_Payload - _Head)
_CopyShim:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _CopyShim
  dsb   sy
  ic    iallu
  isb
  movz  x5, #(SHIM_BASE >> 16), lsl #16
  add   x5, x5, #(_Start - _Head)
  br    x5
#endif

_Start:
  mov   x4, x1
  ldr   x5, =FD_BASE
  ldr   x6,  =FD_SIZE
  add   x8, x4, x6
  bl    _Move
  mov   x4, x8

#ifdef PAYLOAD_BASE
  /* x4 now points just past the FD: an optional "XNUP" payload may follow */
  ldr   w2, [x4]
  movz  w3, #0x4E58
  movk  w3, #0x5055, lsl #16
  cmp   w2, w3
  b.ne  _NoPayload
  ldr   w6, [x4, #4]    /* Payload Size */
  add   x6, x6, #23     /* Header + Round Up to 16 */
  and   x6, x6, #~15
  movz  x5, #(PAYLOAD_BASE >> 16), lsl #16

  bl    _Move

_NoPayload:
#endif
  ldr   x5, =FD_BASE

_Entry:
  br    x5

_Dead:
  b     _Dead

/* Relocation can overlap the decompressed input. Copy toward the free end. */
_Move:
  cmp   x4, x5
  b.eq  _MoveDone
  b.hi  _MoveForward
  add   x7, x4, x6
  cmp   x5, x7
  b.hs  _MoveForward
  add   x4, x4, x6
  add   x5, x5, x6
_MoveBackward:
  ldp   x2, x3, [x4, #-16]!
  stp   x2, x3, [x5, #-16]!
  subs  x6, x6, #16
  b.ne  _MoveBackward
  ret
_MoveForward:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _MoveForward
_MoveDone:
  ret

.text
.align 4

_Payload:
