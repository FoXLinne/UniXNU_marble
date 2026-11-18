// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compiled against the native kernel's private layout, like its IRQ helpers.
#define MACH_KERNEL_PRIVATE 1
#include <kern/processor.h>
#include <kern/thread.h>
#include <arm/cpu_data.h>
#include <arm/cpu_data_internal.h>

/* Native retained suspend. XNU owns the CPU shutdown/resume contexts; this
 * board hook supplies a wakeable WFI/WFE state without an Apple PMGR reset.
 * CPU1 (resident USB) is outside XNU's CPU table and stays firmware-owned. */
#include <pexpert/pexpert.h>
#include <arm/machine_routines.h>

extern void arm_init_cpu(cpu_data_t *, uint64_t) __attribute__((noreturn));
extern void native_marble_cpu_idle(unsigned, _Bool);
extern void native_marble_cpu_l3_sleep(_Bool);
extern _Bool native_resident_wake_requested(void);
volatile uint64_t marble_system_sleep[24];
extern int armcpu_retained_standby(void);
extern void armcpu_clear_virtual_timer_pending(void);
static unsigned retained_sleep_enabled;
static unsigned retained_parked[8], retained_release[8];

void native_marble_system_sleep_configure(void);
int native_marble_cpu_sleep_ready(void);
int native_marble_cpu_resume_request(unsigned);
void native_marble_retained_sleep(void *) __attribute__((noreturn));

void native_marble_system_sleep_configure(void)
{
    unsigned enabled=0, usb_mode=0;
    PE_parse_boot_argn("marble-sleep", &enabled, sizeof(enabled));
    PE_parse_boot_argn("rootstage-runtime-usb", &usb_mode, sizeof(usb_mode));
    const unsigned selected = enabled == 1 && usb_mode == 3;
    __atomic_store_n(&retained_sleep_enabled,selected,__ATOMIC_RELEASE);
    marble_system_sleep[0]=selected;
}
int native_marble_cpu_sleep_ready(void)
{
    return __atomic_load_n(&retained_sleep_enabled,__ATOMIC_ACQUIRE)!=0;
}
int native_marble_cpu_resume_request(unsigned cpu)
{
    if(cpu>=8 || !__atomic_load_n(&retained_parked[cpu],__ATOMIC_ACQUIRE))return 0;
    __atomic_store_n(&retained_release[cpu],1,__ATOMIC_RELEASE);
    __asm__ volatile("dsb ishst; sev":::"memory");
    return 1;
}
static void publish_retained_sleep(void)
{
    const uintptr_t first=(uintptr_t)marble_system_sleep & ~(uintptr_t)63;
    const uintptr_t end=(uintptr_t)marble_system_sleep+sizeof(marble_system_sleep);
    for(uintptr_t line=first;line<end;line+=64)__asm__ volatile("dc cvac,%0"::"r"(line):"memory");
    __asm__ volatile("dsb sy":::"memory");
}

void native_marble_retained_sleep(void *opaque)
{
    cpu_data_t *cdp=(cpu_data_t *)opaque;
    const unsigned cpu=cdp->cpu_number;
    uint64_t mpidr,start,frequency;
    __asm__ volatile("mrs %0,mpidr_el1":"=r"(mpidr));
    const unsigned physical=(mpidr>>8)&0xff;
    if(cpu>=8)__builtin_trap();
    __atomic_store_n(&retained_release[cpu],0,__ATOMIC_RELAXED);
    native_marble_cpu_idle(physical,1);
    ++marble_system_sleep[8+cpu];
    // Publish readiness after the CPU's final clock vote and entry record.
    __atomic_store_n(&retained_parked[cpu],1,__ATOMIC_RELEASE);
    if(cdp==&BootCpuData) {
        const int maximum=ml_get_max_cpu_number();
        for(int i=0;i<=maximum;++i) {
            if(i==(int)cpu || !CpuDataEntries[i].cpu_data_vaddr)continue;
            while(!__atomic_load_n(&retained_parked[i],__ATOMIC_ACQUIRE))__asm__ volatile("yield");
        }
        __asm__ volatile("mrs %0,cntpct_el0; mrs %1,cntfrq_el0":"=r"(start),"=r"(frequency));
        native_marble_cpu_l3_sleep(1);
        marble_system_sleep[0]=2;++marble_system_sleep[1];marble_system_sleep[4]=start;
        // Bring the shared entry-count line to this CPU before publishing it
        // to the retained firmware's memory reader.
        marble_system_sleep[8]=marble_system_sleep[8];
        publish_retained_sleep();
        // A one-second architectural timer is a mailbox wake check. It never
        // dispatches scheduler work while the system remains suspended.
        uint64_t next=0;
        int standby=1;
        for(;;) {
            uint64_t now,irq;
            __asm__ volatile("mrs %0,cntvct_el0":"=r"(now));
            if(!next || now>=next) {
                next=now+frequency;
                __asm__ volatile("msr cntv_cval_el0,%0; msr cntv_ctl_el0,%1; isb"::"r"(next),"r"(1ULL):"memory");
                armcpu_clear_virtual_timer_pending();
            }
            if(native_resident_wake_requested()) {marble_system_sleep[3]=0x10000;break;}
            __asm__ volatile("mrs %0,icc_hppir1_el1":"=r"(irq));irq&=0xffffff;
            if(irq<1020 && irq!=27) {marble_system_sleep[3]=irq;break;}
            uint64_t before,after;
            __asm__ volatile("mrs %0,cntpct_el0":"=r"(before));
            if(standby) {
                const int status=armcpu_retained_standby();
                __asm__ volatile("mrs %0,cntpct_el0":"=r"(after));
                if(status<0 || after-before<frequency/10000)standby=0;
            } else __asm__ volatile("dsb sy; wfi":::"memory");
            __asm__ volatile("mrs %0,icc_hppir1_el1":"=r"(irq));irq&=0xffffff;
            if(irq<1020 && irq!=27) {marble_system_sleep[3]=irq;break;}
        }
        __asm__ volatile("msr cntv_ctl_el0,%0; isb"::"r"(2ULL):"memory");
        uint64_t end;__asm__ volatile("mrs %0,cntpct_el0":"=r"(end));
        native_marble_cpu_l3_sleep(0);
        marble_system_sleep[5]=end-start;++marble_system_sleep[2];marble_system_sleep[0]=3;
        publish_retained_sleep();
    } else {
        while(!__atomic_load_n(&retained_release[cpu],__ATOMIC_ACQUIRE))__asm__ volatile("wfe":::"memory");
    }
    native_marble_cpu_idle(physical,0);
    __atomic_store_n(&retained_parked[cpu],0,__ATOMIC_RELEASE);
    arm_init_cpu(cdp,0);
    __builtin_unreachable();
}

/* IOCPU's public IOKit headers expose opaque processor handles. Keep the
 * native scheduler affinity operations in this private-layout translation unit. */
#include <kern/sched_prim.h>
void *native_marble_suspend_bind_boot(void);
void native_marble_suspend_restore_binding(void *);
void *native_marble_suspend_bind_boot(void)
{
    processor_t previous=thread_bind(master_processor);
    thread_block(THREAD_CONTINUE_NULL);
    return (void *)previous;
}
void native_marble_suspend_restore_binding(void *previous)
{
    thread_bind((processor_t)previous);
}
