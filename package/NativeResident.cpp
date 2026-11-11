#include <arm64/proc_reg.h>
#include <arm/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <kern/cpu_number.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <Protocol/RootStage.h>
#include <kern/clock.h>
#include "MarbleUsbHealth.h"

extern "C" void native_resident_configure(uint64_t);
extern "C" void native_resident_return(void);
extern "C" void native_resident_panic_return(void);
extern "C" void native_root_block_capture_log(void);
extern "C" void native_marble_display_snapshot(void);
extern "C" void native_hid_request_tap(unsigned, unsigned);
extern "C" void native_hid_request_contact(unsigned, unsigned, bool);
extern "C" void armcpu_complete_active_interrupt(void);
extern "C" void native_runtime_usb_stop(void);
#ifdef MARBLE_IOS18
extern "C" void native_marble_usb_quiesce(void);
#endif
extern "C" void native_root_block_export_data(void);
extern "C" void native_marble_reboot_bootloader(void);
extern "C" void native_marble_buttons_stop(void);
extern "C" void native_marble_buttons_restore(void);
extern "C" void native_marble_buttons_pulse(unsigned);
extern "C" void native_marble_haptics_stop(void);
extern "C" void native_marble_audio_stop(void);
extern "C" void native_marble_haptics_restore(void);
#ifdef MARBLE_IOS18_GPU
extern "C" int native_marble_adreno_stop(void);
extern "C" void native_marble_gmu_stop(void);
extern "C" void native_marble_display_timing_request(void);
extern "C" void native_marble_panel_refresh_request(unsigned);
#endif
extern "C" volatile uint32_t armcpu_smp_step[8], armcpu_smp_mpidr[8];
extern "C" void native_resident_usb_control(const ROOT_STAGE_TABLE *);
extern "C" void native_marble_rtc_start(const ROOT_STAGE_TABLE *);
extern "C" void native_marble_rtc_observe(void);
extern "C" void native_marble_rtc_sync(void);
extern "C" {
char native_resident_panic_message[512] __attribute__((aligned(64), used));
void native_resident_record_panic(const char *message);
}

extern "C" void native_resident_record_panic(const char *message)
{
  strlcpy(native_resident_panic_message,message,sizeof(native_resident_panic_message));
  for (uintptr_t line=reinterpret_cast<uintptr_t>(native_resident_panic_message);
       line<reinterpret_cast<uintptr_t>(native_resident_panic_message)+sizeof(native_resident_panic_message);line+=64)
    __asm__ volatile("dc civac, %0" :: "r"(line) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t return_stub;
static const ROOT_STAGE_TABLE *residentTable;
extern "C" const ROOT_STAGE_TABLE *native_resident_table(void) {return residentTable;}
static volatile ROOT_STAGE_USB_HOST *usb_host;
static volatile ROOT_STAGE_USB_KERNEL *usb_kernel;

static void refreshUsbHost(void)
{
  __asm__ volatile("dc ivac, %0\n\tdsb sy" :: "r"(usb_host) : "memory");
}

static void publishUsbKernel(void)
{
  __asm__ volatile("dc cvac, %0\n\tdsb sy" :: "r"(usb_kernel) : "memory");
}
static MarbleUsbHealth usbHealth;
extern "C" {
volatile uint32_t marble_usb_heartbeat, marble_usb_stalls;
void native_marble_usb_health_check(void);
bool native_resident_usb_service_active(void);
}

extern "C" bool native_resident_usb_service_active(void)
{
  if (!usb_host) return false;
  refreshUsbHost();
  return (usb_host->Heartbeat & MARBLE_USB_HEARTBEAT_MASK) == MARBLE_USB_HEARTBEAT_TAG;
}
static IOMemoryMap *earlyResidentMapping;

extern "C" void native_marble_usb_health_check(void)
{
  if (!usb_host) return;
  refreshUsbHost();
  uint32_t heartbeat=usb_host->Heartbeat;
  marble_usb_heartbeat=heartbeat;
  uint64_t ns=0;
  absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
  if (marble_usb_health_tick(&usbHealth,heartbeat,ns/1000000000ULL)) {
    ++marble_usb_stalls;
    IOLog("MarbleUSB: resident heartbeat stalled value=%x\n",heartbeat);
    usbHealth.changed=ns/1000000000ULL;
  }
}

extern "C" bool native_resident_prepare(void)
{
  if (return_stub) return true;
  IORegistryEntry *map=IORegistryEntry::fromPath("/chosen/memory-map",gIODTPlane);
  OSData *range=map ? OSDynamicCast(OSData,map->getProperty("RootDisks")) : nullptr;
  uint64_t location[2]={};
  if (range && range->getLength()==sizeof(location))
    memcpy(location,range->getBytesNoCopy(),sizeof(location));
  OSSafeReleaseNULL(map);
  if (!location[0] || location[1]!=sizeof(ROOT_STAGE_TABLE)) return false;
  IOMemoryDescriptor *memory=IOMemoryDescriptor::withAddressRange(
      location[0],location[1],kIODirectionInOut|kIOMemoryMapperNone,nullptr);
  earlyResidentMapping=memory ? memory->map(kIOMapAnywhere|kIOMapCopybackCache) : nullptr;
  OSSafeReleaseNULL(memory);
  if (!earlyResidentMapping) return false;
  const auto *table=reinterpret_cast<const ROOT_STAGE_TABLE *>(earlyResidentMapping->getVirtualAddress());
  if (table->Magic!=ROOT_STAGE_MAGIC || table->Version!=ROOT_STAGE_VERSION) return false;
  return_stub=table->ReturnStub;
  native_marble_rtc_start(table);
  native_resident_usb_control(table);
  return return_stub!=0;
}

static void usbControlWorker(void *, wait_result_t)
{
  uint64_t activeUntil=0;
  for (;;) {
    refreshUsbHost();
    uint32_t sequence=usb_host->Sequence;
    uint32_t request=sequence==usb_kernel->Completed ? 0 : usb_host->Command;
    if (request) clock_interval_to_deadline(500,kMillisecondScale,&activeUntil);
    if (request==1) {
      usb_kernel->Completed=sequence;
      usb_kernel->Ready=0;
      publishUsbKernel();
      native_marble_buttons_stop();
      native_marble_haptics_stop();
      native_marble_audio_stop();
      native_root_block_capture_log();
      panic("RootStage USB host interrupt");
    }
    if (request & 0x80000000U) {
      const unsigned operation=(request>>24)&0x7f, x=(request>>12)&0xfff, y=request&0xfff;
      if (x<1080 && y<2400) {
        if (operation==1) native_hid_request_tap(x,y);
        if (operation==2 || operation==3) native_hid_request_contact(x,y,operation==2);
      }
      if (operation==4) native_marble_display_snapshot();
      if (operation==6) native_marble_rtc_observe();
      if (operation==7) native_marble_rtc_sync();
#ifdef MARBLE_IOS18_GPU
      if (operation==5) {
        if (y==1) native_marble_buttons_pulse(x);
        else if (x) native_marble_panel_refresh_request(x==1 ? 0 : x);
        else native_marble_display_timing_request();
      }
#endif
    }
    if (request==2) {
      usb_kernel->Completed=sequence;
      publishUsbKernel();
      native_marble_reboot_bootloader();
    }
    if (request) {
      usb_kernel->Completed=sequence;
      publishUsbKernel();
    }
    IOSleep(mach_absolute_time()<activeUntil ? 1 : 16);
  }
}

extern "C" void native_resident_usb_control(const ROOT_STAGE_TABLE *table)
{
  residentTable=table;
  unsigned mode=0;
  if (!PE_parse_boot_argn("rootstage-runtime-usb",&mode,sizeof(mode)) || mode!=3) return;
  if (usb_host || table->Version!=ROOT_STAGE_VERSION) return;
  usb_host=const_cast<ROOT_STAGE_USB_HOST *>(&table->UsbHost);
  usb_kernel=const_cast<ROOT_STAGE_USB_KERNEL *>(&table->UsbKernel);
  refreshUsbHost();
  usb_kernel->Completed=0;
  usb_kernel->Ready=1;
  publishUsbKernel();
  thread_t worker;
  if (kernel_thread_start(usbControlWorker,nullptr,&worker)!=KERN_SUCCESS)
    panic("RootStage USB interrupt worker start failed");
  thread_deallocate(worker);
}
static uint64_t level1[512] __attribute__((aligned(4096)));
static uint64_t level2[512] __attribute__((aligned(4096)));

static uint64_t psci(uint64_t function, uint64_t argument = 0)
{
  register uint64_t x0 __asm__("x0") = function;
  register uint64_t x1 __asm__("x1") = argument;
  register uint64_t x2 __asm__("x2") = 0;
  __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2)
      :: "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  return x0;
}

static void powerOffSecondary(void *)
{
  __asm__ volatile("msr daifset, #15\n\tmsr cntv_ctl_el0, xzr\n\tmsr icc_igrpen1_el1, xzr\n\tdsb sy\n\tisb" ::: "memory");
  armcpu_complete_active_interrupt();
  psci(0x84000002); // PSCI CPU_OFF
  for (;;) __asm__ volatile("wfi");
}

static void returnOnBootCpu(void *)
{
#ifdef MARBLE_IOS18
  native_marble_usb_quiesce();
#endif
  unsigned requested = 0;
  for (unsigned core = 1; core < 8; ++core) {
    // The startup worker and initCPU publish separate milestones. A running
    // CPU can retain the worker's step 3; query PSCI ownership before reuse.
    if (!armcpu_smp_mpidr[core] || psci(0xc4000004, armcpu_smp_mpidr[core]) != 0) continue;
    requested |= 1U << core;
    if (cpu_immediate_xcall(core, powerOffSecondary, &return_stub) != KERN_SUCCESS)
      psci(0x84000009); // PSCI SYSTEM_RESET
  }
  uint64_t began, now, frequency;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(began));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  while (requested) {
    for (unsigned core = 1; core < 8; ++core)
      if ((requested & (1U << core)) && psci(0xc4000004, armcpu_smp_mpidr[core]) == 1)
        requested &= ~(1U << core);
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    if (now - began > frequency / 2) psci(0x84000009);
  }
  native_runtime_usb_stop();
#ifdef MARBLE_IOS18_GPU
  native_marble_buttons_restore();
  native_marble_haptics_restore();
  if (native_marble_adreno_stop()) psci(0x84000009);
  native_marble_gmu_stop();
#endif
  native_resident_return();
}

void native_resident_configure(uint64_t address)
{
  return_stub = address;
}

extern "C" __attribute__((used)) void native_resident_panic_return(void)
{
  if (!return_stub && !native_resident_prepare()) return;
  native_root_block_capture_log();
  native_root_block_export_data();
  uint64_t start, now, frequency;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  // Keep the panic visible before restoring the resident firmware.
  do {
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
  } while (now - start < frequency * 5);
  // The resident firmware resumes on its boot core after the secondary cores leave the kernel.
  if (cpu_number() == 0) returnOnBootCpu(nullptr);
  if (cpu_immediate_xcall(0, returnOnBootCpu, &return_stub) != KERN_SUCCESS)
    psci(0x84000009);
  powerOffSecondary(nullptr);
}

extern "C" void native_resident_return(void)
{
  if (!return_stub) return;
  uint64_t tcr;
  __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
  unsigned t0sz = tcr & 63;
  if (t0sz < 25 || t0sz > 33 || return_stub >> (64 - t0sz)) return;
  level1[return_stub >> 30] = ml_static_vtop((vm_offset_t)level2) | ARM_TTE_TYPE_TABLE | ARM_TTE_VALID;
  level2[(return_stub >> 21) & 511] = (return_stub & ~0x1fffffULL) |
      ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK | ARM_TTE_BLOCK_AF |
      ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_AP(AP_RWNA) |
      ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITEBACK);
  uint64_t table = ml_static_vtop((vm_offset_t)level1);
  armcpu_complete_active_interrupt();
  tcr &= ~0x80ULL;
  __asm__ volatile(
      "msr daifset, #15\n"
      "msr cntv_ctl_el0, xzr\n"
      "dsb sy\n"
      "msr tcr_el1, %0\n"
      "msr ttbr0_el1, %1\n"
      "isb\n"
      "tlbi vmalle1\n"
      "dsb sy\n"
      "isb\n"
      "mov x0, %3\n"
      "br %2\n" :: "r"(tcr), "r"(table), "r"(return_stub),
      "r"(native_resident_panic_message) : "x0", "memory");
  __builtin_unreachable();
}
