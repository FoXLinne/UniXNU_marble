// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <kern/startup.h>

extern "C" void native_marble_watchdog_start(void);
extern "C" struct vnode *rootvnode;

static uint64_t watchdogCall(uint64_t function, uint64_t *control = nullptr) {
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1) ::
        "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
        "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    if (control) *control = x1;
    return x0;
}

static void watchdogWorker(void *, wait_result_t) {
    uint64_t control = 0;
    uint64_t status = watchdogCall(0x86000006, &control);
    IOLog("MarbleWatchdog: startup active=%u status=%llx\n", !status && (control & 1), status);
    if (!status && (control & 1)) {
        unsigned bootSeconds = 0;
        while (bootSeconds < 90 || rootvnode) {
            watchdogCall(0x86000007);
            IOSleep(1000);
            if (bootSeconds < 90) ++bootSeconds;
        }
        IOLog("MarbleWatchdog: root mount timeout\n");
    }
    thread_terminate(current_thread());
}

extern "C" void native_marble_watchdog_start(void) {
    thread_t thread;
    if (kernel_thread_start(watchdogWorker, nullptr, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}

STARTUP(THREAD_CALL, STARTUP_RANK_LAST, native_marble_watchdog_start);
