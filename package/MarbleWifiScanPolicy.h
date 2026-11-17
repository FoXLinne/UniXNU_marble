// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MARBLE_WIFI_SCAN_POLICY_H
#define MARBLE_WIFI_SCAN_POLICY_H
#include <stdint.h>
/* Keep handshakes on channel, coalesce bursts, and refresh associated BSS data.
 * Directed discovery reaches the radio; a fresh covered-channel follow-up
 * can reuse the just-completed observation through the helper below. */
static inline int marble_wifi_scan_cached(uint64_t now, uint64_t completed,
    uint64_t started, int connected, int authenticating, int directed, unsigned count)
{
    if (authenticating) return 1;
    if (!directed && started && now >= started && now - started < 5000) return 1;
    const uint64_t lifetime = connected ? 30000 : 10000;
    return !directed && count && completed && now >= completed && now - completed < lifetime;
}
/* A follow-up on one channel may reuse a real observation for 500 ms after
 * scan completion. Keep the observation timestamp and expire stale entries. */
static inline int marble_wifi_scan_target_cached(uint64_t now, uint64_t completed,
    uint32_t observed_age_ms)
{
    return completed && now >= completed && now - completed < 500 && observed_age_ms < 1000;
}
#endif
