// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// Caller serializes this policy with GPU submission and retirement.
class MarbleGpuIdlePolicy {
    bool restore_=false,low_=false,disabled_=false;
public:
    bool needsRestore() const {return restore_;}
    bool lowConfirmed() const {return low_;}
    bool idleDisabled() const {return disabled_;}
    template<class Vote> int change(bool idle,Vote vote) {
        if(idle) {
            if(disabled_ || restore_)return 0;
            // An unacknowledged low vote may have reached firmware. Every
            // subsequent submission must obtain a high-vote acknowledgement.
            restore_=true;
            const int status=vote(1,1);
            if(status)disabled_=true;
            else low_=true;
            return status;
        }
        if(!restore_)return 0;
        const int status=vote(7,11);
        if(status)disabled_=true;
        else {restore_=false;low_=false;}
        return status;
    }
};
