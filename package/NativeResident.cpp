#include <arm64/proc_reg.h>
#include <arm/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <kern/cpu_number.h>
#include <IOKit/IOLib.h>

extern "C" void native_resident_configure(uint64_t);
extern "C" void native_resident_return(void);
extern "C" void native_resident_panic_return(void);
extern "C" void native_root_block_capture_log(void);
extern "C" void native_runtime_usb_stop(void);
extern "C" void native_root_block_export_data(void);
extern "C" volatile uint32_t armcpu_smp_step[8], armcpu_smp_mpidr[8];

static uint64_t return_stub;
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
  psci(0x84000002); // PSCI CPU_OFF
  for (;;) __asm__ volatile("wfi");
}

static void returnOnBootCpu(void *)
{
  unsigned requested = 0;
  for (unsigned core = 1; core < 8; ++core) {
    if (armcpu_smp_step[core] != 7) continue;
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
  native_resident_return();
}

void native_resident_configure(uint64_t address)
{
  return_stub = address;
}

extern "C" __attribute__((used)) void native_resident_panic_return(void)
{
  if (!return_stub) return;
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
      "br %2\n" :: "r"(tcr), "r"(table), "r"(return_stub) : "memory");
  __builtin_unreachable();
}
