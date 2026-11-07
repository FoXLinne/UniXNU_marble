// Offline component instrument: two real read-only data mounts and ordinary
// chroot/exec. The boot root stays the own mockfs; root authentication is intact.
  adrp x20, _apfs_launchd_results@PAGE
  add x20, x20, _apfs_launchd_results@PAGEOFF
  adrp x0, Los_mountpoint@PAGE
  add x0, x0, Los_mountpoint@PAGEOFF
  mov x1, #0755
  mov x16, #136
  svc #0x80
  str x0, [x20]
  mrs x9, nzcv
  str x9, [x20, #8]
  b.cs _checkpoint
  mov x8, #1
  str x8, [x20, #96]
  adrp x1, Los_mountpoint@PAGE
  add x1, x1, Los_mountpoint@PAGEOFF
  adrp x3, Los_arguments@PAGE
  add x3, x3, Los_arguments@PAGEOFF
  bl Lcomponent_mount
  str x0, [x20, #16]
  mrs x9, nzcv
  str x9, [x20, #24]
  b.cs _checkpoint
  mov x8, #2
  str x8, [x20, #96]
  adrp x1, Lcache_mountpoint@PAGE
  add x1, x1, Lcache_mountpoint@PAGEOFF
  adrp x3, Lcache_arguments@PAGE
  add x3, x3, Lcache_arguments@PAGEOFF
  bl Lcomponent_mount
  str x0, [x20, #32]
  mrs x9, nzcv
  str x9, [x20, #40]
  b.cs _checkpoint
  mov x8, #3
  str x8, [x20, #96]
  adrp x0, Los_mountpoint@PAGE
  add x0, x0, Los_mountpoint@PAGEOFF
  mov x16, #61 // chroot
  svc #0x80
  str x0, [x20, #48]
  mrs x9, nzcv
  str x9, [x20, #56]
  b.cs _checkpoint
  mov x8, #4
  str x8, [x20, #96]
  adrp x0, Lroot_directory@PAGE
  add x0, x0, Lroot_directory@PAGEOFF
  mov x16, #12 // chdir
  svc #0x80
  str x0, [x20, #64]
  mrs x9, nzcv
  str x9, [x20, #72]
  b.cs _checkpoint
  mov x8, #5
  str x8, [x20, #96]
#ifdef NATIVE_APFS_STOP_BEFORE_EXEC
  b _checkpoint
#else
  adrp x0, Loriginal_launchd@PAGE
  add x0, x0, Loriginal_launchd@PAGEOFF
  adrp x1, Llaunchd_argv@PAGE
  add x1, x1, Llaunchd_argv@PAGEOFF
  adrp x2, Llaunchd_envp@PAGE
  add x2, x2, Llaunchd_envp@PAGEOFF
  mov x16, #59 // execve: original launchd and original /usr/lib/dyld on APFS
  svc #0x80
  str x0, [x20, #80]
  mrs x9, nzcv
  str x9, [x20, #88]
  b _checkpoint
#endif
Lcomponent_mount:
  adrp x0, Lcomponent_apfs_name@PAGE
  add x0, x0, Lcomponent_apfs_name@PAGEOFF
  mov x2, #1 // MNT_RDONLY
  mov x16, #167
  svc #0x80
  ret
.data
.balign 8
.global _apfs_launchd_results
_apfs_launchd_results:
  .fill 13, 8, 0
Lcomponent_apfs_name:
  .asciz "apfs"
Los_mountpoint:
  .asciz "/dev/osroot"
Lcache_mountpoint:
  .asciz "/dev/osroot/dev"
Los_device:
  .asciz NATIVE_APFS_OS_DEVICE
Lcache_device:
  .asciz NATIVE_APFS_CACHE_DEVICE
Lroot_directory:
  .asciz "/"
Loriginal_launchd:
  .asciz "/sbin/launchd"
Lcache_environment:
  .asciz "DYLD_SHARED_CACHE_DIR=/dev/System/Library/Caches/com.apple.dyld"
.balign 8
Llaunchd_argv:
  .quad Loriginal_launchd, 0
Llaunchd_envp:
  .quad Lcache_environment, 0
Los_arguments:
  .quad Los_device, 1
  .short 1
  .fill 0x144 - 18, 1, 0
  .word 99, 99, 0
Lcache_arguments:
  .quad Lcache_device, 1
  .short 1
  .fill 0x144 - 18, 1, 0
  .word 99, 99, 0
.text
