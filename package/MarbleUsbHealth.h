// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MARBLE_USB_HEALTH_H
#define MARBLE_USB_HEALTH_H
#include <stdint.h>

#define MARBLE_USB_HEARTBEAT_TAG UINT32_C(0x80000001)
#define MARBLE_USB_HEARTBEAT_MASK UINT32_C(0x80000001)
#define MARBLE_USB_STALL_SECONDS 30

struct MarbleUsbHealth {
    uint32_t last;
    uint64_t changed;
    unsigned armed;
};

static inline unsigned marble_usb_health_tick(struct MarbleUsbHealth *health, uint32_t heartbeat,
                                              uint64_t seconds)
{
    if ((heartbeat & MARBLE_USB_HEARTBEAT_MASK) != MARBLE_USB_HEARTBEAT_TAG) {
        health->armed = 0;
        return 0;
    }
    if (!health->armed || heartbeat != health->last || seconds < health->changed) {
        health->last = heartbeat;
        health->changed = seconds;
        health->armed = 1;
        return 0;
    }
    return seconds - health->changed >= MARBLE_USB_STALL_SECONDS;
}
#endif
