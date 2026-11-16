// What Metal's `copyFromTexture:toBuffer:` reads out of an IOSurface plane whose GPU rows share
// bytes, and whether it is the row copy's answer.
//
// `linear_plan.rs`'s **Rows that overlap** settles the write direction: Metal takes a plane at its
// `bytesPerRow` rounded down to 16, so a plane whose pitch covers its own row but whose rounded
// pitch falls below it has consecutive GPU rows 1 to 15 bytes apart in each other, and which row's
// bytes survive a *write* has no single rule (423 processes: alternating by row parity at a pitch
// of 16, three whole-plane answers over 23 processes for one shape). A *read* can have no such
// race: two rows that overlap are read, not written, and the question is only which plane byte each
// destination byte comes from.
//
// So this measures the read alone, over shapes that share bytes and controls that do not:
//
//   1. whether Metal makes the texture at all over a plane whose pitch is not a multiple of 16.
//      A surface with a plane dictionary is the only form it can be asked over: Metal's own
//      descriptor validation ends the process on one without ("IOSurface texture: bytesPerRow
//      (20) must be aligned to 16 bytes", `_mtlValidateStrideTextureParameters`, with no debug
//      layer set), which is the runtime's `Invalid(RowPitchAlignment)` for a flat surface, so the
//      flat form runs only where the pitch is aligned and shares nothing;
//   2. whether IOSurface honours such a `bytesPerRow`, read back off the surface;
//   3. for every destination texel, which plane word Metal read it from, against the row copy's
//      answer: destination row `y` from plane byte `max(bytesPerRow & ~15, 16) * y`.
//
// Every 4-byte word of the plane holds `0xB0000000 | k` for its own word index `k`, so a
// destination word names the plane word Metal read, not merely that it differs; the destination
// starts as `0xEEEEEEEE`, so an untouched word says so. One shape a process, the shape's index in
// argv[1], since what Metal answers for a plane whose rows overlap was process-dependent for
// writes.
//
// Nothing here writes a surface's bytes from the GPU, so no step can leave the plane racing.
//
// clang -fobjc-arc -framework Metal -framework Foundation -framework IOSurface \
//     -o overlapping_plane_row_reads overlapping_plane_row_reads.m

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

// The word every destination word starts as, which no plane word can be.
static const uint32_t kUntouched = 0xEEEEEEEEu;
// The high bits of a plane word, so a plane word is never mistaken for an untouched one.
static const uint32_t kPlaneTag = 0xB0000000u;

// One shape: `w` x `h` BGRA8Unorm texels of a plane `bpr` bytes a row.
struct Shape {
    size_t w;
    size_t h;
    size_t bpr;
};

static const struct Shape kShapes[] = {
    // GPU rows that share bytes: the rounded pitch falls below the row.
    {5, 4, 20},  // pitch 16, row 20, 4 bytes shared
    {5, 2, 20},  // the same over two rows
    {5, 4, 28},  // pitch 16, row 20: the CPU's rows 28 apart, the GPU's 16
    {9, 4, 36},  // pitch 32, row 36, 4 bytes shared
    {7, 5, 28},  // pitch 16, row 28, 12 bytes shared
    // Controls whose GPU rows share nothing.
    {3, 3, 20},  // pitch 16, row 12
    {4, 4, 16},  // pitch 16, row 16
};
static const size_t kShapeCount = sizeof(kShapes) / sizeof(kShapes[0]);

// Where Metal's GPU is said to take a plane's rows: its `bytesPerRow` rounded down to 16, or 16
// where that is 0.
static size_t gpuPitch(size_t bpr) {
    size_t down = bpr - bpr % 16;
    return down == 0 ? 16 : down;
}

// A surface of one `w` x `h` BGRA plane `bpr` bytes a row, with a plane dictionary (`planar`) or
// without, every word naming its own index.
static IOSurfaceRef makeSurface(size_t w, size_t h, size_t bpr, BOOL planar) {
    size_t length = bpr * h;
    NSMutableDictionary *properties = [@{
        (__bridge NSString *)kIOSurfaceWidth : @(w),
        (__bridge NSString *)kIOSurfaceHeight : @(h),
        (__bridge NSString *)kIOSurfaceBytesPerElement : @(4),
        (__bridge NSString *)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
        (__bridge NSString *)kIOSurfaceAllocSize : @(length),
    } mutableCopy];
    if (planar) {
        properties[(__bridge NSString *)kIOSurfacePlaneInfo] = @[ @{
            (__bridge NSString *)kIOSurfacePlaneWidth : @(w),
            (__bridge NSString *)kIOSurfacePlaneHeight : @(h),
            (__bridge NSString *)kIOSurfacePlaneBytesPerElement : @(4),
            (__bridge NSString *)kIOSurfacePlaneBytesPerRow : @(bpr),
            (__bridge NSString *)kIOSurfacePlaneOffset : @(0),
            (__bridge NSString *)kIOSurfacePlaneSize : @(length),
        } ];
    } else {
        properties[(__bridge NSString *)kIOSurfaceBytesPerRow] = @(bpr);
    }
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (surface == NULL) { return NULL; }
    IOSurfaceLock(surface, 0, NULL);
    uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(surface);
    size_t alloc = IOSurfaceGetAllocSize(surface);
    memset(base, 0, alloc);
    for (size_t k = 0; k * 4 + 4 <= alloc; k += 1) {
        uint32_t word = kPlaneTag | (uint32_t)k;
        memcpy(base + k * 4, &word, 4);
    }
    IOSurfaceUnlock(surface, 0, NULL);
    return surface;
}

// Run one shape over one surface form; 0 where every destination texel held the row copy's answer,
// else the mismatches.
static int readShape(id<MTLDevice> device, id<MTLCommandQueue> queue, const struct Shape *shape,
                     BOOL planar) {
    const char *form = planar ? "planar" : "flat";
    size_t w = shape->w, h = shape->h, bpr = shape->bpr;
    size_t row = w * 4, pitch = gpuPitch(bpr);
    IOSurfaceRef surface = makeSurface(w, h, bpr, planar);
    if (surface == NULL) {
        printf("%s %zux%zu at %zu: IOSurfaceCreate answered nil\n", form, w, h, bpr);
        return 1;
    }
    size_t surfaceRow = planar ? IOSurfaceGetBytesPerRowOfPlane(surface, 0)
                               : IOSurfaceGetBytesPerRow(surface);
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                          width:w
                                                         height:h
                                                      mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d iosurface:surface plane:0];
    if (texture == nil) {
        printf("%s %zux%zu at %zu (surface row %zu): no texture\n", form, w, h, bpr, surfaceRow);
        CFRelease(surface);
        return 1;
    }
    size_t words = w * h;
    id<MTLBuffer> out = [device newBufferWithLength:words * 4 options:MTLResourceStorageModeShared];
    uint32_t *got = (uint32_t *)out.contents;
    for (size_t i = 0; i < words; i += 1) { got[i] = kUntouched; }
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:out
        destinationOffset:0
   destinationBytesPerRow:row
 destinationBytesPerImage:row * h];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted) {
        printf("%s %zux%zu at %zu: command buffer status %lu\n", form, w, h, bpr,
               (unsigned long)commands.status);
        CFRelease(surface);
        return 1;
    }
    int wrong = 0;
    for (size_t y = 0; y < h; y += 1) {
        for (size_t x = 0; x < w; x += 1) {
            uint32_t have = got[y * w + x];
            // The row copy reads destination row `y` from plane byte `pitch * y`.
            uint32_t want = kPlaneTag | (uint32_t)((pitch * y + x * 4) / 4);
            if (have == want) { continue; }
            wrong += 1;
            if (have == kUntouched) {
                printf("%s %zux%zu at %zu: texel (%zu, %zu) untouched, the row copy's plane word "
                       "%u\n", form, w, h, bpr, x, y, want & 0x0fffffffu);
            } else if ((have & 0xf0000000u) == kPlaneTag) {
                printf("%s %zux%zu at %zu: texel (%zu, %zu) from plane word %u, the row copy's "
                       "%u\n", form, w, h, bpr, x, y, have & 0x0fffffffu, want & 0x0fffffffu);
            } else {
                printf("%s %zux%zu at %zu: texel (%zu, %zu) is %#010x, no plane word; the row "
                       "copy's %u\n", form, w, h, bpr, x, y, have, want & 0x0fffffffu);
            }
        }
    }
    printf("%s %zux%zu at %zu (surface row %zu, GPU pitch %zu, row %zu, shared %zu): %s\n", form,
           w, h, bpr, surfaceRow, pitch, row, row > pitch ? row - pitch : 0,
           wrong == 0 ? "every texel the row copy's" : "DIFFERS");
    CFRelease(surface);
    return wrong == 0 ? 0 : 1;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc < 2) {
            printf("usage: %s <shape 0..%zu>\n", argv[0], kShapeCount - 1);
            return 2;
        }
        size_t index = (size_t)atoi(argv[1]);
        if (index >= kShapeCount) {
            printf("shape %zu is past %zu\n", index, kShapeCount - 1);
            return 2;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            printf("no Metal device\n");
            return 2;
        }
        printf("device %s, shape %zu, pid %d\n", device.name.UTF8String, index, getpid());
        id<MTLCommandQueue> queue = [device newCommandQueue];
        int wrong = 0;
        wrong += readShape(device, queue, &kShapes[index], YES);
        // A flat surface at a pitch off 16 is Metal's own assert, not an answer to read.
        if (kShapes[index].bpr % 16 == 0) {
            wrong += readShape(device, queue, &kShapes[index], NO);
        } else {
            printf("flat %zux%zu at %zu: not asked, Metal's descriptor validation asserts on a "
                   "pitch off 16\n", kShapes[index].w, kShapes[index].h, kShapes[index].bpr);
        }
        return wrong == 0 ? 0 : 1;
    }
}
