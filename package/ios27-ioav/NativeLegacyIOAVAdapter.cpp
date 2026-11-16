// NativeLegacyIOAVAdapter.cpp - iOS 27.2 (24B119) IOAV/IOMobileFramebuffer
// helper adapter for GopFramebuffer's NativeLegacyDisplay cross-system paths.
//
// Each native_legacy_* helper forwards to a real function in the original
// 24B119 kernelcache fileset, reached through a BTI landing-pad stub that
// branches to the qualified original virtual address. This is the same
// binding scheme build_ios27_drivers.py emit_bindings() produces for code
// symbols: absolute-VA materialization into x16, `br x16`, target function
// opens with `bti c`.
//
// Target kernelcache: kernelcache.decompressed, sha256
// 8cc8eccc429aa95397aef0ffe64b226e71b014d84190c1e3f84cf5c270a6e6bc
//
// Original function evidence (see records/ios27-24B119/ioav.md):
//
//   IOAVVideoTimingElement::withData(data, standardType, timingStandard)
//       = 0xfffffff0097ad798 in com.apple.iokit.IOAVFamily
//       Static factory: meta-alloc via metaclass vtable slot 13, virtual
//       init via instance vtable slot 22 with (data, stdType, timingStd),
//       releases on failure. Internal EDID callers at 0xfffffff0097c5928.
//       Class registration sets w3=176 (instance size) for metaclass
//       0xfffffff00afa7878, object vtable 0xfffffff007fa9f28.
//
//   IOAVVideoTimingElement::setColorElements(OSArray *)
//       = 0xfffffff0097aec88, instance vtable slot 28 (diversity 0x828c,
//       unique match for the IOMFBSwapInfo-style mangle of
//       IOAVVideoTimingElement::setColorElements). Retains the incoming
//       array, swaps field +0x70, releases the old object, notifies.
//
//   IOAVVideoColorElement::withCapabilities(7 scalars, dict, dict, dict, set)
//       = 0xfffffff0097acb18 in com.apple.iokit.IOAVFamily
//       Eleven-argument wrapper: packs the seven scalar arguments into a
//       stack capability structure and calls the six-argument worker
//       0xfffffff0097ac91c, which allocates and initializes the 112-byte
//       color element (metaclass 0xfffffff00afa7850, instance vtable
//       0xfffffff007fa9d68, registration w3=0x70).
//
//   IOMobileFramebufferLegacy::hotPlug_notify(uint64_t state)
//       = 0xfffffff009a7a4d4 in com.apple.iokit.IOMobileGraphicsFamily
//       Instance vtable slot 278, diversity 0x786c, unique match for
//       mangle IOMobileFramebufferLegacy::hotPlug_notify Ey. Real arm64e
//       function (bti, pacibsp prologue, state machine, notification).
//
// These helpers exist only for the optional macOS-parity path in
// NativeLegacyDisplay.cpp (NATIVE_LEGACY_MACOS). The iOS 27 default-surface
// baseline does not call them; this module compiles standalone either way.

#include <IOKit/IOReturn.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSObject.h>

// BTI landing pads to original 24B119 code addresses, byte-identical in
// shape to the emit_bindings() code stubs.
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl __orig_ioav_timing_withData\n"
    "__orig_ioav_timing_withData:\n"
    "\thint #34\n"                    // bti c
    "\tmovz x16, #0xd798\n"
    "\tmovk x16, #0x097a, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_ioav_timing_setColorElements\n"
    "__orig_ioav_timing_setColorElements:\n"
    "\thint #34\n"
    "\tmovz x16, #0xec88\n"
    "\tmovk x16, #0x097a, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_ioav_color_withCapabilities\n"
    "__orig_ioav_color_withCapabilities:\n"
    "\thint #34\n"
    "\tmovz x16, #0xcb18\n"
    "\tmovk x16, #0x097a, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_iomfb_hotPlug_notify\n"
    "__orig_iomfb_hotPlug_notify:\n"
    "\thint #34\n"
    "\tmovz x16, #0xa4d4\n"
    "\tmovk x16, #0x09a7, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n");

// Prototypes of the original entry points behind the stubs. Member-function
// signatures are flattened to (receiver, args...) - identical arm64 ABI.
extern "C" OSObject *_orig_ioav_timing_withData(const void *data,
                                              uint32_t standardType,
                                              uint32_t timingStandard);
extern "C" IOReturn _orig_ioav_timing_setColorElements(OSObject *timing,
                                                     OSArray *colors);
extern "C" OSObject *_orig_ioav_color_withCapabilities(
    uint32_t standardType, uint32_t depth, uint32_t encoding, uint32_t range,
    uint32_t eotf, uint32_t colorimetry, bool rgb, const void *d0,
    const void *d1, const void *d2, const void *d3);
extern "C" IOReturn _orig_iomfb_hotPlug_notify(void *display, uint64_t state);

extern "C" OSObject *native_legacy_ioav_timing(const void *data,
                                               uint32_t standardType,
                                               uint32_t timingStandard) {
    // IOAVVideoTimingElement::withData - retains on success, nullptr on
    // allocation or init failure, exactly as the original caller expects.
    return _orig_ioav_timing_withData(data, standardType, timingStandard);
}

extern "C" void native_legacy_ioav_timing_colors(OSObject *timing,
                                                 OSArray *colors) {
    // IOAVVideoTimingElement::setColorElements on the concrete element the
    // factory returned; the original method retains/replaces/notifies.
    if (timing) {
        _orig_ioav_timing_setColorElements(timing, colors);
    }
}

extern "C" OSObject *native_legacy_ioav_color(
    uint32_t standardType, uint32_t depth, uint32_t encoding, uint32_t range,
    uint32_t eotf, uint32_t colorimetry, bool rgb, const void *d0,
    const void *d1, const void *d2, const void *d3) {
    // IOAVVideoColorElement::withCapabilities - the original wrapper packs
    // the seven scalars into its capability structure itself.
    return _orig_ioav_color_withCapabilities(standardType, depth, encoding,
                                             range, eotf, colorimetry, rgb,
                                             d0, d1, d2, d3);
}

extern "C" void native_legacy_hotplug_notify(void *display, uint64_t state) {
    // IOMobileFramebufferLegacy::hotPlug_notify(uint64_t) - the original
    // gated state transition and client notification path.
    if (display) {
        _orig_iomfb_hotPlug_notify(display, state);
    }
}
