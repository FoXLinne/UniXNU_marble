// Offline file probe: genuine open, F_ADDFILESIGS_RETURN, mmap and page faults.
  adrp x20, _file_probe_results@PAGE
  add x20, x20, _file_probe_results@PAGEOFF
  adrp x0, Lprobe_path@PAGE
  add x0, x0, Lprobe_path@PAGEOFF
  mov x1, #0 // O_RDONLY
  mov x2, #0
  mov x16, #5
  svc #0x80
  str x0, [x20]
  mrs x9, nzcv
  str x9, [x20, #8]
  b.cs _checkpoint
  mov x21, x0
  mov x8, #1
  str x8, [x20, #72]
  mov x1, #97 // Darwin 23 F_ADDFILESIGS_RETURN
  adrp x2, _file_signature@PAGE
  add x2, x2, _file_signature@PAGEOFF
  mov x16, #92
  svc #0x80
  str x0, [x20, #16]
  mrs x9, nzcv
  str x9, [x20, #24]
  b.cs _checkpoint
  adrp x8, _file_signature@PAGE
  ldr x8, [x8, _file_signature@PAGEOFF]
  str x8, [x20, #64]
  mov x8, #2
  str x8, [x20, #72]
  mov x0, #0
  mov x1, #NATIVE_PROBE_LIMIT
  mov x2, #5 // PROT_READ | PROT_EXEC
  mov x3, #2 // MAP_PRIVATE
  mov x4, x21
  mov x5, #0
  mov x16, #197
  svc #0x80
  str x0, [x20, #32]
  mrs x9, nzcv
  str x9, [x20, #40]
  b.cs _checkpoint
  mov x22, x0
  mov x8, #3
  str x8, [x20, #72]
  mov x9, #0
  mov x10, #0
  mov x11, #0
  mov x12, #NATIVE_PROBE_LIMIT
Lprobe_fault:
  ldr x8, [x22, x9]
  eor x10, x10, x8
  add x11, x11, #1
  add x9, x9, #1, lsl #12
  cmp x9, x12
  b.lo Lprobe_fault
  str x11, [x20, #48]
  str x10, [x20, #56]
  mov x8, #4
  str x8, [x20, #72]
  b _checkpoint
.data
.balign 8
.global _file_probe_results, _file_signature
_file_probe_results:
  .fill 10, 8, 0
_file_signature:
  .quad 0, NATIVE_PROBE_SIGNATURE_OFFSET, NATIVE_PROBE_SIGNATURE_SIZE
Lprobe_path:
#ifdef NATIVE_APFS_FILE_PROBE
  .asciz "/dev/apfs-offline/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.02"
#else
  .asciz "/probe.bin"
#endif
.text
