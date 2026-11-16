// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/trust_caches.h>
#include <libkern/amfi/amfi.h>

struct marble_trust_query_record {
    uint64_t ordinal, query_type, length, result;
    uint64_t cache, entry, capabilities;
    uint32_t capabilities_status, category_status;
    uint8_t category, hash[20], reserved[3];
};
volatile uint64_t marble_trust_query_count;
struct marble_trust_query_record marble_trust_queries[64];

__attribute__((used)) kern_return_t
native_ios27_query_trust_cache(TCQueryType_t type, const uint8_t *hash,
                              size_t length, TrustCacheQueryToken_t *output)
{
    TrustCacheQueryToken_t token = {0};
    kern_return_t result = query_trust_cache_with_length(type, hash, length, &token);
    struct marble_trust_query_record row = {0};
    row.ordinal = __atomic_fetch_add(&marble_trust_query_count, 1, __ATOMIC_RELAXED);
    row.query_type = type;
    row.length = length;
    row.result = result;
    if (hash && length == 20) memcpy(row.hash, hash, 20);
    if (result == KERN_SUCCESS) {
        TCCapabilities_t caps = 0;
        uint8_t category = 0;
        TCReturn_t status = amfi->TrustCache.queryGetCapabilities(&token, &caps);
        memcpy(&row.capabilities_status, &status, sizeof(status));
        status = amfi->TrustCache.queryGetConstraintCategory(&token, &category);
        memcpy(&row.category_status, &status, sizeof(status));
        row.cache = (uintptr_t)token.trustCache;
        row.entry = (uintptr_t)token.trustCacheEntry;
        row.capabilities = caps;
        row.category = category;
        if (output) *output = token;
    }
    marble_trust_queries[row.ordinal % 64] = row;
    return result;
}
