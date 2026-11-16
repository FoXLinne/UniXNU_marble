// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <pexpert/device_tree.h>
#include <libkern/libkern.h>

// Board services can run from STARTUP before IOKit has published its registry.
// Match the loader's immutable root compatibility list at that point.
static inline bool marble_platform_matches(void) {
    DTEntry root;
    const void *data;
    unsigned int bytes;
    if (SecureDTLookupEntry(nullptr, "/", &root) != kSuccess ||
        SecureDTGetProperty(root, "compatible", &data, &bytes) != kSuccess)
        return false;
    const char *text = static_cast<const char *>(data);
    for (unsigned int offset = 0; offset < bytes;) {
        unsigned int length = 0;
        while (length < bytes - offset && text[offset + length]) ++length;
        if (length == bytes - offset) return false;
        if (length == sizeof("xiaomi,marble") - 1 &&
            memcmp(text + offset, "xiaomi,marble", length) == 0)
            return true;
        offset += length + 1;
    }
    return false;
}
