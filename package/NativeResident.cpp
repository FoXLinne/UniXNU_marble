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

extern "C" void native_resident_configure(uint64_t);
extern "C" void native_resident_return(void);
extern "C" void native_resident_panic_return(void);
extern "C" void native_root_block_capture_log(void);
extern "C" void native_marble_display_snapshot(void);
extern "C" void native_hid_request_tap(unsigned, unsigned);
extern "C" void native_hid_request_contact(unsigned, unsigned, bool);
extern "C" void armcpu_complete_active_interrupt(void);
extern "C" void native_runtime_usb_stop(void);
extern "C" void native_root_block_export_data(void);
extern "C" void native_marble_reboot_bootloader(void);
extern "C" void native_marble_buttons_stop(void);
extern "C" void native_marble_buttons_restore(void);
#ifdef MARBLE_IOS18_GPU
extern "C" int native_marble_adreno_stop(void);
extern "C" void native_marble_gmu_stop(void);
extern "C" void native_marble_display_timing_request(void);
#endif
extern "C" volatile uint32_t armcpu_smp_step[8], armcpu_smp_mpidr[8];
extern "C" void native_resident_usb_control(volatile uint64_t *);
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
static volatile uint32_t *usb_interrupt;
static IOMemoryMap *earlyResidentMapping;

extern "C" bool native_resident_prepare(void)
{
  if (return_stub) return true;
  IORegistryEntry *map=IORegistryEntry::fromPath("/chosen/memory-map",gIODTPlane);
  OSData *range=map ? OSDynamicCast(OSData,map->getProperty("RootDisks")) : nullptr;
  uint64_t location[2]={};
  if (range && range->getLength()==sizeof(location))
    memcpy(location,range->getBytesNoCopy(),sizeof(location));
  OSSafeReleaseNULL(map);
  if (!location[0]) return false;
  IOMemoryDescriptor *memory=IOMemoryDescriptor::withAddressRange(
      location[0],location[1],kIODirectionInOut|kIOMemoryMapperNone,nullptr);
  earlyResidentMapping=memory ? memory->map(kIOMapAnywhere|kIOMapCopybackCache) : nullptr;
  OSSafeReleaseNULL(memory);
  if (!earlyResidentMapping) return false;
  const auto *table=reinterpret_cast<const ROOT_STAGE_TABLE *>(earlyResidentMapping->getVirtualAddress());
  return_stub=table->ReturnStub;
  native_resident_usb_control(const_cast<volatile uint64_t *>(&table->ReturnTest));
  return return_stub!=0;
}

static void usbControlWorker(void *, wait_result_t)
{
  for (;;) {
    uint32_t request=usb_interrupt[0];
    __asm__ volatile("dmb oshld" ::: "memory");
    if (request==1) {
      native_marble_buttons_stop();
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
#ifdef MARBLE_IOS18_GPU
      if (operation==5) native_marble_display_timing_request();
#endif
      usb_interrupt[0]=0;
      __asm__ volatile("dsb sy" ::: "memory");
    }
    if (request==2) {
      usb_interrupt[0]=0;
      __asm__ volatile("dsb sy" ::: "memory");
      native_marble_reboot_bootloader();
    }
    IOSleep(1);
  }
}

extern "C" void native_resident_usb_control(volatile uint64_t *request)
{
  unsigned mode=0;
  if (!PE_parse_boot_argn("rootstage-runtime-usb",&mode,sizeof(mode)) || mode!=3) return;
  if (usb_interrupt) return;
  usb_interrupt=reinterpret_cast<volatile uint32_t *>(request);
  // The firmware request and kernel readiness occupy separate words. Ordinary
  // accesses support both RAM and device mappings without RCpc instructions.
  usb_interrupt[1]=1;
  __asm__ volatile("dsb sy" ::: "memory");
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
#ifdef MARBLE_IOS18_GPU
  native_marble_buttons_restore();
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
  native_runtime_usb_stop();
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
