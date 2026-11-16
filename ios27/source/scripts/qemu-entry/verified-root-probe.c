/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Development-only crypto instrument. It never changes root/AMFI policy. */
#include "verified_root.h"
#include <libkern/libkern.h>
#include <kern/kalloc.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <sys/errno.h>

void native_verified_root_probe(void);
void native_verified_root_checkpoint(const volatile void *, const volatile void *);
extern const struct vr_crypto vr_xnu_crypto;
extern const uint8_t verified_root_manifest[], verified_root_key[], verified_root_samples[];
extern const uint64_t verified_root_manifest_bytes, verified_root_sample_indices[4], verified_root_sample_lengths[4];
volatile uint32_t native_verified_root_status = 0;
volatile uint32_t native_verified_root_results[8] = {0};
volatile uint32_t native_verified_root_signature_calls = 0;
volatile uintptr_t native_verified_root_manifest_pointer = 0;
volatile uintptr_t native_verified_root_key_pointer = 0;
volatile uint64_t native_verified_root_block_count = 0;

static int hash(const void *p, size_t n, uint8_t out[32]) {
    return vr_xnu_crypto.sha256(p, n, out);
}
static int verify(const uint8_t key[VR_KEY_BYTES], const uint8_t digest[32], const uint8_t signature[VR_SIGNATURE_BYTES]) {
    native_verified_root_signature_calls++;
    return vr_xnu_crypto.rsa3072_sha256(key, digest, signature);
}
static const struct vr_crypto provider = {hash, verify};

__attribute__((noinline)) void
native_verified_root_checkpoint(const volatile void *manifest, const volatile void *key)
{
    __asm__ volatile("" : : "r"(manifest), "r"(key) : "memory");
}

void
native_verified_root_probe(void)
{
    struct vr_view view, refused;
    vm_offset_t copy = 0;
    vm_size_t copy_bytes = round_page((vm_size_t)verified_root_manifest_bytes);
    uint8_t wrong_key[VR_KEY_BYTES];
    uint8_t *block = NULL;
    int all_blocks = 0;
    native_verified_root_manifest_pointer = (uintptr_t)verified_root_manifest;
    native_verified_root_key_pointer = (uintptr_t)verified_root_key;
    native_verified_root_results[0] = vr_open(verified_root_manifest, verified_root_manifest_bytes,
        verified_root_key, 1, &provider, &view);
    if (native_verified_root_results[0] != VR_OK) goto done;
    native_verified_root_block_count = view.block_count;
    for (unsigned i = 0; i < 4; ++i) {
        if (vr_verify_block(&view, verified_root_sample_indices[i], verified_root_samples + i * VR_BLOCK_BYTES,
                            (size_t)verified_root_sample_lengths[i], &provider) == VR_OK) all_blocks++;
    }
    native_verified_root_results[1] = (uint32_t)all_blocks;
    if (kmem_alloc(kernel_map, &copy, copy_bytes,
                   KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) goto done;
    memcpy((void *)copy, verified_root_manifest, verified_root_manifest_bytes);
    uint8_t *changed = (uint8_t *)copy;
    changed[VR_HEADER_BYTES] ^= 1;
    native_verified_root_results[2] = vr_open(changed, verified_root_manifest_bytes, verified_root_key, 1, &provider, &refused);
    changed[VR_HEADER_BYTES] ^= 1;
    changed[40] ^= 1;
    native_verified_root_results[3] = vr_open(changed, verified_root_manifest_bytes, verified_root_key, 1, &provider, &refused);
    changed[40] ^= 1;
    changed[VR_HEADER_BYTES + VR_SIGNATURE_BYTES] ^= 1;
    native_verified_root_results[4] = vr_open(changed, verified_root_manifest_bytes, verified_root_key, 1, &provider, &refused);
    block = kalloc_data(VR_BLOCK_BYTES, Z_WAITOK);
    memcpy(block, verified_root_samples, VR_BLOCK_BYTES);
    block[7] ^= 1;
    native_verified_root_results[5] = vr_verify_block(&view, 0, block, VR_BLOCK_BYTES, &provider);
    memcpy(wrong_key, verified_root_key, VR_KEY_BYTES);
    wrong_key[100] ^= 1;
    native_verified_root_results[6] = vr_open(verified_root_manifest, verified_root_manifest_bytes,
        wrong_key, 1, &provider, &refused);
    native_verified_root_results[7] = vr_open(verified_root_manifest, verified_root_manifest_bytes,
        verified_root_key, 2, &provider, &refused);
    if (native_verified_root_results[1] == 4 && native_verified_root_results[2] == VR_SIGNATURE &&
        native_verified_root_results[3] == VR_SIGNATURE && native_verified_root_results[4] == VR_HASH_TABLE &&
        native_verified_root_results[5] == VR_BLOCK && native_verified_root_results[6] == VR_KEY &&
        native_verified_root_results[7] == VR_FORMAT && !refused.hashes && native_verified_root_signature_calls == 4)
        native_verified_root_status = 1;
done:
    if (block) kfree_data(block, VR_BLOCK_BYTES);
    if (copy) kmem_free(kernel_map, copy, copy_bytes);
    native_verified_root_checkpoint(&native_verified_root_manifest_pointer, &native_verified_root_key_pointer);
}
