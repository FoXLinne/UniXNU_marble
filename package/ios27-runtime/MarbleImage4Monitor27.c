// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/errno.h>
#include <stdint.h>
#include <stddef.h>

/* 24B119 adds a monitor-only plane after the Image4 kext has performed its
 * local operation. code_signing_configuration reports CS_MONITOR_TYPE_NONE
 * for this source backend. The kext checks that capability before calling
 * these entries. Preserve the new signatures and report an unavailable
 * monitor to a caller that requests protected execution on this platform.
 * The regular nonce/object APIs retain the source kernel's Image4 runtime.
 * Consumer and complete-function evidence: devin-image4-contract-review.json.
 */
volatile uint64_t native_ios27_image4_monitor_requests[4];

static int valid_region(const void *address, size_t bytes)
{
    uintptr_t start = (uintptr_t)address;
    return start && bytes && bytes <= UINTPTR_MAX - start;
}

__attribute__((used)) int
native_ios27_image4_monitor_set_nonce_digest(uint64_t domain,
                                            const void *digest, uint64_t bytes)
{
    (void)domain;
    __atomic_fetch_add(&native_ios27_image4_monitor_requests[0], 1, __ATOMIC_RELAXED);
    if (bytes > 48 || !valid_region(digest, (size_t)bytes)) return EINVAL;
    return ENOTSUP;
}

__attribute__((used)) int
native_ios27_image4_monitor_roll_nonce(uint64_t domain)
{
    (void)domain;
    __atomic_fetch_add(&native_ios27_image4_monitor_requests[1], 1, __ATOMIC_RELAXED);
    return ENOTSUP;
}

__attribute__((used)) int
native_ios27_image4_monitor_load_root(const void *manifest, size_t manifest_bytes,
                                     const void *payload, size_t payload_bytes)
{
    __atomic_fetch_add(&native_ios27_image4_monitor_requests[2], 1, __ATOMIC_RELAXED);
    if (!manifest || !manifest_bytes || !payload || !payload_bytes) return ESRCH;
    if (!valid_region(manifest, manifest_bytes) || !valid_region(payload, payload_bytes)) return EINVAL;
    return ENOTSUP;
}

__attribute__((used)) int
native_ios27_image4_monitor_load_local_policy(const void *manifest, size_t bytes)
{
    __atomic_fetch_add(&native_ios27_image4_monitor_requests[3], 1, __ATOMIC_RELAXED);
    if (!manifest || !bytes) return ESRCH;
    if (!valid_region(manifest, bytes)) return EINVAL;
    return ENOTSUP;
}
