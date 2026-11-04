// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOService.h>
// 22H384: instance size 0xf8, vtable 0xfffffff007ac9ea0.
// Hardware slots 168-171 use IA diversities e2f7, 59b4, 7d00, b78a.
// Method names preserve those diversities for original authenticated callers.
class IOWatchdog : public IOService {
    OSDeclareAbstractStructors(IOWatchdog)
    unsigned char originalPrivate[0xf8 - sizeof(IOService)];
public:
    void stop(IOService *) override;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) override;
    virtual void enable3234(void) = 0;
    virtual void disable13331(void) = 0;
    virtual void pet65562(void) = 0;
    virtual void properties34074(void);
};
static_assert(sizeof(IOWatchdog) == 0xf8, "22H384 IOWatchdog instance size");
