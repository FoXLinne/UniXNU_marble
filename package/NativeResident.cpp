#include <arm64/proc_reg.h>
#include <arm/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" void native_resident_configure(uint64_t);
extern "C" void native_resident_return(void);
extern "C" void native_resident_panic_return(void);
extern "C" void native_root_block_capture_log(void);

static uint64_t return_stub;
static uint64_t level1[512] __attribute__((aligned(4096)));
static uint64_t level2[512] __attribute__((aligned(4096)));

void native_resident_configure(uint64_t address)
{
  return_stub = address;
}

extern "C" __attribute__((used)) void native_resident_panic_return(void)
{
  if (!return_stub) return;
  native_root_block_capture_log();
  uint64_t start, now, frequency;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  // Keep the panic visible before restoring the resident firmware.
  do {
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
  } while (now - start < frequency * 5);
  native_resident_return();
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
