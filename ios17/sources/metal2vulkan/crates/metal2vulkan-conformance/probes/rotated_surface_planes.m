// What Metal stores into an IOSurface plane behind a framebuffer-only rotated texture.
//
// Settles the shape of the conformance port of the runtime lanes `rotated_textures` and
// `rotated_targets`, whose Metal rule was measured on an M4 this tree no longer has:
//
//   1. whether `MTLTextureDescriptor` answers the private `setRotation:` and `setFramebufferOnly:`
//      on this chip and OS, and what their signatures are;
//   2. which IOSurface extent Metal accepts for a rotated texture of `w x h` -- the texture's own
//      or the quarter turn's -- answered by `nil` rather than by an assert, so the sweep is safe;
//   3. where a pass's texel `(x, y)` lands in the plane at each rotation 0 to 4, re-measured here;
//   4. what a pass that loads the plane and draws only its top-left corner leaves at the texels it
//      does not draw, which is where rotation 4 is said to differ.
//
// Nothing here writes outside a surface's own bytes, so no step can fault the GPU. Descriptor
// sweeps may make Metal end this process; that is why they are a probe and not a case.
//
// clang -fobjc-arc -framework Metal -framework Foundation -framework IOSurface \
//     -o rotated_surface_planes rotated_surface_planes.m

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

// The private descriptor setters this port needs, declared so the calls are typed. Both are
// checked with `respondsToSelector:` before any use.
@interface MTLTextureDescriptor (M2VPrivateRotation)
- (void)setRotation:(NSUInteger)rotation;
- (NSUInteger)rotation;
- (void)setFramebufferOnly:(BOOL)framebufferOnly;
- (BOOL)framebufferOnly;
@end

// The texture's own private rotation getter, for the read-back the port states.
@protocol M2VRotatedTexture <MTLTexture>
- (NSUInteger)rotation;
@end

// The byte every plane starts as, so a stored texel is a byte that is no longer it.
static const uint8_t kPad = 0xAB;

// The fragment writes byte0 = x, byte1 = y, byte2 = 0x55, byte3 = 0xff of the pass position, so a
// plane byte names the pass texel it came from. BGRA8Unorm stores (b, g, r, a) in byte order, so
// blue carries x and green carries y.
static NSString *const kSource =
    @"#include <metal_stdlib>\n"
    @"using namespace metal;\n"
    @"struct Out { float4 position [[position]]; };\n"
    @"vertex Out tob_rect(uint vid [[vertex_id]], constant float4 &corners [[buffer(0)]]) {\n"
    @"  float2 p[6] = { float2(corners.x, corners.y), float2(corners.z, corners.y),\n"
    @"                  float2(corners.x, corners.w), float2(corners.z, corners.y),\n"
    @"                  float2(corners.z, corners.w), float2(corners.x, corners.w) };\n"
    @"  Out o; o.position = float4(p[vid], 0.0, 1.0); return o;\n"
    @"}\n"
    @"fragment float4 tob_name(Out in [[stage_in]]) {\n"
    @"  float x = floor(in.position.x), y = floor(in.position.y);\n"
    @"  return float4(0x55 / 255.0, y / 255.0, x / 255.0, 1.0);\n"
    @"}\n";

// An IOSurface of `w x h` BGRA8Unorm texels, its rows `bpr` bytes apart, every byte kPad.
static IOSurfaceRef makeSurface(size_t w, size_t h, size_t bpr) {
    NSDictionary *properties = @{
        (__bridge NSString *)kIOSurfaceWidth : @(w),
        (__bridge NSString *)kIOSurfaceHeight : @(h),
        (__bridge NSString *)kIOSurfaceBytesPerElement : @(4),
        (__bridge NSString *)kIOSurfaceBytesPerRow : @(bpr),
        (__bridge NSString *)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (surface == NULL) { return NULL; }
    IOSurfaceLock(surface, 0, NULL);
    memset(IOSurfaceGetBaseAddress(surface), kPad, IOSurfaceGetAllocSize(surface));
    IOSurfaceUnlock(surface, 0, NULL);
    return surface;
}

// The plane's bytes, after the GPU has finished with it.
static const uint8_t *planeBytes(IOSurfaceRef surface) {
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *bytes = (const uint8_t *)IOSurfaceGetBaseAddress(surface);
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    return bytes;
}

// Fill every texel of the plane with its own stored position, so a pass that keeps a texel leaves
// a byte naming where it already was.
static void nameStoredTexels(IOSurfaceRef surface, size_t sw, size_t sh, size_t bpr) {
    IOSurfaceLock(surface, 0, NULL);
    uint8_t *bytes = (uint8_t *)IOSurfaceGetBaseAddress(surface);
    for (size_t sy = 0; sy < sh; sy += 1) {
        for (size_t sx = 0; sx < sw; sx += 1) {
            uint8_t *at = bytes + sy * bpr + sx * 4;
            at[0] = (uint8_t)sx; at[1] = (uint8_t)sy; at[2] = 0x33; at[3] = 0x77;
        }
    }
    IOSurfaceUnlock(surface, 0, NULL);
}

// A framebuffer-only rotated texture of `w x h` over `surface`, or nil.
static id<MTLTexture> rotatedTexture(id<MTLDevice> device, IOSurfaceRef surface,
                                     NSUInteger rotation, size_t w, size_t h) {
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                          width:w
                                                         height:h
                                                      mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    [d setFramebufferOnly:YES];
    if (rotation != 0) { [d setRotation:rotation]; }
    return [device newTextureWithDescriptor:d iosurface:surface plane:0];
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) { fprintf(stderr, "no Metal device\n"); return 2; }
        printf("DEVICE %s\n", device.name.UTF8String);

        SEL rotationSel = @selector(setRotation:);
        SEL framebufferSel = @selector(setFramebufferOnly:);
        MTLTextureDescriptor *probe = [MTLTextureDescriptor new];
        printf("SELECTOR setRotation: %s\n", [probe respondsToSelector:rotationSel] ? "yes" : "no");
        printf("SELECTOR setFramebufferOnly: %s\n",
               [probe respondsToSelector:framebufferSel] ? "yes" : "no");
        if (![probe respondsToSelector:rotationSel] || ![probe respondsToSelector:framebufferSel]) {
            fprintf(stderr, "the descriptor answers neither private setter\n");
            return 2;
        }
        printf("SIGNATURE setRotation: %s\n",
               [[probe methodSignatureForSelector:rotationSel] description].UTF8String);
        printf("SIGNATURE setFramebufferOnly: %s\n",
               [[probe methodSignatureForSelector:framebufferSel] description].UTF8String);
        // What the descriptor reads back, so the case can state the same fields.
        [probe setRotation:3];
        [probe setFramebufferOnly:YES];
        printf("READBACK rotation %lu framebufferOnly %d\n",
               (unsigned long)[probe rotation], (int)[probe framebufferOnly]);

        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:kSource options:nil error:&error];
        if (library == nil) { fprintf(stderr, "library: %s\n", error.description.UTF8String); return 2; }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [library newFunctionWithName:@"tob_rect"];
        pd.fragmentFunction = [library newFunctionWithName:@"tob_name"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pipeline =
            [device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (pipeline == nil) { fprintf(stderr, "pipeline: %s\n", error.description.UTF8String); return 2; }
        id<MTLCommandQueue> queue = [device newCommandQueue];

        // The texture is 8x4, so every plane byte fits on the terminal and a quarter turn is
        // plainly a different extent from the texture's.
        const size_t w = 8, h = 4;

        // Which IOSurface extent Metal accepts for each rotation: the texture's own (w x h) or
        // the quarter turn's (h x w). A refusal is nil, which is safe to ask -- but only once the
        // stride rule below is respected, because Metal asserts on a stride it rejects and an
        // assert ends this process.
        //
        // MEASURED, the hard way: a surface of 4x8 at its own tight stride of 16 bytes behind an
        // 8x4 texture ends the process with
        //   _mtlValidateStrideTextureParameters: IOSurface texture: bytesPerRow (16) must be
        //   greater or equal to (32) bytes
        // So Metal reads the stride against the TEXTURE's width, not the surface's, whatever the
        // rotation. Every stride below is therefore at least `w * 4`.
        const size_t kMinStride = w * 4;
        for (NSUInteger rotation = 0; rotation <= 4; rotation += 1) {
            for (int turned = 0; turned <= 1; turned += 1) {
                const size_t sw = turned ? h : w, sh = turned ? w : h;
                size_t bpr = sw * 4;
                if (bpr < kMinStride) { bpr = kMinStride; }
                IOSurfaceRef surface = makeSurface(sw, sh, bpr);
                if (surface == NULL) { printf("EXTENT %lu %zux%zu: no surface\n",
                                              (unsigned long)rotation, sw, sh); continue; }
                printf("EXTENT rotation %lu surface %zux%zu stride %zu (surface stride %zu): ",
                       (unsigned long)rotation, sw, sh, bpr,
                       (size_t)IOSurfaceGetBytesPerRow(surface));
                id<MTLTexture> texture = rotatedTexture(device, surface, rotation, w, h);
                printf("%s", texture == nil ? "nil" : "made");
                if (texture != nil) {
                    printf(" (texture %lux%lu rotation %lu framebufferOnly %d)",
                           (unsigned long)texture.width, (unsigned long)texture.height,
                           (unsigned long)[(id<M2VRotatedTexture>)texture rotation],
                           (int)texture.framebufferOnly);
                }
                printf("\n");
                CFRelease(surface);
            }
        }

        // Where a pass's texels land, and what a corner draw leaves. For each rotation, the extent
        // the sweep above accepted is tried first and the other second, so one run answers whichever
        // Metal takes.
        for (NSUInteger rotation = 0; rotation <= 4; rotation += 1) {
            for (int turned = 0; turned <= 1; turned += 1) {
                const size_t sw = turned ? h : w, sh = turned ? w : h;
                // At or above `w * 4`, which is the stride rule measured above.
                size_t bpr = sw * 4 + 16;
                if (bpr < kMinStride) { bpr = kMinStride; }
                IOSurfaceRef surface = makeSurface(sw, sh, bpr);
                if (surface == NULL) { continue; }
                id<MTLTexture> texture = rotatedTexture(device, surface, rotation, w, h);
                if (texture == nil) { CFRelease(surface); continue; }

                // A pass that clears and draws every texel: each plane byte names the pass texel
                // Metal stored there.
                float whole[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
                id<MTLBuffer> corners = [device newBufferWithBytes:whole
                                                           length:sizeof(whole)
                                                          options:MTLResourceStorageModeShared];
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = texture;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                id<MTLRenderCommandEncoder> e = [commands renderCommandEncoderWithDescriptor:rp];
                [e setRenderPipelineState:pipeline];
                [e setVertexBuffer:corners offset:0 atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [e endEncoding];
                [commands commit];
                [commands waitUntilCompleted];
                if (commands.status != MTLCommandBufferStatusCompleted) {
                    printf("STORE rotation %lu surface %zux%zu: status %lu\n",
                           (unsigned long)rotation, sw, sh, (unsigned long)commands.status);
                    CFRelease(surface);
                    continue;
                }
                const uint8_t *bytes = planeBytes(surface);
                printf("STORE rotation %lu surface %zux%zu bpr %zu:", (unsigned long)rotation,
                       sw, sh, bpr);
                for (size_t sy = 0; sy < sh; sy += 1) {
                    for (size_t sx = 0; sx < sw; sx += 1) {
                        const uint8_t *at = bytes + sy * bpr + sx * 4;
                        printf(" (%zu,%zu)=%02x,%02x,%02x,%02x", sx, sy, at[0], at[1], at[2], at[3]);
                    }
                }
                printf("\n");

                // A pass that loads the plane (every texel naming its own stored position) and
                // draws only the top-left 4x2 pass texels: what the undrawn texels keep.
                nameStoredTexels(surface, sw, sh, bpr);
                const size_t px = 4, py = 2;
                float part[4] = { -1.0f, 1.0f - 2.0f * (float)py / (float)h,
                                  -1.0f + 2.0f * (float)px / (float)w, 1.0f };
                id<MTLBuffer> partial = [device newBufferWithBytes:part
                                                           length:sizeof(part)
                                                          options:MTLResourceStorageModeShared];
                MTLRenderPassDescriptor *lp = [MTLRenderPassDescriptor renderPassDescriptor];
                lp.colorAttachments[0].texture = texture;
                lp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                lp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLCommandBuffer> second = [queue commandBuffer];
                id<MTLRenderCommandEncoder> le = [second renderCommandEncoderWithDescriptor:lp];
                [le setRenderPipelineState:pipeline];
                [le setVertexBuffer:partial offset:0 atIndex:0];
                [le drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [le endEncoding];
                [second commit];
                [second waitUntilCompleted];
                if (second.status != MTLCommandBufferStatusCompleted) {
                    printf("LOAD rotation %lu surface %zux%zu: status %lu\n",
                           (unsigned long)rotation, sw, sh, (unsigned long)second.status);
                    CFRelease(surface);
                    continue;
                }
                bytes = planeBytes(surface);
                printf("LOAD rotation %lu surface %zux%zu drawn %zux%zu:", (unsigned long)rotation,
                       sw, sh, px, py);
                for (size_t sy = 0; sy < sh; sy += 1) {
                    for (size_t sx = 0; sx < sw; sx += 1) {
                        const uint8_t *at = bytes + sy * bpr + sx * 4;
                        printf(" (%zu,%zu)=%02x,%02x,%02x,%02x", sx, sy, at[0], at[1], at[2], at[3]);
                    }
                }
                printf("\n");
                CFRelease(surface);
            }
        }
        printf("DONE\n");
    }
    return 0;
}
