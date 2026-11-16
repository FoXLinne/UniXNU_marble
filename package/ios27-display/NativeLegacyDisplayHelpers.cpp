// NativeLegacyDisplayHelpers.cpp - iOS 27.2 (24B119) IOSurface/IOSurfaceRoot
// display helper adapter for GopFramebuffer's NativeLegacyDisplay and
// package/MarbleDisplay.cpp rejection/telemetry paths.
//
// Each native_legacy_* helper forwards to a real function in the original
// 24B119 kernelcache fileset, reached through a BTI landing-pad stub that
// branches to the qualified original virtual address - the same binding
// scheme build_ios27_drivers.py emit_bindings() produces for code symbols.
//
// Target kernelcache: kernelcache.decompressed, sha256
// 8cc8eccc429aa95397aef0ffe64b226e71b014d84190c1e3f84cf5c270a6e6bc
// (iPhone12,1 iOS 27.2, com.apple.iokit.IOMobileGraphicsFamily +
//  com.apple.iokit.IOSurface + com.apple.kernel members)
//
// Original function evidence (see records/ios27-24B119/display-helpers.md):
//
//   IOSurface field getters (com.apple.iokit.IOSurface trivial accessors,
//   each `bti c; ldr* [x0,#imm]; ret` - byte-identical check patterns to
//   the sheng 24A446 reference profile):
//     descriptor = 0xfffffff009be5bf4  ldr x0,[x0,#0x30]  - swap teardown
//       path at 0xfffffff009a71bd4 unlocks (vt+0xe0) and releases (vt+0x28)
//       the object fetched through this getter; it is the surface's locked
//       memory descriptor.
//     width      = 0xfffffff009be5cf8  ldr x0,[x0,#0x58]  - swap validator
//       at 0xfffffff009a762f0 bounds-checks src_x + src_width <= this field.
//     height     = 0xfffffff009be5d04  ldr x0,[x0,#0x60]  - paired bound for
//       src_y + src_height at 0xfffffff009a76390.
//     stride     = 0xfffffff009be5d1c  ldr x0,[x0,#0x68]  - reported as
//       "src_stride" by the original swap-info debug serializer
//       0xfffffff009a7bad4.
//     format     = 0xfffffff009be5ce0  ldr w0,[x0,#0x80]  - compared against
//       the pixel-format constants in the format dispatch path.
//     planes     = 0xfffffff009be5d4c  ldr w0,[x0,#0x98]  - zero/nonzero gate
//       selects per-plane stride queries in the same serializer.
//
//   IOMobileFramebufferLegacy::setDisplayRefreshProperties()
//       = 0xfffffff009a7e3d4 (IOMobileGraphicsFamily). Only function
//       referencing the "IOMFBDisplayRefresh" cstring; calls
//       getRefreshAttributes via authenticated instance slot +0x9f8
//       (diversity 0xdcfc) and setProperty via slot +0xc8
//       (diversity 0xff53) publishing the serialized refresh dict.
//
//   Vsync client notify sender
//       = 0xfffffff009a7a200 (IOMobileGraphicsFamily). Called at
//       0xfffffff009a72ce4 only when the vsync-client queue field
//       display+0x1ab8 is non-empty, with (display, [+0x100],
//       [+0x108], [+0x110]) - timestamp, continuousTime, interval.
//       Iterates the client list and invokes each client's virtual
//       callback at vt+0x7e8 (diversity 0x9562). Signature
//       (display, uint64_t, uint64_t, uint64_t) matches the
//       native_legacy_vsync_notify declaration.
//
//   Default-surface factory: reproduces the body of the target
//   IOMobileFramebufferLegacy::create_default_fb_surface
//   (0xfffffff009a780c4, instance vtable slot 316) exactly, substituting
//   the caller's (root, width, height) and the BGRA pixel format the
//   original reads from its params block at display+0x1c40:
//     1. dict  = 0xfffffff00a878000(capacity=1)   [com.apple.kernel]
//     2. region= 0xfffffff00a8a9920("PurpleGfxMem")[com.apple.kernel]
//     3. dict->setObject(key, region) via authenticated slot +0x100
//        (diversity 0xc323); key = **(0xfffffff008030f70) as in original.
//        region released after the call (vt+0x28, diversity 0x3a87).
//     4. surface = 0xfffffff009bf75b8(root, 0, dict)
//        [com.apple.iokit.IOSurface create thunk, w3=0 -> 0xfffffff009bf7544]
//        dict released afterwards.
//     5. surface region = 0xfffffff009bf7bb4(root, "PurpleGfxMem")
//        then 0xfffffff009bedd54(surface, region) [this+0x28 setter thunk].
//     6. 0xfffffff009bedfd0(surface, format)  [str w1,[x0,#0x80]; ret]
//        format 0x42475241 is the same constant the original gates on; a
//        non-BGRA format would additionally call the +0x1c8/+0x1cc path.
//     7. 0xfffffff009bedd60(surface, 0) and
//        0xfffffff009bedfec(surface, 1)     [the original's two flag calls]
//     8. layout = 0xfffffff009a74928(surface, width, height, params)
//        [IOMobileGraphicsFamily layout_buffers; params[0x48]=0 selects
//        0xfffffff009a74b68, which aligns each width*elementBytes row
//        to 128 bytes and the plane allocation to the target page size].
//        params[0]=format, params[1]=planes, params[2]=elems, rest zero -
//        the same 80-byte struct the original passes at display+0x1c40.
//   On layout failure the surface is released and NULL is returned, so the
//   helper never yields a stale create_default_fb_surface residue.
//
//   surface_allocation: the original IOSurface exposes no allocation-size
//   getter; the bound descriptor's getLength() supplies it (same rule the
//   24A446 adapter used).
//
// All originals keep their expected PAC authentication boundary: the
// landing pads branch directly to each function's own `bti c`/pacibsp
// prologue, and the only virtual call performed inside the adapter (the
// options-dict setObject) reproduces the original autda/blraa sequence
// with the observed diversity values.

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSMetaClass.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>

// BTI landing pads to original 24B119 code addresses, byte-identical in
// shape to the emit_bindings() code stubs.
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl __orig_surf_descriptor\n"
    "__orig_surf_descriptor:\n"
    "\thint #34\n"
    "\tmovz x16, #0x5bf4\n"
    "\tmovk x16, #0x09be, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_width\n"
    "__orig_surf_width:\n"
    "\thint #34\n"
    "\tmovz x16, #0x5cf8\n"
    "\tmovk x16, #0x09be, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_height\n"
    "__orig_surf_height:\n"
    "\thint #34\n"
    "\tmovz x16, #0x5d04\n"
    "\tmovk x16, #0x09be, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_stride\n"
    "__orig_surf_stride:\n"
    "\thint #34\n"
    "\tmovz x16, #0x5d1c\n"
    "\tmovk x16, #0x09be, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_format\n"
    "__orig_surf_format:\n"
    "\thint #34\n"
    "\tmovz x16, #0x5ce0\n"
    "\tmovk x16, #0x09be, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_planes\n"
    "__orig_surf_planes:\n"
    "\thint #34\n"
    "\tmovz x16, #0x5d4c\n"
    "\tmovk x16, #0x09be, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_create\n"
    "__orig_surf_create:\n"
    "\thint #34\n"
    "\tmovz x16, #0x75b8\n"
    "\tmovk x16, #0x09bf, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_region\n"
    "__orig_surf_region:\n"
    "\thint #34\n"
    "\tmovz x16, #0x7bb4\n"
    "\tmovk x16, #0x09bf, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_setregion\n"
    "__orig_surf_setregion:\n"
    "\thint #34\n"
    "\tmovz x16, #0xdd54\n"
    "\tmovk x16, #0x09be, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_setformat\n"
    "__orig_surf_setformat:\n"
    "\thint #34\n"
    "\tmovz x16, #0xdfd0\n"
    "\tmovk x16, #0x09be, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_setx\n"
    "__orig_surf_setx:\n"
    "\thint #34\n"
    "\tmovz x16, #0xdd60\n"
    "\tmovk x16, #0x09be, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_sety\n"
    "__orig_surf_sety:\n"
    "\thint #34\n"
    "\tmovz x16, #0xdfec\n"
    "\tmovk x16, #0x09be, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_surf_layout\n"
    "__orig_surf_layout:\n"
    "\thint #34\n"
    "\tmovz x16, #0x4928\n"
    "\tmovk x16, #0x09a7, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_vsync_notify\n"
    "__orig_vsync_notify:\n"
    "\thint #34\n"
    "\tmovz x16, #0xa200\n"
    "\tmovk x16, #0x09a7, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
    ".p2align 2\n"
    ".globl __orig_refresh_props\n"
    "__orig_refresh_props:\n"
    "\thint #34\n"
    "\tmovz x16, #0xe3d4\n"
    "\tmovk x16, #0x09a7, lsl #16\n"
    "\tmovk x16, #0xfff0, lsl #32\n"
    "\tmovk x16, #0xffff, lsl #48\n"
    "\tbr x16\n"
);

__asm__(
    ".p2align 2\n.globl __orig_surf_register_region\n__orig_surf_register_region:\n"
    "hint #34\nmovz x16, #0x7cb4\nmovk x16, #0x09bf, lsl #16\n"
    "movk x16, #0xfff0, lsl #32\nmovk x16, #0xffff, lsl #48\nbr x16\n");
extern "C" uint32_t _orig_surf_register_region(OSObject *, const void *, const char *);

__asm__(
 ".p2align 2\n.globl __orig_region_name\n__orig_region_name:\nhint #34\n"
 "movz x16,#0x7df8\nmovk x16,#0x09bf,lsl #16\nmovk x16,#0xfff0,lsl #32\nmovk x16,#0xffff,lsl #48\nbr x16\n"
 ".p2align 2\n.globl __orig_region_init\n__orig_region_init:\nhint #34\npacibsp\n"
 "sub sp,sp,#0x50\nstp x24,x23,[sp,#0x10]\nstp x22,x21,[sp,#0x20]\nstp x20,x19,[sp,#0x30]\nstp x29,x30,[sp,#0x40]\nadd x29,sp,#0x40\n"
 "mov x23,x5\nmov x21,x4\nmov x22,x3\nmov x20,x0\nbl _marble_region_base_init\n"
 "cbz w0,1f\nmov x0,x22\nbl _marble_region_retain_provider\nmov x24,x0\nmov x19,xzr\n"
 // The verified original continuation owns device-memory/map/extent fields.
 // RET keeps BTYPE zero for the interior continuation; original RETAB uses
 // this matching prologue and the original caller's signed return address.
 "movz x16,#0x3618\nmovk x16,#0x09bf,lsl #16\nmovk x16,#0xfff0,lsl #32\nmovk x16,#0xffff,lsl #48\nret x16\n"
 "1: mov w0,wzr\nldp x29,x30,[sp,#0x40]\nldp x20,x19,[sp,#0x30]\nldp x22,x21,[sp,#0x20]\nldp x24,x23,[sp,#0x10]\nadd sp,sp,#0x50\nretab\n"
 ".p2align 2\n.globl _marble_region_base_init\n_marble_region_base_init:\nhint #34\n"
 "movz x16,#0x4268\nmovk x16,#0x09bf,lsl #16\nmovk x16,#0xfff0,lsl #32\nmovk x16,#0xffff,lsl #48\nbr x16\n"
 ".p2align 2\n.globl __orig_region_register\n__orig_region_register:\nhint #34\n"
 "movz x16,#0x7924\nmovk x16,#0x09bf,lsl #16\nmovk x16,#0xfff0,lsl #32\nmovk x16,#0xffff,lsl #48\nbr x16\n");
extern "C" IOService *marble_region_retain_provider(IOService *provider) { provider->retain(); return provider; }
extern "C" OSObject *_orig_region_name(const char *);
extern "C" bool _orig_region_init(OSObject *,OSObject *,OSObject *,IOService *,uint32_t,uint32_t);
extern "C" void _orig_region_register(OSObject *,OSObject *);

__asm__(
 ".p2align 2\n.globl __orig_surf_allocate\n__orig_surf_allocate:\nhint #34\n"
 "movz x16,#0x1fb4\nmovk x16,#0x09be,lsl #16\nmovk x16,#0xfff0,lsl #32\nmovk x16,#0xffff,lsl #48\nbr x16\n");
extern "C" void _orig_surf_allocate(OSObject *);

// Prototypes of the original entry points behind the stubs. Original
// signatures flattened to plain C linkage - identical arm64 ABI.
extern "C" IOMemoryDescriptor *_orig_surf_descriptor(OSObject *surface);
extern "C" uint64_t _orig_surf_width(OSObject *surface);
extern "C" uint64_t _orig_surf_height(OSObject *surface);
extern "C" uint64_t _orig_surf_stride(OSObject *surface);
extern "C" uint32_t _orig_surf_format(OSObject *surface);
extern "C" uint32_t _orig_surf_planes(OSObject *surface);
extern "C" OSObject *_orig_surf_opts(uint32_t capacity) {
    return OSDictionary::withCapacity(capacity);
}
extern "C" OSObject *_orig_surf_memlookup(const char *name) {
    return OSString::withCString(name);
}
extern "C" OSObject *_orig_surf_create(OSObject *root, void *ctx,
                                       OSObject *opts);
extern "C" OSObject *_orig_surf_region(OSObject *root, const char *name);
extern "C" void _orig_surf_setregion(OSObject *surface, OSObject *region);
extern "C" void _orig_surf_setformat(OSObject *surface, uint32_t format);
extern "C" void _orig_surf_setx(OSObject *surface, uint32_t value);
extern "C" void _orig_surf_sety(OSObject *surface, uint32_t value);
extern "C" uint32_t _orig_surf_layout(OSObject *surface, uint32_t width,
                                      uint32_t height, const void *params);
extern "C" void _orig_vsync_notify(void *display, uint64_t timestamp,
                                   uint64_t continuousTime,
                                   uint64_t interval);
extern "C" bool _orig_refresh_props(void *display);
extern "C" void _orig_opts_setObject(OSObject *dict, OSObject *value) {
    OSDictionary *options=OSDynamicCast(OSDictionary,dict);
    if (options) options->setObject("IOSurfaceMemoryRegion",value);
}

// The format word the original factory reads from display+0x1c40 for the
// default GOP surface: four-cc BGRA (0x42475241). The original skips its
// extra format-extra path precisely for this value.
static const uint32_t kOrigDefaultFormat = 0x42475241;

extern "C" OSObject *
native_legacy_default_surface_create(OSObject *root, uint32_t width,
                                     uint32_t height, IOService *provider) {
    if (!root || !provider || !width || !height || width > 16384 || height > 16384) {
        return nullptr;
    }
    OSObject *memoryRegion=_orig_surf_region(root,"PurpleGfxMem");
    if (!memoryRegion) {
        OSObject *region=OSMetaClass::allocClassWithName("IOSurfaceDeviceMemoryRegion");
        OSObject *name=_orig_region_name("PurpleGfxMem");
        if (region && name && _orig_region_init(region,root,name,provider,1,0)) {
            // Root uses requested (+0x168) and completed (+0x16c) region
            // generations. Publish this real registration request under its
            // lock; the original registrar publishes completion and wakeup.
            auto *bytes=reinterpret_cast<unsigned char *>(root);
            IOLock *lock=nullptr;
            memcpy(&lock,bytes+0x140,sizeof(lock));
            if (lock) {
                IOLockLock(lock);
                uint32_t requested;
                memcpy(&requested,bytes+0x168,4);++requested;
                memcpy(bytes+0x168,&requested,4);
                IOLockUnlock(lock);
                _orig_region_register(root,region);
            }
        }
        if (name) name->release();
        if (region) region->release();
        memoryRegion=_orig_surf_region(root,"PurpleGfxMem");
    }
    const char *regionName="PurpleGfxMem";
    if (!memoryRegion) {
        memoryRegion=_orig_surf_region(root,"ScalableMemory");
        regionName="ScalableMemory";
    }
    if (!memoryRegion) {
        IOLog("MarbleDisplay: IOSurfaceRoot memory region registration pending\n");
        return nullptr;
    }
    IOLog("MarbleDisplay: default surface allocator %s\n",regionName);
    OSObject *opts = _orig_surf_opts(1);
    if (!opts) {
        memoryRegion->release();
        return nullptr;
    }
    if (OSObject *region = _orig_surf_memlookup(regionName)) {
        _orig_opts_setObject(opts, region);
        region->release();
    }
    OSObject *surface = _orig_surf_create(root, nullptr, opts);
    opts->release();
    if (!surface) {
        memoryRegion->release();
        return nullptr;
    }
    _orig_surf_setregion(surface,memoryRegion);
    memoryRegion->release();
    _orig_surf_setformat(surface, kOrigDefaultFormat);
    _orig_surf_setx(surface, 0);
    _orig_surf_sety(surface, 1);
    // 80-byte layout params, matching the display+0x1c40 struct the
    // original passes to layout_buffers: format @0, planes @4, elems @8,
    // flag byte @0x48 = 0 selects the linear layout branch at 0x9a74b68.
    uint64_t params[10] = {};
    uint32_t *fields = reinterpret_cast<uint32_t *>(params);
    fields[0] = kOrigDefaultFormat;
    fields[1] = 1;  // one plane
    fields[2] = 4;  // four bytes per element (BGRA8)
    if (!_orig_surf_layout(surface, width, height, params)) {
        IOLog("MarbleDisplay: default surface layout failed %ux%u\n",width,height);
        surface->release();
        return nullptr;
    }
    _orig_surf_allocate(surface);
    IOMemoryDescriptor *memory = _orig_surf_descriptor(surface);
    const uint64_t stride = _orig_surf_stride(surface);
    IOLog("MarbleDisplay: default surface memory %p dimensions %llux%llu stride %llu planes %u\n",
          memory,_orig_surf_width(surface),_orig_surf_height(surface),stride,_orig_surf_planes(surface));
    if (!memory || _orig_surf_width(surface) != width ||
        _orig_surf_height(surface) != height ||
        _orig_surf_format(surface) != kOrigDefaultFormat ||
        _orig_surf_planes(surface) != 0 || stride < uint64_t(width) * 4 ||
        stride > memory->getLength() / height) {
        surface->release();
        return nullptr;
    }
    return surface;
}

extern "C" bool native_legacy_set_refresh_properties(void *display) {
    // IOMobileFramebufferLegacy::setDisplayRefreshProperties - builds the
    // refresh dict from getRefreshAttributes and publishes it as
    // "IOMFBDisplayRefresh".
    return display ? _orig_refresh_props(display) : false;
}

extern "C" void native_legacy_vsync_notify(void *display, uint64_t timestamp,
                                           uint64_t continuousTime,
                                           uint64_t interval) {
    // The original vsync-client notify sender gated on display+0x1ab8.
    if (display) {
        _orig_vsync_notify(display, timestamp, continuousTime, interval);
    }
}

extern "C" IOMemoryDescriptor *
native_legacy_surface_descriptor(OSObject *surface) {
    return surface ? _orig_surf_descriptor(surface) : nullptr;
}

extern "C" uint64_t native_legacy_surface_width(OSObject *surface) {
    return surface ? _orig_surf_width(surface) : 0;
}

extern "C" uint64_t native_legacy_surface_height(OSObject *surface) {
    return surface ? _orig_surf_height(surface) : 0;
}

extern "C" uint64_t native_legacy_surface_stride(OSObject *surface) {
    return surface ? _orig_surf_stride(surface) : 0;
}

extern "C" uint64_t native_legacy_surface_allocation(OSObject *surface) {
    // No allocation-size getter exists on the original IOSurface; the
    // bound memory descriptor's getLength() supplies it.
    IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surface);
    return memory ? memory->getLength() : 0;
}

extern "C" uint32_t native_legacy_surface_format(OSObject *surface) {
    return surface ? _orig_surf_format(surface) : 0;
}

extern "C" uint32_t native_legacy_surface_planes(OSObject *surface) {
    return surface ? _orig_surf_planes(surface) : 0;
}
