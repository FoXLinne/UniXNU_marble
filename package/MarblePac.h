// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

struct MarblePacResult {
    unsigned long long mpidr, isar1, isar2, currentEL;
    unsigned keys, authenticated, visited;
};

// Each test restores the key and SCTLR before returning to compiler-generated code.
#define TEST_ADDRESS_KEY(KEY, SIGN, AUTH, BIT) do { \
    unsigned long long oldLow, oldHigh, control, signedPointer, restored, readLow, readHigh; \
    const unsigned long long low = 0x91b37519c2e7486dULL, high = 0xf4a68d2b903157ceULL; \
    const unsigned long long pointer = 0xfffffff007012340ULL, modifier = 0x72830519ULL; \
    __asm__ volatile( \
        ".arch_extension pauth\n" \
        "mrs %[ol], " KEY "KeyLo_EL1\n" \
        "mrs %[oh], " KEY "KeyHi_EL1\n" \
        "mrs %[ctl], SCTLR_EL1\n" \
        "msr " KEY "KeyLo_EL1, %[lo]\n" \
        "msr " KEY "KeyHi_EL1, %[hi]\n" \
        "mrs %[rl], " KEY "KeyLo_EL1\n" \
        "mrs %[rh], " KEY "KeyHi_EL1\n" \
        "orr x9, %[ctl], %[enable]\n" \
        "msr SCTLR_EL1, x9\n" \
        "isb\n" \
        "mov %[sig], %[ptr]\n" \
        SIGN " %[sig], %[mod]\n" \
        "mov %[restored], %[sig]\n" \
        AUTH " %[restored], %[mod]\n" \
        "msr " KEY "KeyLo_EL1, %[ol]\n" \
        "msr " KEY "KeyHi_EL1, %[oh]\n" \
        "msr SCTLR_EL1, %[ctl]\n" \
        "isb\n" \
        : [ol] "=&r"(oldLow), [oh] "=&r"(oldHigh), [ctl] "=&r"(control), \
          [sig] "=&r"(signedPointer), [restored] "=&r"(restored), \
          [rl] "=&r"(readLow), [rh] "=&r"(readHigh) \
        : [lo] "r"(low), [hi] "r"(high), [ptr] "r"(pointer), [mod] "r"(modifier), \
          [enable] "r"((1ULL << 31) | (1ULL << 30) | (1ULL << 27) | (1ULL << 13)) \
        : "x9", "memory"); \
    if (readLow == low && readHigh == high) result->keys |= BIT; \
    if (signedPointer != pointer && restored == pointer) result->authenticated |= BIT; \
} while (0)

static void marbleProbePac(struct MarblePacResult *result) {
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(result->mpidr));
    __asm__ volatile("mrs %0, ID_AA64ISAR1_EL1" : "=r"(result->isar1));
    __asm__ volatile("mrs %0, S3_0_C0_C6_2" : "=r"(result->isar2));
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(result->currentEL));
    result->visited = 1;
    int address = ((result->isar1 >> 4) & 0xff) || ((result->isar2 >> 12) & 0xf);
    int generic = ((result->isar1 >> 24) & 0xff) || ((result->isar2 >> 8) & 0xf);
    if (!address || !generic || result->currentEL != 4) return;
    unsigned long long savedDaif;
    __asm__ volatile("mrs %0, DAIF\nmsr daifset, #15" : "=r"(savedDaif) :: "memory");
    TEST_ADDRESS_KEY("APIA", "pacia", "autia", 1);
    TEST_ADDRESS_KEY("APIB", "pacib", "autib", 2);
    TEST_ADDRESS_KEY("APDA", "pacda", "autda", 4);
    TEST_ADDRESS_KEY("APDB", "pacdb", "autdb", 8);
    unsigned long long oldLow, oldHigh, readLow, readHigh, first, second;
    const unsigned long long low = 0x91b37519c2e7486dULL, high = 0xf4a68d2b903157ceULL;
    const unsigned long long input = 0x72830519ULL, other = 0x72830520ULL;
    __asm__ volatile(
        ".arch_extension pauth\n"
        "mrs %[ol], APGAKeyLo_EL1\n"
        "mrs %[oh], APGAKeyHi_EL1\n"
        "msr APGAKeyLo_EL1, %[lo]\n"
        "msr APGAKeyHi_EL1, %[hi]\n"
        "isb\n"
        "mrs %[rl], APGAKeyLo_EL1\n"
        "mrs %[rh], APGAKeyHi_EL1\n"
        "pacga %[first], %[input], %[lo]\n"
        "pacga %[second], %[other], %[lo]\n"
        "msr APGAKeyLo_EL1, %[ol]\n"
        "msr APGAKeyHi_EL1, %[oh]\n"
        "isb\n"
        : [ol] "=&r"(oldLow), [oh] "=&r"(oldHigh), [rl] "=&r"(readLow),
          [rh] "=&r"(readHigh), [first] "=&r"(first), [second] "=&r"(second)
        : [lo] "r"(low), [hi] "r"(high), [input] "r"(input), [other] "r"(other)
        : "memory");
    if (readLow == low && readHigh == high) result->keys |= 16;
    if (first != second) result->authenticated |= 16;
    __asm__ volatile("msr DAIF, %0" :: "r"(savedDaif) : "memory");
}

#undef TEST_ADDRESS_KEY
