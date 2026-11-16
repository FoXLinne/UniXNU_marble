// SPDX-License-Identifier: BSD-2-Clause-Patent
// QUP DMA stream bypass in the firmware-managed apps SMMU.
// Device-tree overrides cap the context banks and stream groups reported by hardware.
#ifndef GENI_SMMU_BYPASS_H
#define GENI_SMMU_BYPASS_H
#include <stdint.h>

struct smmu_io {
    void *ctx;
    uint32_t (*rd)(void *ctx, unsigned offset);
    void (*wr)(void *ctx, unsigned offset, uint32_t value);   // must complete an MMIO barrier before returning
};
struct smmu_plan {
    unsigned slot, bank, pages;
    int reuse;               // the stream already maps to the bank
    int bank_ready;          // another stream (or an earlier run) already left the bank in bypass state
    int extended;
    const char *error;
};
static inline unsigned smmu_bits(unsigned code) {
    switch (code) {case 0: return 32; case 1: return 36; case 2: return 40; case 3: return 42; case 4: return 44; case 5: return 48; default: return 0;}
}
// dma/bytes: the physical range the device will access; refused when it exceeds the input, output or intermediate width.
static inline struct smmu_plan smmu_plan_bypass(const struct smmu_io *io, uint16_t stream, uint64_t dma, uint64_t bytes, unsigned banks, unsigned count) {
    struct smmu_plan p = {0};
    p.slot = 0xffffffffu;
    uint32_t idr0 = io->rd(io->ctx, 0x20), idr1 = io->rd(io->ctx, 0x24), idr2 = io->rd(io->ctx, 0x28);
    p.extended = (idr0 & (1u << 8)) && (io->rd(io->ctx, 0) & (1u << 3));
    if (count > (idr0 & 255)) count = idr0 & 255;
    if (banks > (idr1 & 255)) banks = idr1 & 255;
    if (!count || !banks) {p.error = "SMMU geometry is empty"; return p;}
    if (!(idr0 & (1u << 30)) || !(idr2 & (1u << 12))) {p.error = "SMMU AArch64 stage 1 unsupported"; return p;}
    for (unsigned shift = 0; shift <= 8; shift += 4) {
        unsigned bits = smmu_bits((idr2 >> shift) & 15);
        if (!bits || !bytes || dma >= (1ull << bits) || bytes > (1ull << bits) - dma) {p.error = "DMA exceeds SMMU address width"; return p;}
    }
    p.pages = 1u << (((idr1 >> 28) & 7) + 1);
    if ((idr1 >> 31) || !count || p.pages > 128 || banks < 2 || banks > 128 || (p.pages + banks) * 4096u > 0xd0000u) {
        p.error = "unsupported SMMU geometry"; return p;
    }
    p.bank = banks - 1;
    unsigned matches = 0, sharing = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t smr = io->rd(io->ctx, 0x800 + 4 * i);
        uint32_t s2cr = io->rd(io->ctx, 0xc00 + 4 * i);
        int valid = p.extended ? (s2cr & (1u << 10)) != 0 : (smr & 0x80000000u) != 0;
        if (!valid) {if (p.slot == 0xffffffffu) p.slot = i; continue;}
        uint32_t mask = (smr >> 16) & (p.extended ? 0xffff : 0x7fff);
        int ourBank = ((s2cr >> 16) & 3) == 0 && (s2cr & 255) == p.bank;
        if (((smr ^ stream) & 0xffff & ~mask) == 0) {
            if (++matches > 1 || mask || !ourBank) {p.slot = i; p.error = "conflicting stream mapping"; return p;}
            p.reuse = 1; p.slot = i;
        } else if (ourBank) {
            ++sharing;
        }
    }
    uint32_t sctlr = io->rd(io->ctx, (p.pages + p.bank) * 4096u);
    uint32_t cbar = io->rd(io->ctx, 0x1000 + 4 * p.bank), cba2r = io->rd(io->ctx, 0x1800 + 4 * p.bank);
    p.bank_ready = (sctlr == 0 && cbar == 0x10000 && (cba2r & ~1u) == 0 && (cba2r & 1u));
    if ((sctlr & 1) || ((sharing || p.reuse) && !p.bank_ready)) {p.error = "reserved context bank is not a bypass bank"; return p;}
    if (p.slot == 0xffffffffu) p.error = "no free stream-match group";
    return p;
}
static inline const char *smmu_configure_bypass(const struct smmu_io *io, const struct smmu_plan *p, uint16_t stream) {
    if (p->error) return p->error;
    unsigned cb = (p->pages + p->bank) * 4096u;
    if (!p->bank_ready) {
        io->wr(io->ctx, cb, 0);
        io->wr(io->ctx, 0x1800 + 4 * p->bank, 1);
        io->wr(io->ctx, 0x1000 + 4 * p->bank, 0x10000);
    }
    if (io->rd(io->ctx, cb) != 0 || io->rd(io->ctx, 0x1800 + 4 * p->bank) != 1 || io->rd(io->ctx, 0x1000 + 4 * p->bank) != 0x10000)
        return "SMMU bypass bank readback";
    if (!p->reuse) {
        io->wr(io->ctx, 0xc00 + 4 * p->slot, p->bank);
        if (io->rd(io->ctx, 0xc00 + 4 * p->slot) != p->bank) return "SMMU stream context readback";
        io->wr(io->ctx, 0x800 + 4 * p->slot, (p->extended ? 0 : 0x80000000u) | stream);
        if (p->extended) io->wr(io->ctx, 0xc00 + 4 * p->slot, p->bank | (1u << 10));
    }
    uint32_t expected_smr = (p->extended ? 0 : 0x80000000u) | stream;
    uint32_t expected_s2cr = p->bank | (p->extended ? 1u << 10 : 0);
    if (io->rd(io->ctx, 0x800 + 4 * p->slot) != expected_smr ||
        (io->rd(io->ctx, 0xc00 + 4 * p->slot) & ((3u << 16) | (1u << 10) | 255u)) != expected_s2cr)
        return "SMMU stream mapping readback";
    return 0;
}
#endif
