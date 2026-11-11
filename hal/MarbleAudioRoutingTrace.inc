// SPDX-License-Identifier: BSD-2-Clause-Patent
// 22H384 VirtualAudio diagnostic import tracing. Only the matched image's
// imported HAL calls and exception throws are observed; return values and
// exceptions continue through their original functions.
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <ptrauth.h>
#include <sys/mman.h>
static _Atomic uint32_t routingTraceCount;
static uint32_t routingTrace[128][8];
static uintptr_t routingBase;
static OSStatus (*originalGet)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *, void *);
static OSStatus (*originalSize)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static void (*originalThrow)(void *, void *, void (*)(void *));
static void traceRow(unsigned kind, unsigned object, const AudioObjectPropertyAddress *address, OSStatus status, uintptr_t pc) {
    unsigned n = atomic_fetch_add(&routingTraceCount, 1);
    if (n >= 128) return;
    routingTrace[n][0] = kind; routingTrace[n][1] = object;
    routingTrace[n][2] = address ? address->mSelector : 0;
    routingTrace[n][3] = address ? address->mScope : 0;
    routingTrace[n][4] = (uint32_t)status;
    routingTrace[n][5] = (uint32_t)(pc - routingBase);
}
static OSStatus routeGet(AudioObjectID object, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size, void *out) {
    OSStatus result = originalGet(object, a, qs, q, size, out);
    if (result) traceRow(1, object, a, result, (uintptr_t)__builtin_return_address(0));
    return result;
}
static OSStatus routeSize(AudioObjectID object, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size) {
    OSStatus result = originalSize(object, a, qs, q, size);
    if (result) traceRow(2, object, a, result, (uintptr_t)__builtin_return_address(0));
    return result;
}
__attribute__((noreturn)) static void routeThrow(void *exception, void *type, void (*destroy)(void *)) {
    traceRow(3, 0, NULL, 0, (uintptr_t)__builtin_return_address(0));
    originalThrow(exception, type, destroy);
    __builtin_unreachable();
}
struct RoutingImport { uintptr_t offset; uint16_t diversity; unsigned kind; };
#include "MarbleAudioRoutingImports.inc"
static void routingImage(const struct mach_header *header, intptr_t slide) {
    (void)slide;
    if (header->magic != MH_MAGIC_64) return;
    const uint8_t *command = (const uint8_t *)(header + 1);
    bool matched = false;
    const uint8_t expected[16] = {0x62,0xfa,0x1c,0x1b,0xed,0xd1,0x39,0xbd,0xad,0x79,0x84,0x4b,0x2f,0xc4,0x08,0x85};
    for (unsigned i = 0; i < header->ncmds; ++i) {
        const struct load_command *lc = (const void *)command;
        if (lc->cmd == LC_UUID && !memcmp(((const struct uuid_command *)command)->uuid, expected, 16)) matched = true;
        command += lc->cmdsize;
    }
    if (!matched) return;
    routingBase = (uintptr_t)header;
    originalGet = dlsym(RTLD_DEFAULT, "AudioObjectGetPropertyData");
    originalSize = dlsym(RTLD_DEFAULT, "AudioObjectGetPropertyDataSize");
    originalThrow = dlsym(RTLD_DEFAULT, "__cxa_throw");
    if (!originalGet || !originalSize || !originalThrow) return;
    void *hooks[] = {(void *)routeGet, (void *)routeSize, (void *)routeThrow};
    for (unsigned i = 0; i < sizeof(routingImports) / sizeof(routingImports[0]); ++i) {
        const struct RoutingImport *entry = &routingImports[i];
        void **slot = (void **)(routingBase + entry->offset);
        uintptr_t page = (uintptr_t)slot & ~(uintptr_t)(getpagesize() - 1);
        if (vm_protect(mach_task_self(), page, (vm_size_t)getpagesize(), false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY)) {
            traceRow(4, entry->kind, NULL, -1, routingBase + entry->offset); continue;
        }
        void *hook = ptrauth_strip(hooks[entry->kind], ptrauth_key_function_pointer);
        *slot = ptrauth_sign_unauthenticated(hook, ptrauth_key_asia,
            ptrauth_blend_discriminator(slot, entry->diversity));
        vm_protect(mach_task_self(), page, (vm_size_t)getpagesize(), false, VM_PROT_READ);
        traceRow(5, entry->kind, NULL, 0, routingBase + entry->offset);
    }
}
static void routingWatch(void) {
    static atomic_bool watched;
    if (atomic_exchange(&watched, true)) return;
    traceRow(6, 0, NULL, 0, 0);
    _dyld_register_func_for_add_image(routingImage);
}
