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
#include "MarbleWifi.h"

extern void arm_init_cpu(cpu_data_t *, uint64_t) __attribute__((noreturn));
extern void native_marble_cpu_idle(unsigned, _Bool);
extern void native_marble_cpu_l3_sleep(_Bool);
extern uint32_t native_resident_pending_wake_command(void);
extern _Bool native_resident_usb_has_doorbell(void);
volatile uint64_t marble_system_sleep[24];
volatile uint64_t marble_system_sleep_wait[8];
volatile uint64_t marble_wifi_retained_batch[3]; // deferred, deadline, other wake
extern int armcpu_retained_standby(void);
extern void armcpu_clear_virtual_timer_pending(void);
extern int armcpu_spi_mask(uint32_t intid, _Bool masked);
static uint64_t wifi_hold_until;
static unsigned wifi_batch_enabled;
static unsigned retained_sleep_enabled;
static unsigned retained_user_wake;
static unsigned retained_parked[8], retained_release[8];

void native_marble_system_sleep_configure(void);
int native_marble_cpu_sleep_ready(void);
int native_marble_retained_user_wake(void);
int native_marble_cpu_resume_request(unsigned);
void native_marble_retained_sleep(void *) __attribute__((noreturn));

void native_marble_system_sleep_configure(void)
{
    unsigned enabled=0;
    PE_parse_boot_argn("marble-sleep", &enabled, sizeof(enabled));
    unsigned wifi_batch = 0;
    PE_parse_boot_argn("marble-wlan-batch", &wifi_batch, sizeof(wifi_batch));
    wifi_batch_enabled = wifi_batch == 1;
    __atomic_store_n(&retained_sleep_enabled,enabled==1,__ATOMIC_RELEASE);
    marble_system_sleep[0]=enabled==1;
}
int native_marble_cpu_sleep_ready(void)
{
    return __atomic_load_n(&retained_sleep_enabled,__ATOMIC_ACQUIRE)!=0;
}
int native_marble_retained_user_wake(void)
{
    return __atomic_load_n(&retained_user_wake,__ATOMIC_ACQUIRE)!=0;
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
    const uintptr_t wait_first=(uintptr_t)marble_system_sleep_wait & ~(uintptr_t)63;
    const uintptr_t wait_end=(uintptr_t)marble_system_sleep_wait+sizeof(marble_system_sleep_wait);
    for(uintptr_t line=wait_first;line<wait_end;line+=64)__asm__ volatile("dc cvac,%0"::"r"(line):"memory");
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
        // Older resident firmware needs a mailbox deadline. Negotiated
        // firmware rings the boot core for every pending control command.
        const _Bool needs_mailbox_timer = !native_resident_usb_has_doorbell();
        uint64_t mailbox_check = 0, armed_deadline = 0;
        uint64_t now;
        __asm__ volatile("mrs %0,cntvct_el0":"=r"(now));
        // Only defer a second WLAN interrupt close to the last WLAN wake.
        // Let it remain pending in the GIC. No mask survives a CPU resume.
        const uint64_t wifi_deadline = wifi_hold_until;
        _Bool wifi_deferred = wifi_batch_enabled && !needs_mailbox_timer &&
            now < wifi_deadline &&
            armcpu_spi_mask(MarbleWifiMsiInterrupt, 1) == 0;
        if (wifi_deferred) ++marble_wifi_retained_batch[0];
        __asm__ volatile("msr cntv_ctl_el0,%0; isb"::"r"(2ULL):"memory");
        armcpu_clear_virtual_timer_pending();
        uint32_t wake_command=0;
        int standby=1;
        for(;;) {
            uint64_t irq;
            __asm__ volatile("mrs %0,cntvct_el0":"=r"(now));
            if (wifi_deferred && now >= wifi_deadline) {
                armcpu_spi_mask(MarbleWifiMsiInterrupt, 0);
                wifi_deferred = 0;
                ++marble_wifi_retained_batch[1];
            }
            if (needs_mailbox_timer && (!mailbox_check || now >= mailbox_check)) {
                mailbox_check = now + frequency;
                ++marble_system_sleep_wait[3];
            }
            const uint64_t deadline = wifi_deferred ? wifi_deadline : mailbox_check;
            if (deadline != armed_deadline) {
                __asm__ volatile("msr cntv_ctl_el0,%0; isb"::"r"(2ULL):"memory");
                armcpu_clear_virtual_timer_pending();
                if (deadline)
                    __asm__ volatile("msr cntv_cval_el0,%0; msr cntv_ctl_el0,%1; isb"::"r"(deadline),"r"(1ULL):"memory");
                armed_deadline = deadline;
            }
            wake_command=native_resident_pending_wake_command();
            if(wake_command) {marble_system_sleep[3]=0x10000;break;}
            __asm__ volatile("mrs %0,icc_hppir1_el1":"=r"(irq));irq&=0xffffff;
            if(irq<1020 && irq!=27) {marble_system_sleep[3]=irq;break;}
            uint64_t before,after;
            __asm__ volatile("mrs %0,cntpct_el0":"=r"(before));
            if(standby) {
                const int status=armcpu_retained_standby();
                ++marble_system_sleep_wait[0];marble_system_sleep_wait[2]=(uint64_t)(int64_t)status;
                __asm__ volatile("mrs %0,cntpct_el0":"=r"(after));
                if(status<0 || after-before<frequency/10000)standby=0;
                else ++marble_system_sleep_wait[1];
            } else __asm__ volatile("dsb sy; wfi":::"memory");
            __asm__ volatile("mrs %0,icc_hppir1_el1":"=r"(irq));irq&=0xffffff;
            if(irq<1020 && irq!=27) {marble_system_sleep[3]=irq;break;}
            ++marble_system_sleep[6];
            if(!(marble_system_sleep[6]&1023)) {
                uint64_t value;
                __asm__ volatile("mrs %0,icc_rpr_el1":"=r"(value));marble_system_sleep_wait[4]=value;
                __asm__ volatile("mrs %0,icc_pmr_el1":"=r"(value));marble_system_sleep_wait[5]=value;
                __asm__ volatile("mrs %0,icc_hppir1_el1":"=r"(value));marble_system_sleep_wait[6]=value;
                __asm__ volatile("mrs %0,isr_el1":"=r"(value));marble_system_sleep_wait[7]=value;
            }
        }
        __asm__ volatile("msr cntv_ctl_el0,%0; isb"::"r"(2ULL):"memory");
        if (wifi_deferred) {
            armcpu_spi_mask(MarbleWifiMsiInterrupt, 0);
            ++marble_wifi_retained_batch[2];
        }
        uint64_t end;__asm__ volatile("mrs %0,cntpct_el0":"=r"(end));
        native_marble_cpu_l3_sleep(0);
        const uint64_t reason=marble_system_sleep[3];
        // The command is still pending while the XNU workers are parked.
        // Op 5, argument 1 pulses a key mask. Bit 1 is the side button.
        if (!wake_command) wake_command=native_resident_pending_wake_command();
        const _Bool host_power_key=(wake_command >> 24)==0x85 &&
            (wake_command & 0xfff)==1 && (wake_command & (2u << 12));
        const _Bool user_wake=reason==513 || host_power_key;
        if (user_wake) wifi_hold_until = 0;
        else if (wifi_batch_enabled && reason == MarbleWifiMsiInterrupt) {
            __asm__ volatile("mrs %0,cntvct_el0":"=r"(now));
            wifi_hold_until = now + 3 * frequency / 4;
        }
        __atomic_store_n(&retained_user_wake,user_wake,__ATOMIC_RELEASE);
        if (user_wake) ++marble_system_sleep[23];
        marble_system_sleep[5]=end-start;
        marble_system_sleep[16]+=end-start;
        if (reason==0 || reason==1 || reason==0x10000) ++marble_system_sleep[17];
        else if (reason>=173 && reason<181) ++marble_system_sleep[18]; // WLAN PCIe MSI
        else if (reason==240) ++marble_system_sleep[19]; // Bluetooth host wake
        else if (reason==261) ++marble_system_sleep[20]; // ADSP/AOP IPCC
        else if (reason==513) ++marble_system_sleep[21]; // PMIC keys
        else ++marble_system_sleep[22];
        ++marble_system_sleep[2];marble_system_sleep[0]=3;
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
