// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

class MarbleCpuIdlePolicy {
    uint32_t enabled_ = 0;
    uint32_t active_[3] = {};

public:
    static unsigned domain(unsigned physical) { return physical < 4 ? 0 : physical < 7 ? 1 : 2; }

    void configure(uint32_t enabled)
    {
        enabled_ = enabled;
        active_[0] = enabled & 0x0f;
        active_[1] = enabled & 0x70;
        active_[2] = enabled & 0x80;
    }

    bool owns(unsigned physical) const
    {
        return physical < 8 && (enabled_ & (1u << physical));
    }

    uint32_t active(unsigned domain) const { return active_[domain]; }
    bool allIdle() const { return !(active_[0] | active_[1] | active_[2]); }

    // Return true when the domain needs an EPSS vote for its first wake or last idle entry.
    bool change(unsigned physical, bool enteringIdle)
    {
        const unsigned index = domain(physical);
        const uint32_t bit = 1u << physical;
        const bool wasActive = active_[index] != 0;
        if (enteringIdle) active_[index] &= ~bit;
        else active_[index] |= bit;
        return wasActive != (active_[index] != 0);
    }
};

// Sample the busiest XNU-owned CPU. A brief interruption between 20% and
// 35% resets the quiet streak without toggling an already-low L3 vote.
class MarbleL3IdlePolicy {
    unsigned quiet_ = 0;
    bool low_ = false;
public:
    bool update(bool panelOn, unsigned busiest)
    {
        if (panelOn || busiest >= 35) {
            quiet_ = 0;
            low_ = false;
        } else if (busiest >= 20) {
            quiet_ = 0;
        } else if (quiet_ < 4 && ++quiet_ == 4) {
            low_ = true;
        }
        return low_;
    }
    unsigned quietSamples() const { return quiet_; }
};
