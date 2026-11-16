// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compiled against the native kernel's private layout, like its IRQ helpers.
#define MACH_KERNEL_PRIVATE 1
#include <kern/ast.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <arm/cpu_data.h>
#include <arm/cpu_data_internal.h>
#include <mach/arm/thread_status.h>
#include <mach/vm_param.h>

volatile uint64_t marble_cpu_power_ancestors[8][64][5];
extern volatile uint64_t armcpu_power_pc_count[8];

uint64_t native_marble_power_interrupted_lr(void);
uint64_t native_marble_power_interrupted_lr(void)
{
    const arm_saved_state_t *state=getCpuDatap()->cpu_int_state;
    if(!state || !is_saved_state64(state))return 0;
    const arm_saved_state64_t *saved=const_saved_state64(state);
    const unsigned cpu=getCpuDatap()->cpu_number;
    if(cpu<8) {
        const uint64_t sequence=armcpu_power_pc_count[cpu];
        volatile uint64_t *row=marble_cpu_power_ancestors[cpu][sequence&63];
        row[0]=0;row[1]=saved->lr;
        for(unsigned i=2;i<5;++i)row[i]=0;
        const uintptr_t stack=current_thread()->kernel_stack;
        uintptr_t fp=saved->fp;
        for(unsigned i=2;i<5 && stack && !(fp&15) && fp>=stack &&
            fp<=stack+KERNEL_STACK_SIZE-16;++i) {
            const uint64_t *frame=(const uint64_t *)fp;
            const uintptr_t next=frame[0];
            row[i]=frame[1];
            if(next<=fp)break;
            fp=next;
        }
        __atomic_store_n(&row[0],sequence,__ATOMIC_RELEASE);
    }
    return saved->lr;
}
