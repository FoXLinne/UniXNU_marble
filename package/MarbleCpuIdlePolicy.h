// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

struct MarbleCpuIdleDecision { unsigned domain; int vote; };
class MarbleCpuIdlePolicy {
    // Physical CPU1 belongs to resident USB. XNU owns 0,2,3 / 4,5,6 / 7.
    uint32_t active_[3]={0x0d,0x70,0x80};
    bool low_[3]={false,false,false};
public:
    static unsigned domain(unsigned physical) {return physical<4?0:physical<7?1:2;}
    uint32_t active(unsigned d) const {return active_[d];}
    // The caller serializes each domain. A wake restores its maximum vote
    // before the kernel handles the interrupt or schedules runnable work.
    MarbleCpuIdleDecision change(unsigned physical,bool enter) {
        if(physical>=8 || physical==1)return {0,-1};
        const unsigned d=domain(physical),bit=1u<<physical;
        if(enter)active_[d]&=~bit;else active_[d]|=bit;
        const bool low=active_[d]==0;
        if(low==low_[d])return {d,-1};
        low_[d]=low;
        return {d,low?0:1};
    }
};
