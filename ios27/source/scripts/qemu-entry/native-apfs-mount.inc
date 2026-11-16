// Own offline process: create a real devfs directory and request a data mount.
// Root remains the explicit RAM fixture. No root-authentication policy changes.
  adrp x20, _apfs_mount_results@PAGE
  add x20, x20, _apfs_mount_results@PAGEOFF
  adrp x0, Lapfs_mountpoint@PAGE
  add x0, x0, Lapfs_mountpoint@PAGEOFF
  mov x1, #0755
  mov x16, #136 // Darwin 23 mkdir
  svc #0x80
  str x0, [x20]
  mrs x9, nzcv
  str x9, [x20, #8]
  b.cs _checkpoint
  mov x8, #1
  str x8, [x20, #32]
  adrp x0, Lapfs_name@PAGE
  add x0, x0, Lapfs_name@PAGEOFF
  adrp x1, Lapfs_mountpoint@PAGE
  add x1, x1, Lapfs_mountpoint@PAGEOFF
  mov x2, #1 // MNT_RDONLY
  adrp x3, Lapfs_arguments@PAGE
  add x3, x3, Lapfs_arguments@PAGEOFF
  mov x16, #167 // Darwin 23 mount
  svc #0x80
  str x0, [x20, #16]
  mrs x9, nzcv
  str x9, [x20, #24]
  b.cs _checkpoint
  mov x8, #2
  str x8, [x20, #32]
  b Lapfs_mounted
.data
.balign 8
.global _apfs_mount_results
_apfs_mount_results:
  .fill 5, 8, 0
Lapfs_name:
  .asciz "apfs"
Lapfs_mountpoint:
  .asciz "/dev/apfs-offline"
Lapfs_device:
  .asciz NATIVE_APFS_DEVICE
.balign 8
Lapfs_arguments:
  // Fields are derived from the supplied mount_apfs; reserve 0x150 bytes:
  // fspec at 0, MNT_RDONLY at 8, ordinary data-mount mode 1 at 0x10,
  // and explicit uid/gid 99 at 0x144/0x148. Mode 0 requests a boot root.
  .quad Lapfs_device
  .quad 1
  .short NATIVE_APFS_MOUNT_MODE
  .fill 0x144 - 18, 1, 0
  .word 99, 99
  .word 0
.text
Lapfs_mounted:
