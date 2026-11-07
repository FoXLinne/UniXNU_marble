// Open the unchanged cache on real APFS, register its complete original
// signature and execute its public getuid success path through genuine SVC.
  adrp x20, _cache_leaf_results@PAGE
  add x20, x20, _cache_leaf_results@PAGEOFF
  adrp x0, Lcache_leaf_path@PAGE
  add x0, x0, Lcache_leaf_path@PAGEOFF
  mov x1, #0
  mov x2, #0
  mov x16, #5
  svc #0x80
  str x0, [x20]
  mrs x9, nzcv
  str x9, [x20, #8]
  b.cs _checkpoint
  mov x21, x0
  mov x8, #1
  str x8, [x20, #48]
  mov x1, #97
  adrp x2, Lcache_leaf_signature@PAGE
  add x2, x2, Lcache_leaf_signature@PAGEOFF
  mov x16, #92
  svc #0x80
  str x0, [x20, #16]
  mrs x9, nzcv
  str x9, [x20, #24]
  b.cs _checkpoint
  mov x8, #2
  str x8, [x20, #48]
  mov x0, #0
  mov x1, #4096
  mov x2, #5 // PROT_READ | PROT_EXEC
  mov x3, #2 // MAP_PRIVATE
  mov x4, x21
  adrp x5, Lcache_leaf_page_offset@PAGE
  ldr x5, [x5, Lcache_leaf_page_offset@PAGEOFF]
  mov x16, #197
  svc #0x80
  str x0, [x20, #32]
  mrs x9, nzcv
  str x9, [x20, #40]
  b.cs _checkpoint
  add x0, x0, #NATIVE_CACHE_LEAF_ENTRY_OFFSET
  adrp x8, _cache_leaf_target@PAGE
  str x0, [x8, _cache_leaf_target@PAGEOFF]
  mov x8, #3
  str x8, [x20, #48]
.global _cache_leaf_dispatch
_cache_leaf_dispatch:
  blr x0
  str x0, [x20, #56]
  mrs x9, nzcv
  str x9, [x20, #64]
  mov x8, #4
  str x8, [x20, #48]
  b _checkpoint
.data
.balign 8
.global _cache_leaf_results, _cache_leaf_target
_cache_leaf_results:
  .fill 9, 8, 0
_cache_leaf_target:
  .quad 0
Lcache_leaf_page_offset:
  .quad NATIVE_CACHE_LEAF_PAGE_OFFSET
Lcache_leaf_signature:
  .quad 0, NATIVE_CACHE_LEAF_SIGNATURE_OFFSET, NATIVE_CACHE_LEAF_SIGNATURE_SIZE
Lcache_leaf_path:
  .asciz NATIVE_CACHE_LEAF_PATH
.text
