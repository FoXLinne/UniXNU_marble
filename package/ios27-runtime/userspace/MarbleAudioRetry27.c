// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>

extern xpc_connection_t marble_system_create_service(const char *, dispatch_queue_t, uint64_t)
    __asm__("_xpc_connection_create_mach_service");

/* The 24B119 BTAudio HAL invalid-listener handlers recreate these registrations
 * synchronously. Failed bootstrap check-ins otherwise form an unbounded loop.
 * Each name identifies a unique receive endpoint; space repeated registration
 * attempts on that name, and keep the caller's queue, flags and return value.
 */
volatile uint64_t marble_audio_retry_stats[2][4];
static pthread_mutex_t retry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t clock_once = PTHREAD_ONCE_INIT;
static mach_timebase_info_data_t clock_ratio;
static void clock_init(void) { (void)mach_timebase_info(&clock_ratio); }
static uint64_t clock_ns(void)
{
    pthread_once(&clock_once, clock_init);
    return (uint64_t)(((__uint128_t)mach_absolute_time() * clock_ratio.numer) / clock_ratio.denom);
}
static int service_slot(const char *name, uint64_t flags)
{
    if (!name || !(flags & XPC_CONNECTION_MACH_SERVICE_LISTENER)) return -1;
    if (!strcmp(name, "com.apple.BTAudioHALPluginAccessories")) return 0;
    if (!strcmp(name, "com.apple.AudioAccessoryCommunication.xpc")) return 1;
    return -1;
}
static uint64_t reserve_attempt(unsigned slot, uint64_t now)
{
    const uint64_t interval = 250000000;
    pthread_mutex_lock(&retry_lock);
    uint64_t last = marble_audio_retry_stats[slot][3];
    uint64_t next = last && last + interval > now ? last + interval : now;
    marble_audio_retry_stats[slot][3] = next;
    marble_audio_retry_stats[slot][0]++;
    if (next > now) marble_audio_retry_stats[slot][1]++;
    pthread_mutex_unlock(&retry_lock);
    return next;
}
static xpc_connection_t marble_audio_create_service(const char *name, dispatch_queue_t queue, uint64_t flags)
{
    int slot = service_slot(name, flags);
    if (slot >= 0) {
        uint64_t began = clock_ns();
        uint64_t deadline = reserve_attempt((unsigned)slot, began);
        for (;;) {
            uint64_t now = clock_ns();
            if (now >= deadline) break;
            uint64_t wait = deadline - now;
            struct timespec delay = {(time_t)(wait / 1000000000), (long)(wait % 1000000000)};
            if (nanosleep(&delay, NULL) && errno != EINTR) break;
        }
        __atomic_fetch_add(&marble_audio_retry_stats[slot][2], clock_ns() - began, __ATOMIC_RELAXED);
    }
    return marble_system_create_service(name, queue, flags);
}
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement; const void *replacee; } audio_retry_interpose = {
    (const void *)marble_audio_create_service, (const void *)marble_system_create_service
};
