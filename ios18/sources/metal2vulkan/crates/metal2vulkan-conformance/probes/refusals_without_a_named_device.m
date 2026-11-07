// Plain Metal's own answer, with the API validation layer OFF, for the inputs that 21 refusals of
// the runtime's forwarded problem families assert Metal refuses, each of them a refusal whose doc
// comment reported a measurement but named no device or release. One shape per process, so a process that
// ends is attributable to the shape that ended it, and each process prints the device it ran on
// and the release, so every reading carries its own attribution.
//
// Build (no GPU needed):
//
//   clang -fobjc-arc -fmodules -framework Metal -framework Foundation -framework IOSurface \
//       -o refusals_without_a_named_device refusals_without_a_named_device.m
//
// Run one shape per process, WITHOUT MTL_DEBUG_LAYER (the layer is not passive: it asserts, and
// it has been measured to change what a shader reads):
//
//   ./refusals_without_a_named_device <shape>
//   ./refusals_without_a_named_device --list
//
// Every shape prints `DEVICE <name> / RELEASE <os>` first, then `READ <key> <value>` lines, then
// `DONE <shape>`. A shape whose process Metal ends prints no `DONE`, and its exit status is the
// signal: that is the reading. Each offending shape has a legal neighbour beside it whose
// readings must differ, so a run where everything reads the same has measured nothing.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <stdio.h>
#include <string.h>

// `MTLTextureDescriptor`'s private rotation, which the runtime's `TextureDescriptor::rotation`
// sets, and `MTLRasterizationRateMapDescriptor`'s private mutability.
@interface MTLTextureDescriptor (M2VPrivate)
- (void)setRotation:(NSUInteger)rotation;
- (NSUInteger)rotation;
- (void)setFramebufferOnly:(BOOL)framebufferOnly;
- (BOOL)framebufferOnly;
@end

@protocol M2VRateMapMutability
- (void)setMutability:(NSUInteger)mutability;
- (NSUInteger)mutability;
@end

static id<MTLDevice> gDevice = nil;
static id<MTLCommandQueue> gQueue = nil;

static void head(void) {
    printf("DEVICE %s\n", gDevice.name.UTF8String);
    printf("RELEASE macOS %s\n",
           [NSProcessInfo processInfo].operatingSystemVersionString.UTF8String);
    fflush(stdout);
}

static void readv(const char *key, const char *fmt, ...) {
    printf("READ %s ", key);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void done(const char *shape) {
    printf("DONE %s\n", shape);
    fflush(stdout);
}

static void commit_and_read(id<MTLCommandBuffer> cb, const char *key) {
    [cb commit];
    [cb waitUntilCompleted];
    readv(key, "status %ld error %s", (long)cb.status,
          cb.error ? cb.error.localizedDescription.UTF8String : "(none)");
}

// The first `n` bytes of `data`, as hex, for a texel reading.
static void read_bytes(const char *key, const void *data, size_t n) {
    char line[1024];
    size_t at = 0;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < n && at + 3 < sizeof(line); i++) {
        at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x", p[i]);
    }
    line[at] = 0;
    readv(key, "%s", line);
}

static id<MTLTexture> color_target(NSUInteger samples, NSUInteger side, BOOL readable);
static void clear_color(id<MTLTexture> t, float value);

static MTLTextureDescriptor *tex2d(MTLPixelFormat pf, NSUInteger w, NSUInteger h,
                                   NSUInteger levels, MTLStorageMode mode,
                                   MTLTextureUsage usage) {
    MTLTextureDescriptor *d = [[MTLTextureDescriptor alloc] init];
    d.textureType = MTLTextureType2D;
    d.pixelFormat = pf;
    d.width = w;
    d.height = h;
    d.mipmapLevelCount = levels;
    d.storageMode = mode;
    d.usage = usage;
    return d;
}

// ---------------------------------------------------------------------------------------------
// `HeapProblem::Offset` and `HeapProblem::PastEnd`: a placement offset off the resource's
// alignment, and a resource whose bytes run past the heap's end. The refusals say plain Metal
// answers nil. The control places the same resource at a sound offset inside the heap.

static const NSUInteger kHeapBytes = 1u << 20;

static id<MTLHeap> placement_heap(void) {
    MTLHeapDescriptor *hd = [[MTLHeapDescriptor alloc] init];
    hd.type = MTLHeapTypePlacement;
    hd.size = kHeapBytes;
    hd.storageMode = MTLStorageModePrivate;
    return [gDevice newHeapWithDescriptor:hd];
}

static int group_heap(const char *shape) {
    if (strcmp(shape, "heap-auto-fits") == 0 || strcmp(shape, "heap-auto-longer") == 0) {
        MTLHeapDescriptor *hd = [[MTLHeapDescriptor alloc] init];
        hd.type = MTLHeapTypeAutomatic;
        hd.size = kHeapBytes;
        hd.storageMode = MTLStorageModePrivate;
        id<MTLHeap> heap = [gDevice newHeapWithDescriptor:hd];
        readv("heap", "size %lu", (unsigned long)heap.size);
        NSUInteger length = strcmp(shape, "heap-auto-fits") == 0 ? 4096 : kHeapBytes * 4;
        id<MTLBuffer> b = [heap newBufferWithLength:length
                                            options:MTLResourceStorageModePrivate];
        readv("buffer", "length %lu made %s", (unsigned long)length, b ? "yes" : "nil");
        done(shape);
        return 0;
    }

    id<MTLHeap> heap = placement_heap();
    if (heap == nil) {
        readv("heap", "nil");
        return 2;
    }
    readv("heap", "size %lu", (unsigned long)heap.size);

    // The texture every texture shape places: 64 by 64 R8Unorm, one level.
    MTLTextureDescriptor *td = tex2d(MTLPixelFormatR8Unorm, 64, 64, 1, MTLStorageModePrivate,
                                     MTLTextureUsageShaderRead);
    MTLSizeAndAlign sa = [gDevice heapTextureSizeAndAlignWithDescriptor:td];
    readv("texture-size-and-align", "size %lu align %lu", (unsigned long)sa.size,
          (unsigned long)sa.align);

    NSUInteger offset = 0;
    NSUInteger length = 4096;
    BOOL texture = NO;
    if (strcmp(shape, "heap-buffer-aligned") == 0) {
        offset = 256;
    } else if (strcmp(shape, "heap-buffer-off-align") == 0) {
        offset = 255;
    } else if (strcmp(shape, "heap-buffer-past-end") == 0) {
        offset = kHeapBytes - 1024;
        length = 4096;
    } else if (strcmp(shape, "heap-texture-aligned") == 0) {
        texture = YES;
        offset = sa.align;
    } else if (strcmp(shape, "heap-texture-off-align") == 0) {
        texture = YES;
        offset = sa.align + 1;
    } else if (strcmp(shape, "heap-texture-past-end") == 0) {
        texture = YES;
        offset = kHeapBytes - sa.align;
    } else {
        return 3;
    }

    if (texture) {
        id<MTLTexture> t = [heap newTextureWithDescriptor:td offset:offset];
        readv("texture", "offset %lu made %s", (unsigned long)offset, t ? "yes" : "nil");
    } else {
        id<MTLBuffer> b = [heap newBufferWithLength:length
                                            options:MTLResourceStorageModePrivate
                                             offset:offset];
        readv("buffer", "offset %lu length %lu made %s", (unsigned long)offset,
              (unsigned long)length, b ? "yes" : "nil");
    }
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// `SparseProblem::TextureType`: a 1D, 1D array or buffer texture of a sparse heap, for which the
// refusal says Metal answers nil. The control is the 2D texture of the same heap.

static int group_sparse(const char *shape) {
    MTLHeapDescriptor *hd = [[MTLHeapDescriptor alloc] init];
    hd.type = MTLHeapTypeSparse;
    hd.size = 4u << 20;
    hd.storageMode = MTLStorageModePrivate;
    id<MTLHeap> heap = [gDevice newHeapWithDescriptor:hd];
    if (heap == nil) {
        readv("heap", "nil (no sparse heap)");
        return 2;
    }
    readv("heap", "size %lu type sparse", (unsigned long)heap.size);

    MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];
    td.pixelFormat = MTLPixelFormatR32Float;
    td.width = 256;
    td.height = 1;
    td.depth = 1;
    td.mipmapLevelCount = 1;
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    const char *kind = "?";
    if (strcmp(shape, "sparse-2d") == 0) {
        td.textureType = MTLTextureType2D;
        td.height = 256;
        kind = "2D";
    } else if (strcmp(shape, "sparse-1d") == 0) {
        td.textureType = MTLTextureType1D;
        kind = "1D";
    } else if (strcmp(shape, "sparse-1d-array") == 0) {
        td.textureType = MTLTextureType1DArray;
        td.arrayLength = 2;
        kind = "1DArray";
    } else if (strcmp(shape, "sparse-buffer") == 0) {
        td.textureType = MTLTextureTypeTextureBuffer;
        kind = "TextureBuffer";
    } else {
        return 3;
    }
    MTLSizeAndAlign sa = [gDevice heapTextureSizeAndAlignWithDescriptor:td];
    readv("size-and-align", "type %s size %lu align %lu", kind, (unsigned long)sa.size,
          (unsigned long)sa.align);
    id<MTLTexture> t = [heap newTextureWithDescriptor:td];
    readv("texture", "type %s made %s", kind, t ? "yes" : "nil");
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// `RateMapProblem::EmptyScreen` and `RateMapProblem::NotCompiled`: the screen-to-physical map of a
// map whose screen side is 0, and the physical-to-screen map of a mutable map not yet reset. Both
// refusals say Metal's process ends. Each has its sibling call beside it as the control: the other
// coordinate map of the very same map, which must answer.

static id<MTLRasterizationRateMap> rate_map(NSUInteger w, NSUInteger h, BOOL mutable_map) {
    MTLRasterizationRateLayerDescriptor *layer =
        [[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(4, 4, 0)];
    for (int i = 0; i < 4; i++) {
        layer.horizontalSampleStorage[i] = 1.0f;
        layer.verticalSampleStorage[i] = 1.0f;
    }
    MTLRasterizationRateMapDescriptor *d =
        [MTLRasterizationRateMapDescriptor rasterizationRateMapDescriptorWithScreenSize:
            MTLSizeMake(w, h, 1) layer:layer];
    if (mutable_map) {
        id<M2VRateMapMutability> spi = (id<M2VRateMapMutability>)d;
        readv("mutability-selector", "setMutability: %s",
              [d respondsToSelector:@selector(setMutability:)] ? "yes" : "no");
        if ([d respondsToSelector:@selector(setMutability:)]) {
            [spi setMutability:1];  // MTLMutabilityMutable
            readv("mutability", "%lu", (unsigned long)[spi mutability]);
        }
    }
    return [gDevice newRasterizationRateMapWithDescriptor:d];
}

static int group_rate(const char *shape) {
    BOOL empty = strncmp(shape, "rate-empty-", 11) == 0;
    BOOL mutable_map = strncmp(shape, "rate-mutable-", 13) == 0;
    NSUInteger w = 64, h = empty ? 0 : 64;
    id<MTLRasterizationRateMap> map = rate_map(w, h, mutable_map);
    if (map == nil) {
        readv("map", "nil");
        return 2;
    }
    MTLSize screen = map.screenSize;
    MTLSize phys = [map physicalSizeForLayer:0];
    readv("map", "screen %lux%lu physical %lux%lu layers %lu", (unsigned long)screen.width,
          (unsigned long)screen.height, (unsigned long)phys.width, (unsigned long)phys.height,
          (unsigned long)map.layerCount);

    const char *tail = strrchr(shape, '-');
    if (strcmp(tail, "-s2p") == 0) {
        MTLCoordinate2D out = [map mapScreenToPhysicalCoordinates:MTLCoordinate2DMake(8, 8)
                                                         forLayer:0];
        readv("screen-to-physical", "(8,8) -> (%f,%f)", out.x, out.y);
    } else if (strcmp(tail, "-p2s") == 0) {
        MTLCoordinate2D out = [map mapPhysicalToScreenCoordinates:MTLCoordinate2DMake(8, 8)
                                                         forLayer:0];
        readv("physical-to-screen", "(8,8) -> (%f,%f)", out.x, out.y);
    } else {
        return 3;
    }
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// `TextureProblem::Rotation` and `TextureProblem::RotationWithoutFramebufferOnly`: a rotation past
// 4, and a rotation other than 0 without `framebufferOnly`. Both refusals say Metal's descriptor
// validation asserts, plain too. The descriptor validation runs from
// `heapTextureSizeAndAlignWithDescriptor:` as well as from a creator (the `NotAFormat` row above
// them measured both), so the query is the shape that asks the question without the
// framebuffer-only creator's own rules in the way; the IOSurface creator, which is the one that
// makes a framebuffer-only texture, is measured beside it. The control is rotation 1 with
// `framebufferOnly`, which the descriptor validation must take.

static IOSurfaceRef make_surface(size_t w, size_t h) {
    NSDictionary *props = @{
        (id)kIOSurfaceWidth : @(w),
        (id)kIOSurfaceHeight : @(h),
        (id)kIOSurfaceBytesPerElement : @(4),
        (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
    };
    return IOSurfaceCreate((CFDictionaryRef)props);
}

static int group_rotation(const char *shape) {
    MTLTextureDescriptor *d = tex2d(MTLPixelFormatBGRA8Unorm, 64, 64, 1, MTLStorageModeShared,
                                    MTLTextureUsageRenderTarget);
    readv("rotation-selector", "setRotation: %s",
          [d respondsToSelector:@selector(setRotation:)] ? "yes" : "no");
    if (![d respondsToSelector:@selector(setRotation:)]) {
        return 2;
    }

    NSUInteger rotation = 0;
    BOOL fb = YES;
    if (strcmp(shape, "rot-size-r1-fb") == 0) {
        rotation = 1;
    } else if (strcmp(shape, "rot-size-r4-fb") == 0) {
        rotation = 4;
    } else if (strcmp(shape, "rot-size-r5-fb") == 0) {
        rotation = 5;
    } else if (strcmp(shape, "rot-size-r1-nofb") == 0) {
        rotation = 1;
        fb = NO;
    } else if (strcmp(shape, "rot-size-r0-nofb") == 0) {
        rotation = 0;
        fb = NO;
    } else if (strcmp(shape, "rot-iosurface-r1") == 0 ||
               strcmp(shape, "rot-iosurface-r5") == 0 ||
               strcmp(shape, "rot-iosurface-r1-nofb") == 0) {
        rotation = strcmp(shape, "rot-iosurface-r5") == 0 ? 5 : 1;
        fb = strcmp(shape, "rot-iosurface-r1-nofb") == 0 ? NO : YES;
        d.framebufferOnly = fb;
        [d setRotation:rotation];
        readv("descriptor", "rotation %lu framebufferOnly %d", (unsigned long)[d rotation],
              (int)d.framebufferOnly);
        IOSurfaceRef surface = make_surface(64, 64);
        id<MTLTexture> t = [gDevice newTextureWithDescriptor:d iosurface:surface plane:0];
        readv("iosurface-texture", "rotation %lu framebufferOnly %d made %s",
              (unsigned long)rotation, (int)fb, t ? "yes" : "nil");
        CFRelease(surface);
        done(shape);
        return 0;
    } else {
        return 3;
    }

    d.framebufferOnly = fb;
    [d setRotation:rotation];
    readv("descriptor", "rotation %lu framebufferOnly %d", (unsigned long)[d rotation],
          (int)d.framebufferOnly);
    MTLSizeAndAlign sa = [gDevice heapTextureSizeAndAlignWithDescriptor:d];
    readv("size-and-align", "rotation %lu framebufferOnly %d size %lu align %lu",
          (unsigned long)rotation, (int)fb, (unsigned long)sa.size, (unsigned long)sa.align);
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// `RenderPassProblem::DefaultRasterSampleCount`: a pass without attachments whose
// `defaultRasterSampleCount`'s low 8 bits are 8 to 15 or 128 to 255, or are 0 in a count of 256 or
// more, for which the refusal says plain Metal ends the process making its encoder. The controls
// are the counts beside them: 1, 2 and 4, and 260, whose low 8 bits are 4.

static int group_default_raster(const char *shape, NSUInteger count) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.renderTargetWidth = 64;
    rp.renderTargetHeight = 64;
    rp.defaultRasterSampleCount = count;
    readv("descriptor", "defaultRasterSampleCount %lu low-8 %lu", (unsigned long)count,
          (unsigned long)(count & 0xff));
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    readv("encoder", "made %s", enc ? "yes" : "nil");
    [enc endEncoding];
    commit_and_read(cb, "command-buffer");
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// The shader source the drawing and sample-reading shapes share.

static NSString *const kShaders = @"\
#include <metal_stdlib>\n\
using namespace metal;\n\
struct VOut { float4 position [[position]]; };\n\
vertex VOut vmain(uint vid [[vertex_id]], const device float2 *pts [[buffer(0)]]) {\n\
    VOut o; o.position = float4(pts[vid], 0.0, 1.0); return o;\n\
}\n\
fragment float4 fmain() { return float4(0.75, 0.5, 0.25, 1.0); }\n\
struct MaskedOut { float4 color [[color(0)]]; uint mask [[sample_mask]]; };\n\
fragment MaskedOut fconst(constant float &v [[buffer(0)]]) {\n\
    MaskedOut o; o.color = float4(v, 0.0, 0.0, 1.0); o.mask = 0xCu; return o;\n\
}\n\
kernel void read_color_samples(texture2d_ms<float> src [[texture(0)]],\n\
                               device float *out [[buffer(0)]],\n\
                               uint2 gid [[thread_position_in_grid]]) {\n\
    uint samples = src.get_num_samples();\n\
    for (uint s = 0; s < samples; s++) {\n\
        out[(gid.y * 8 + gid.x) * 8 + s] = src.read(gid, s).r;\n\
    }\n\
}\n\
kernel void read_depth_samples(depth2d_ms<float> src [[texture(0)]],\n\
                               device float *out [[buffer(0)]],\n\
                               uint2 gid [[thread_position_in_grid]]) {\n\
    uint samples = src.get_num_samples();\n\
    for (uint s = 0; s < samples; s++) {\n\
        out[(gid.y * 8 + gid.x) * 8 + s] = src.read(gid, s);\n\
    }\n\
}\n\
using MeshT = mesh<VOut, void, 3, 1, topology::triangle>;\n\
[[mesh]] void mmain(MeshT m, uint tid [[thread_index_in_threadgroup]]) {\n\
    if (tid != 0) { return; }\n\
    VOut v; v.position = float4(0.0, 0.0, 0.0, 1.0);\n\
    m.set_vertex(0, v); m.set_vertex(1, v); m.set_vertex(2, v);\n\
    m.set_index(0, 0); m.set_index(1, 1); m.set_index(2, 2);\n\
    m.set_primitive_count(1);\n\
}\n\
kernel void tile_mark(device uint *out [[buffer(0)]],\n\
                      ushort2 tp [[thread_position_in_threadgroup]]) {\n\
    uint i = uint(tp.y) * 64u + uint(tp.x);\n\
    if (i < 4096u) { out[i] = 0xA5A50000u | i; }\n\
}\n\
kernel void tile_visit(device atomic_uint *out [[buffer(0)]],\n\
                       ushort2 tp [[thread_position_in_threadgroup]],\n\
                       ushort2 size [[threads_per_threadgroup]],\n\
                       ushort2 tgp [[threadgroup_position_in_grid]],\n\
                       ushort li [[thread_index_in_threadgroup]]) {\n\
    uint tile = uint(tgp.y) * 2u + uint(tgp.x);\n\
    if (tile < 4u && tp.x < 64u && tp.y < 64u) {\n\
        uint i = tile * 4096u + uint(tp.y) * 64u + uint(tp.x);\n\
        atomic_fetch_add_explicit(&out[i], 1u, memory_order_relaxed);\n\
    } else {\n\
        atomic_fetch_add_explicit(&out[16384u + 11u], 1u, memory_order_relaxed);\n\
    }\n\
    device atomic_uint *x = out + 16384u;\n\
    atomic_fetch_max_explicit(x + 0u, uint(size.x), memory_order_relaxed);\n\
    atomic_fetch_max_explicit(x + 1u, uint(size.y), memory_order_relaxed);\n\
    atomic_fetch_add_explicit(x + 2u, 1u, memory_order_relaxed);\n\
    if (tp.x >= 16u) { atomic_fetch_add_explicit(x + 3u, 1u, memory_order_relaxed); }\n\
    if (tp.y >= 16u) { atomic_fetch_add_explicit(x + 4u, 1u, memory_order_relaxed); }\n\
    atomic_fetch_max_explicit(x + 5u, uint(tp.x), memory_order_relaxed);\n\
    atomic_fetch_max_explicit(x + 6u, uint(tp.y), memory_order_relaxed);\n\
    atomic_fetch_max_explicit(x + 7u, tile, memory_order_relaxed);\n\
    atomic_fetch_max_explicit(x + 8u, uint(li), memory_order_relaxed);\n\
}\n\
vertex VOut vrec(uint vid [[vertex_id]], device atomic_uint *rec [[buffer(0)]]) {\n\
    atomic_fetch_add_explicit(&rec[vid & 4095u], 1u, memory_order_relaxed);\n\
    atomic_fetch_add_explicit(&rec[4096u], 1u, memory_order_relaxed);\n\
    atomic_fetch_max_explicit(&rec[4097u], vid, memory_order_relaxed);\n\
    atomic_fetch_min_explicit(&rec[4098u], vid, memory_order_relaxed);\n\
    if (vid >= 4096u) { atomic_fetch_add_explicit(&rec[4099u], 1u, memory_order_relaxed); }\n\
    VOut o; o.position = float4(0.0, 0.0, 0.0, 1.0); return o;\n\
}\n\
[[patch(quad, 4)]] vertex VOut tess_post(float2 uv [[position_in_patch]],\n\
                                         const device float2 *pts [[buffer(0)]]) {\n\
    VOut o;\n\
    float2 a = mix(pts[0], pts[1], uv.x);\n\
    float2 b = mix(pts[2], pts[3], uv.x);\n\
    o.position = float4(mix(a, b, uv.y), 0.0, 1.0);\n\
    return o;\n\
}\n\
";

static id<MTLLibrary> library(void) {
    NSError *err = nil;
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
    id<MTLLibrary> lib = [gDevice newLibraryWithSource:kShaders options:opts error:&err];
    if (lib == nil) {
        readv("library", "nil error %s", err.localizedDescription.UTF8String);
    }
    return lib;
}

// A pipeline that draws `fmain` into one color attachment of `pf` at `samples`.
static id<MTLRenderPipelineState> draw_pipeline(id<MTLLibrary> lib, MTLPixelFormat pf,
                                                NSUInteger samples, BOOL icb,
                                                MTLPixelFormat depth) {
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = pf;
    pd.rasterSampleCount = samples;
    pd.depthAttachmentPixelFormat = depth;
    pd.supportIndirectCommandBuffers = icb;
    NSError *err = nil;
    id<MTLRenderPipelineState> ps = [gDevice newRenderPipelineStateWithDescriptor:pd error:&err];
    if (ps == nil) {
        readv("pipeline", "nil error %s", err.localizedDescription.UTF8String);
    }
    return ps;
}

// The four corners of the whole target, as a triangle strip's vertices.
static id<MTLBuffer> quad_points(void) {
    const float pts[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    return [gDevice newBufferWithBytes:pts length:sizeof(pts)
                              options:MTLResourceStorageModeShared];
}

// Every sample of the 8 by 8 texels at the origin of `src`, through `kernel`.
static void read_samples(id<MTLLibrary> lib, const char *kernel_name, id<MTLTexture> src,
                         const char *key) {
    NSError *err = nil;
    id<MTLFunction> fn = [lib newFunctionWithName:
        [NSString stringWithUTF8String:kernel_name]];
    id<MTLComputePipelineState> cps = [gDevice newComputePipelineStateWithFunction:fn error:&err];
    if (cps == nil) {
        readv(key, "no pipeline: %s", err.localizedDescription.UTF8String);
        return;
    }
    id<MTLBuffer> out = [gDevice newBufferWithLength:8 * 8 * 8 * sizeof(float)
                                            options:MTLResourceStorageModeShared];
    memset(out.contents, 0, out.length);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:cps];
    [enc setTexture:src atIndex:0];
    [enc setBuffer:out offset:0 atIndex:0];
    [enc dispatchThreads:MTLSizeMake(8, 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const float *f = (const float *)out.contents;
    char line[512];
    size_t at = 0;
    for (int texel = 0; texel < 2; texel++) {
        for (int s = 0; s < 4; s++) {
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%.5f",
                                   (texel || s) ? " " : "", f[texel * 8 + s]);
        }
    }
    line[at] = 0;
    readv(key, "texels 0,1 samples 0-3: %s", line);
}

// ---------------------------------------------------------------------------------------------
// `CopyProblem::SampleCount` (two textures of different sample counts) and `CopyProblem::Size` (a
// copy of whole textures with no levels of one size, and a copy between compressed levels of
// different sizes). Each offending copy's legal neighbour is the same copy between textures that
// agree, whose destination must read back the source's own texels.

// A 64 by 64 R32Float texture of `samples`, filled by a pass that clears it to `value`.
static id<MTLTexture> filled_color(NSUInteger samples, float value) {
    MTLTextureDescriptor *d = tex2d(MTLPixelFormatR32Float, 64, 64, 1,
                                    samples > 1 ? MTLStorageModePrivate : MTLStorageModeShared,
                                    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = samples;
    }
    id<MTLTexture> t = [gDevice newTextureWithDescriptor:d];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(value, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return t;
}

static void read_texels(id<MTLTexture> t, const char *key) {
    size_t row = t.width * 4;
    void *bytes = malloc(row);
    [t getBytes:bytes bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, t.width, 1) mipmapLevel:0];
    const float *f = (const float *)bytes;
    readv(key, "texels 0-3: %.5f %.5f %.5f %.5f", f[0], f[1], f[2], f[3]);
    free(bytes);
}

static int group_copy(const char *shape) {
    // The sample-count legs over a source whose four samples hold two different values, so a
    // copy that keeps them reads differently from one that cannot: samples 0 and 1 hold 0.25 and
    // samples 2 and 3 hold 0.75. The matched 4-to-4 copy beside them is the control.
    if (strncmp(shape, "copy-distinct-", 14) == 0) {
        id<MTLLibrary> lib = library();
        if (lib == nil) { return 2; }
        NSUInteger dsamples = 4;
        if (strcmp(shape, "copy-distinct-to-2") == 0) { dsamples = 2; }
        else if (strcmp(shape, "copy-distinct-to-1") == 0) { dsamples = 1; }
        else if (strcmp(shape, "copy-distinct-to-4") != 0) { return 3; }
        id<MTLTexture> src = color_target(4, 64, NO);
        id<MTLTexture> dst = color_target(dsamples, 64, YES);
        if (dst == nil) { readv("destination", "nil at %lu samples", (unsigned long)dsamples);
                          return 2; }
        // Clear every sample to 0.25, then overwrite samples 2 and 3 with 0.75 through the
        // encoder's sample mask.
        id<MTLRenderPipelineState> ps = nil;
        {
            MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
            pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
            pd.fragmentFunction = [lib newFunctionWithName:@"fconst"];
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
            pd.rasterSampleCount = 4;
            NSError *err = nil;
            ps = [gDevice newRenderPipelineStateWithDescriptor:pd error:&err];
            if (ps == nil) { readv("pipeline", "nil error %s",
                                   err.localizedDescription.UTF8String); return 2; }
        }
        id<MTLBuffer> pts = quad_points();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = src;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> fill = [gQueue commandBuffer];
        id<MTLRenderCommandEncoder> fenc = [fill renderCommandEncoderWithDescriptor:rp];
        [fenc setRenderPipelineState:ps];
        [fenc setVertexBuffer:pts offset:0 atIndex:0];
        float high = 0.75f;
        // The fragment function's own `[[sample_mask]]` keeps the write to samples 2 and 3:
        // Metal has no encoder-side sample mask.
        [fenc setFragmentBytes:&high length:sizeof(high) atIndex:0];
        [fenc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [fenc endEncoding];
        [fill commit];
        [fill waitUntilCompleted];
        read_samples(lib, "read_color_samples", src, "source");
        if (dsamples > 1) {
            clear_color(dst, -1.0f);
            read_samples(lib, "read_color_samples", dst, "destination-before");
        } else {
            clear_color(dst, -1.0f);
            read_texels(dst, "destination-before");
        }
        readv("copy", "source samples 4 destination samples %lu", (unsigned long)dsamples);
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(64, 64, 1) toTexture:dst destinationSlice:0
             destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        commit_and_read(cb, "command-buffer");
        if (dsamples > 1) {
            read_samples(lib, "read_color_samples", dst, "destination-after");
        } else {
            read_texels(dst, "destination-after");
        }
        done(shape);
        return 0;
    }

    if (strcmp(shape, "copy-samples-4-to-1") == 0 || strcmp(shape, "copy-samples-1-to-1") == 0) {
        BOOL mismatch = strcmp(shape, "copy-samples-4-to-1") == 0;
        id<MTLTexture> src = filled_color(mismatch ? 4 : 1, 0.75f);
        MTLTextureDescriptor *dd = tex2d(MTLPixelFormatR32Float, 64, 64, 1,
                                         MTLStorageModeShared, MTLTextureUsageShaderRead);
        id<MTLTexture> dst = [gDevice newTextureWithDescriptor:dd];
        // A pattern the copy must overwrite, so "copied nothing" is distinguishable.
        float *seed = malloc(64 * 4);
        for (int i = 0; i < 64; i++) { seed[i] = -1.0f; }
        for (NSUInteger y = 0; y < 64; y++) {
            [dst replaceRegion:MTLRegionMake2D(0, y, 64, 1) mipmapLevel:0 withBytes:seed
                   bytesPerRow:64 * 4];
        }
        free(seed);
        readv("source", "samples %lu", (unsigned long)src.sampleCount);
        read_texels(dst, "destination-before");
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(64, 64, 1) toTexture:dst destinationSlice:0
             destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        commit_and_read(cb, "command-buffer");
        read_texels(dst, "destination-after");
        done(shape);
        return 0;
    }

    if (strncmp(shape, "copy-whole-", 11) == 0) {
        // `sz_whole_8l4_to_4l3` (the control, which plain Metal pairs at the source's level 1) and
        // `sz_whole_8_to_3` (the offender, whose extents meet at no level).
        NSUInteger dw = 4, dl = 3;
        if (strcmp(shape, "copy-whole-8-to-3") == 0) { dw = 3; dl = 2; }
        else if (strcmp(shape, "copy-whole-8-to-4") != 0) { return 3; }
        MTLTextureDescriptor *sd = tex2d(MTLPixelFormatR8Unorm, 8, 8, 4, MTLStorageModeShared,
                                         MTLTextureUsageShaderRead);
        MTLTextureDescriptor *td = tex2d(MTLPixelFormatR8Unorm, dw, dw, dl,
                                         MTLStorageModeShared, MTLTextureUsageShaderRead);
        id<MTLTexture> src = [gDevice newTextureWithDescriptor:sd];
        id<MTLTexture> dst = [gDevice newTextureWithDescriptor:td];
        unsigned char ones[64];
        memset(ones, 0x5a, sizeof(ones));
        [src replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:ones
               bytesPerRow:8];
        readv("textures", "source 8x8 levels 4 destination %lux%lu levels %lu",
              (unsigned long)dw, (unsigned long)dw, (unsigned long)dl);
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:src toTexture:dst];
        [blit endEncoding];
        commit_and_read(cb, "command-buffer");
        unsigned char out[16];
        memset(out, 0, sizeof(out));
        [dst getBytes:out bytesPerRow:dw fromRegion:MTLRegionMake2D(0, 0, dw, 1) mipmapLevel:0];
        read_bytes("destination-level-0-row-0", out, dw);
        done(shape);
        return 0;
    }

    if (strncmp(shape, "copy-levels-", 12) == 0) {
        // `copyFromTexture:…sliceCount:levelCount:` between level 0 of two textures of different
        // extents: compressed (the offender, whose common region may end inside a block) and
        // uncompressed beside it (the control, measured to copy the common region).
        BOOL compressed = strcmp(shape, "copy-levels-compressed") == 0 ||
                          strcmp(shape, "copy-levels-compressed-equal") == 0;
        BOOL equal = strcmp(shape, "copy-levels-compressed-equal") == 0 ||
                     strcmp(shape, "copy-levels-plain-equal") == 0;
        MTLPixelFormat pf = compressed ? MTLPixelFormatBC1_RGBA : MTLPixelFormatRGBA8Unorm;
        NSUInteger dw = equal ? 16 : 8;
        MTLTextureDescriptor *sd = tex2d(pf, 16, 16, 1, MTLStorageModeShared,
                                         MTLTextureUsageShaderRead);
        MTLTextureDescriptor *td = tex2d(pf, dw, dw, 1, MTLStorageModeShared,
                                         MTLTextureUsageShaderRead);
        id<MTLTexture> src = [gDevice newTextureWithDescriptor:sd];
        id<MTLTexture> dst = [gDevice newTextureWithDescriptor:td];
        if (src == nil || dst == nil) {
            readv("textures", "source %s destination %s (format %lu)", src ? "made" : "nil",
                  dst ? "made" : "nil", (unsigned long)pf);
            return 2;
        }
        readv("textures", "format %s source 16x16 destination %lux%lu",
              compressed ? "BC1_RGBA" : "RGBA8Unorm", (unsigned long)dw, (unsigned long)dw);
        // A pattern in the source and another in the destination, so what the copy wrote is a
        // reading of its own: a copy that moved nothing leaves the destination's.
        NSUInteger srow = compressed ? (16 / 4) * 8 : 16 * 4;
        NSUInteger drow = compressed ? (dw / 4) * 8 : dw * 4;
        NSUInteger srows = compressed ? 16 / 4 : 16;
        NSUInteger drows = compressed ? dw / 4 : dw;
        unsigned char *sbytes = malloc(srow * srows);
        unsigned char *dbytes = malloc(drow * drows);
        for (NSUInteger i = 0; i < srow * srows; i++) { sbytes[i] = (unsigned char)(i & 0xff); }
        memset(dbytes, 0xee, drow * drows);
        [src replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:sbytes
               bytesPerRow:srow];
        [dst replaceRegion:MTLRegionMake2D(0, 0, dw, dw) mipmapLevel:0 withBytes:dbytes
               bytesPerRow:drow];
        read_bytes("destination-before", dbytes, 16);
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:src sourceSlice:0 sourceLevel:0 toTexture:dst
             destinationSlice:0 destinationLevel:0 sliceCount:1 levelCount:1];
        [blit endEncoding];
        commit_and_read(cb, "command-buffer");
        memset(dbytes, 0, drow * drows);
        [dst getBytes:dbytes bytesPerRow:drow
           fromRegion:MTLRegionMake2D(0, 0, dw, dw) mipmapLevel:0];
        read_bytes("destination-after", dbytes, 16);
        read_bytes("source", sbytes, 16);
        free(sbytes);
        free(dbytes);
        done(shape);
        return 0;
    }
    return 3;
}

// ---------------------------------------------------------------------------------------------
// `RenderPassProblem::ResolveSize` (a color resolve level that reaches past the attachment, which
// loads with `Load` or `DontCare`), `RenderPassProblem::ResolveSamples` (a multisampled resolve
// texture) and `RenderPassProblem::DepthStencilResolveMix` (one depth/stencil texture whose
// stencil resolves alone while its depth is stored).
//
// Every shape clears its multisampled attachment to 0.75 in a first pass and resolves in a
// second, so the resolve's own reading is separable from the clear's. Each resolve texture is
// seeded to -1 first, so "Metal wrote nothing there" reads differently from "Metal wrote".

static id<MTLTexture> color_target(NSUInteger samples, NSUInteger side, BOOL readable) {
    MTLTextureDescriptor *d = tex2d(MTLPixelFormatR32Float, side, side, 1,
                                    samples > 1 ? MTLStorageModePrivate : MTLStorageModeShared,
                                    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = samples;
    }
    (void)readable;
    return [gDevice newTextureWithDescriptor:d];
}

// Clear `t` (any sample count) to `value` through a pass of its own.
static void clear_color(id<MTLTexture> t, float value) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(value, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
}

static int group_resolve(const char *shape) {
    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }

    if (strncmp(shape, "resolve-size-", 13) == 0) {
        NSUInteger rside = strcmp(shape, "resolve-size-past") == 0 ? 128 : 64;
        id<MTLTexture> msaa = color_target(4, 64, NO);
        id<MTLTexture> resolve = color_target(1, rside, YES);
        clear_color(msaa, 0.75f);
        clear_color(resolve, -1.0f);
        readv("textures", "attachment 64x64 samples 4 resolve %lux%lu samples 1",
              (unsigned long)rside, (unsigned long)rside);
        read_texels(resolve, "resolve-before");
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = msaa;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        rp.colorAttachments[0].resolveTexture = resolve;
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        readv("encoder", "made %s", enc ? "yes" : "nil");
        [enc endEncoding];
        commit_and_read(cb, "command-buffer");
        read_texels(resolve, "resolve-after");
        if (rside > 64) {
            float out[4];
            [resolve getBytes:out bytesPerRow:16
                   fromRegion:MTLRegionMake2D(64, 0, 4, 1) mipmapLevel:0];
            readv("resolve-after-past-attachment", "texels 64-67: %.5f %.5f %.5f %.5f", out[0],
                  out[1], out[2], out[3]);
        }
        done(shape);
        return 0;
    }

    if (strncmp(shape, "resolve-samples-color-", 22) == 0) {
        // The offender is a resolve texture of the attachment's own sample count; the control
        // beside it is one of another count, which the refusal says plain Metal leaves untouched.
        NSUInteger rsamples = strcmp(shape, "resolve-samples-color-same") == 0 ? 4 : 2;
        id<MTLTexture> msaa = color_target(4, 64, NO);
        id<MTLTexture> resolve = color_target(rsamples, 64, YES);
        if (resolve == nil) { readv("resolve", "nil at %lu samples", (unsigned long)rsamples);
                              return 2; }
        clear_color(msaa, 0.75f);
        clear_color(resolve, -1.0f);
        readv("textures", "attachment samples 4 resolve samples %lu", (unsigned long)rsamples);
        read_samples(lib, "read_color_samples", resolve, "resolve-before");
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = msaa;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        rp.colorAttachments[0].resolveTexture = resolve;
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        readv("encoder", "made %s", enc ? "yes" : "nil");
        [enc endEncoding];
        commit_and_read(cb, "command-buffer");
        read_samples(lib, "read_color_samples", resolve, "resolve-after");
        done(shape);
        return 0;
    }

    if (strncmp(shape, "resolve-samples-depth-", 22) == 0) {
        NSUInteger rsamples = strcmp(shape, "resolve-samples-depth-msaa") == 0 ? 4 : 1;
        MTLTextureDescriptor *md = tex2d(MTLPixelFormatDepth32Float, 64, 64, 1,
                                         MTLStorageModePrivate,
                                         MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        md.textureType = MTLTextureType2DMultisample;
        md.sampleCount = 4;
        id<MTLTexture> msaa = [gDevice newTextureWithDescriptor:md];
        MTLTextureDescriptor *rd = tex2d(MTLPixelFormatDepth32Float, 64, 64, 1,
                                         MTLStorageModePrivate,
                                         MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        if (rsamples > 1) {
            rd.textureType = MTLTextureType2DMultisample;
            rd.sampleCount = rsamples;
        }
        id<MTLTexture> resolve = [gDevice newTextureWithDescriptor:rd];
        readv("textures", "depth attachment samples 4 resolve samples %lu",
              (unsigned long)rsamples);
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = msaa;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 0.5;
        rp.depthAttachment.storeAction = MTLStoreActionMultisampleResolve;
        rp.depthAttachment.resolveTexture = resolve;
        rp.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        readv("encoder", "made %s", enc ? "yes" : "nil");
        [enc endEncoding];
        commit_and_read(cb, "command-buffer");
        read_samples(lib, "read_depth_samples", resolve, "resolve-after");
        done(shape);
        return 0;
    }

    if (strncmp(shape, "resolve-mix-", 12) == 0) {
        // One Depth32Float_Stencil8 multisampled texture as both attachments. The offender
        // resolves its stencil alone while its depth is stored; the control stores both.
        BOOL mix = strncmp(shape, "resolve-mix-stencil-resolved", 27) == 0;
        double clear_d = strstr(shape, "-d25") ? 0.25 : 0.5;
        MTLTextureDescriptor *md = tex2d(MTLPixelFormatDepth32Float_Stencil8, 64, 64, 1,
                                         MTLStorageModePrivate,
                                         MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        md.textureType = MTLTextureType2DMultisample;
        md.sampleCount = 4;
        id<MTLTexture> ds = [gDevice newTextureWithDescriptor:md];
        MTLTextureDescriptor *sd = tex2d(MTLPixelFormatStencil8, 64, 64, 1,
                                         MTLStorageModePrivate,
                                         MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        id<MTLTexture> sresolve = [gDevice newTextureWithDescriptor:sd];
        readv("textures", "depth/stencil samples 4, stencil resolve %s",
              mix ? "MultisampleResolve" : "none");
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = ds;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = clear_d;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.texture = ds;
        rp.stencilAttachment.loadAction = MTLLoadActionClear;
        rp.stencilAttachment.clearStencil = 7;
        if (mix) {
            rp.stencilAttachment.storeAction = MTLStoreActionMultisampleResolve;
            rp.stencilAttachment.resolveTexture = sresolve;
            rp.stencilAttachment.stencilResolveFilter =
                MTLMultisampleStencilResolveFilterSample0;
        } else {
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        readv("encoder", "made %s", enc ? "yes" : "nil");
        [enc endEncoding];
        commit_and_read(cb, "command-buffer");
        readv("clear-depth", "%f", clear_d);
        read_samples(lib, "read_depth_samples", ds, "depth-samples-after");
        if (mix) {
            unsigned char st[16];
            memset(st, 0, sizeof(st));
            // The stencil resolve texture, so a pass whose offending store action did nothing
            // reads differently from one whose resolve ran.
            id<MTLTexture> readable = sresolve;
            MTLTextureDescriptor *cd = tex2d(MTLPixelFormatStencil8, 64, 64, 1,
                                             MTLStorageModeShared,
                                             MTLTextureUsageShaderRead);
            id<MTLTexture> copy = [gDevice newTextureWithDescriptor:cd];
            id<MTLCommandBuffer> cb2 = [gQueue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb2 blitCommandEncoder];
            [blit copyFromTexture:readable sourceSlice:0 sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1)
                        toTexture:copy destinationSlice:0 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            [cb2 commit];
            [cb2 waitUntilCompleted];
            [copy getBytes:st bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 8, 1)
              mipmapLevel:0];
            read_bytes("stencil-resolve-after", st, 8);
        }
        done(shape);
        return 0;
    }
    return 3;
}

// ---------------------------------------------------------------------------------------------
// `DrawProblem::ThreadsPerTilePastTile`: `dispatchThreadsPerTile:` with threads per tile wider than
// the pass's tile. The tile function marks the index of every thread that ran in a buffer, so the
// reading is which threads Metal ran: the control's 16 against the offender's 32.

static int group_tile(const char *shape) {
    NSUInteger wide = (NSUInteger)atol(shape + strlen("tile-threads-"));
    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }
    id<MTLTexture> target = color_target(1, 16, YES);
    MTLTileRenderPipelineDescriptor *td = [[MTLTileRenderPipelineDescriptor alloc] init];
    td.tileFunction = [lib newFunctionWithName:@"tile_mark"];
    td.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
    td.threadgroupSizeMatchesTileSize = NO;
    NSError *err = nil;
    id<MTLRenderPipelineState> ps =
        [gDevice newRenderPipelineStateWithTileDescriptor:td options:MTLPipelineOptionNone
                                              reflection:nil error:&err];
    if (ps == nil) {
        readv("tile-pipeline", "nil error %s", err.localizedDescription.UTF8String);
        return 2;
    }
    id<MTLBuffer> marks = [gDevice newBufferWithLength:4096 * sizeof(uint32_t)
                                              options:MTLResourceStorageModeShared];
    memset(marks.contents, 0, marks.length);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.tileWidth = 16;
    rp.tileHeight = 16;
    readv("pass", "target 16x16 tile %lux%lu dispatch %lux1", (unsigned long)rp.tileWidth,
          (unsigned long)rp.tileHeight, (unsigned long)wide);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setTileBuffer:marks offset:0 atIndex:0];
    [enc dispatchThreadsPerTile:MTLSizeMake(wide, 1, 1)];
    [enc endEncoding];
    commit_and_read(cb, "command-buffer");
    const uint32_t *m = (const uint32_t *)marks.contents;
    NSUInteger ran = 0, highest = 0;
    for (NSUInteger i = 0; i < 4096; i++) {
        if (m[i] != 0) { ran++; highest = i; }
    }
    readv("threads-that-ran", "count %lu highest-index %lu", (unsigned long)ran,
          (unsigned long)highest);
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// `DrawProblem::ThreadsPerTilePastTile`, the counting legs: the reading `tile-threads-*` takes
// (which thread positions wrote a mark) cannot separate a thread that never ran from one that ran
// with its position clamped into the tile, because both leave the in-tile mark alone. These legs
// count every visit to every position of every tile atomically, so a position two threads reach
// reads 2, and they read the threads per threadgroup the tile function is told it has, which says
// whether Metal cut the dispatch or kept it. The target is 32 by 32 in 16 by 16 tiles, four of
// them, so a dispatch that runs on every tile is separable from one that runs on one.
//
// Slots: 4096 per tile for its positions, then the aggregates at 16384.

static int group_tile_visits(const char *shape) {
    // `tilec-WxH`.
    NSUInteger wide = 0, high = 0;
    if (sscanf(shape + strlen("tilec-"), "%lux%lu", (unsigned long *)&wide,
               (unsigned long *)&high) != 2) {
        return 3;
    }
    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }
    id<MTLTexture> target = color_target(1, 32, YES);
    MTLTileRenderPipelineDescriptor *td = [[MTLTileRenderPipelineDescriptor alloc] init];
    td.tileFunction = [lib newFunctionWithName:@"tile_visit"];
    td.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
    td.threadgroupSizeMatchesTileSize = NO;
    NSError *err = nil;
    id<MTLRenderPipelineState> ps =
        [gDevice newRenderPipelineStateWithTileDescriptor:td options:MTLPipelineOptionNone
                                              reflection:nil error:&err];
    if (ps == nil) {
        readv("tile-pipeline", "nil error %s", err.localizedDescription.UTF8String);
        return 2;
    }
    readv("tile-state", "maxTotalThreadsPerThreadgroup %lu",
          (unsigned long)ps.maxTotalThreadsPerThreadgroup);
    const NSUInteger slots = 16384 + 16;
    id<MTLBuffer> visits = [gDevice newBufferWithLength:slots * sizeof(uint32_t)
                                               options:MTLResourceStorageModeShared];
    memset(visits.contents, 0, visits.length);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.tileWidth = 16;
    rp.tileHeight = 16;
    readv("pass", "target 32x32 tile asked 16x16 kept %lux%lu dispatch %lux%lu",
          (unsigned long)rp.tileWidth, (unsigned long)rp.tileHeight, (unsigned long)wide,
          (unsigned long)high);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setTileBuffer:visits offset:0 atIndex:0];
    readv("encoder-tile", "width %lu height %lu", (unsigned long)enc.tileWidth,
          (unsigned long)enc.tileHeight);
    [enc dispatchThreadsPerTile:MTLSizeMake(wide, high, 1)];
    [enc endEncoding];
    commit_and_read(cb, "command-buffer");
    const uint32_t *v = (const uint32_t *)visits.contents;
    for (NSUInteger t = 0; t < 4; t++) {
        const uint32_t *tv = v + t * 4096;
        NSUInteger visited = 0, twice = 0, highest_count = 0;
        NSUInteger max_x = 0, max_y = 0;
        BOOL any = NO;
        for (NSUInteger y = 0; y < 64; y++) {
            for (NSUInteger x = 0; x < 64; x++) {
                uint32_t c = tv[y * 64 + x];
                if (c == 0) { continue; }
                visited++;
                if (c > 1) { twice++; }
                if (c > highest_count) { highest_count = c; }
                if (!any || x > max_x) { max_x = x; }
                if (!any || y > max_y) { max_y = y; }
                any = YES;
            }
        }
        readv("tile-visits", "tile %lu positions %lu highest-x %lu highest-y %lu "
              "positions-visited-more-than-once %lu highest-visit-count %lu",
              (unsigned long)t, (unsigned long)visited, (unsigned long)max_x,
              (unsigned long)max_y, (unsigned long)twice, (unsigned long)highest_count);
    }
    const uint32_t *x = v + 16384;
    readv("aggregates", "threads-per-threadgroup %ux%u invocations %u past-tile-x %u "
          "past-tile-y %u highest-position %ux%u highest-tile %u highest-thread-index %u "
          "out-of-slot %u",
          x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8], x[11]);
    read_texels(target, "target-after");
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// `DrawProblem::IndexBufferRange` and the index fetch past an index buffer's end: what an indexed
// draw's positions past the buffer fetch. The index buffer is a short buffer placed over a long
// one in a shared placement heap, every word of which holds its own word number, so a fetched
// index names the word it came from: a position that fetched the memory after the short buffer
// reads back as that word's own number, and one Metal bounded reads back as 0. The vertex
// function records every `vertex_id` it is given; it puts every vertex at one point, so nothing
// rasterizes and the recording is the whole reading.
//
// Record slots: 4096 for the index values, then the total at 4096, the highest at 4097, the
// lowest at 4098 (seeded to all ones) and the values past the slots at 4099.

// The indirect command buffer the ICB legs make (the ICB section below).
static id<MTLIndirectCommandBuffer> icb(MTLIndirectCommandType types, BOOL inherit,
                                        NSUInteger count, NSUInteger kernel_binds);

static const NSUInteger kIdxHeapBytes = 1u << 18;
static const NSUInteger kIdxLong = 4096;      // the long buffer's bytes, at heap offset 0
static const NSUInteger kRecOffset = 1u << 16;  // the record buffer's heap offset
static const NSUInteger kRecSlots = 4100;
static const NSUInteger kGuardOffset = 1u << 17;  // 256-aligned, past the record buffer
static const NSUInteger kGuardBytes = 1024;

// The heap every index leg places its buffers in, shared so the probe can stamp and read it.
static id<MTLHeap> index_heap(void) {
    MTLHeapDescriptor *hd = [[MTLHeapDescriptor alloc] init];
    hd.type = MTLHeapTypePlacement;
    hd.size = kIdxHeapBytes;
    hd.storageMode = MTLStorageModeShared;
    return [gDevice newHeapWithDescriptor:hd];
}

// Read `rec` back: the index values recorded, in order, with their counts.
static void read_record(id<MTLBuffer> rec, const char *key) {
    const uint32_t *r = (const uint32_t *)rec.contents;
    char line[1024];
    size_t at = 0;
    NSUInteger distinct = 0;
    for (NSUInteger i = 0; i < 4096; i++) {
        if (r[i] == 0) { continue; }
        distinct++;
        if (at + 24 < sizeof(line)) {
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%lu:%u ", (unsigned long)i,
                                   r[i]);
        }
    }
    line[at] = 0;
    readv(key, "invocations %u distinct %lu highest %u lowest %u past-slots %u values %s",
          r[4096], (unsigned long)distinct, r[4097],
          r[4098] == 0xFFFFFFFFu ? 0xFFFFFFFFu : r[4098], r[4099], line);
}

static int group_index_fetch(const char *shape) {
    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }
    id<MTLHeap> heap = index_heap();
    if (heap == nil) {
        readv("heap", "nil (no shared placement heap)");
        return 2;
    }
    // The long buffer: word `i` holds `i`, so a fetched index names its own word.
    id<MTLBuffer> longer = [heap newBufferWithLength:kIdxLong
                                            options:MTLResourceStorageModeShared offset:0];
    if (longer == nil) {
        readv("long-buffer", "nil");
        return 2;
    }
    uint32_t *words = (uint32_t *)longer.contents;
    for (NSUInteger i = 0; i < kIdxLong / 4; i++) { words[i] = (uint32_t)i; }
    // The short buffer over its first bytes: the index buffer every leg draws from.
    NSUInteger length = 512, offset = 480, count = 12;
    if (strcmp(shape, "idxv-inside") == 0 || strcmp(shape, "idxc-inside") == 0) {
        offset = 464;
    } else if (strcmp(shape, "idxv-past") == 0 || strcmp(shape, "idxc-past") == 0) {
        offset = 480;
    } else if (strcmp(shape, "idxv-at-end") == 0) {
        offset = 512;
    } else if (strcmp(shape, "idxv-past-end-start") == 0) {
        offset = 520;
    } else if (strcmp(shape, "idxv-tail-510") == 0) {
        length = 510;
    } else if (strcmp(shape, "idxv-tail-509") == 0) {
        length = 509;
    } else {
        return 3;
    }
    id<MTLBuffer> indices = [heap newBufferWithLength:length
                                             options:MTLResourceStorageModeShared offset:0];
    if (indices == nil) {
        readv("index-buffer", "nil");
        return 2;
    }
    readv("index-buffer", "length %lu offset %lu indexCount %lu bytes-needed %lu "
          "words-in-range %lu first-word %lu",
          (unsigned long)indices.length, (unsigned long)offset, (unsigned long)count,
          (unsigned long)(offset + count * 4), (unsigned long)(length / 4),
          (unsigned long)(offset / 4));
    // The record buffer, and a guard of 0xEE at the next 256-aligned offset past it.
    id<MTLBuffer> rec = [heap newBufferWithLength:kRecSlots * sizeof(uint32_t)
                                         options:MTLResourceStorageModeShared
                                          offset:kRecOffset];
    id<MTLBuffer> guard = [heap newBufferWithLength:kGuardBytes
                                           options:MTLResourceStorageModeShared
                                            offset:kGuardOffset];
    if (rec == nil || guard == nil) {
        readv("record", "nil");
        return 2;
    }
    memset(rec.contents, 0, rec.length);
    ((uint32_t *)rec.contents)[4098] = 0xFFFFFFFFu;
    memset(guard.contents, 0xEE, guard.length);

    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"vrec"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
    pd.supportIndirectCommandBuffers = YES;
    NSError *err = nil;
    id<MTLRenderPipelineState> ps = [gDevice newRenderPipelineStateWithDescriptor:pd error:&err];
    if (ps == nil) {
        readv("pipeline", "nil error %s", err.localizedDescription.UTF8String);
        return 2;
    }
    id<MTLTexture> target = color_target(1, 64, YES);
    clear_color(target, -1.0f);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    BOOL through_icb = strncmp(shape, "idxc-", 5) == 0;
    id<MTLIndirectCommandBuffer> b = nil;
    if (through_icb) {
        b = icb(MTLIndirectCommandTypeDrawIndexed, NO, 4, 4);
        id<MTLIndirectRenderCommand> cmd = [b indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:ps];
        [cmd setVertexBuffer:rec offset:0 atIndex:0];
        [cmd drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:count
                        indexType:MTLIndexTypeUInt32 indexBuffer:indices
                indexBufferOffset:offset instanceCount:1 baseVertex:0 baseInstance:0];
        readv("draw", "an indirect command's drawIndexedPrimitives: returned");
    }
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (through_icb) {
        [enc useResource:indices usage:MTLResourceUsageRead
                  stages:MTLRenderStageVertex | MTLRenderStageFragment];
        [enc useResource:longer usage:MTLResourceUsageRead
                  stages:MTLRenderStageVertex | MTLRenderStageFragment];
        [enc useResource:rec usage:MTLResourceUsageRead | MTLResourceUsageWrite
                  stages:MTLRenderStageVertex];
        [enc executeCommandsInBuffer:b withRange:NSMakeRange(0, 1)];
    } else {
        [enc setRenderPipelineState:ps];
        [enc setVertexBuffer:rec offset:0 atIndex:0];
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:count
                        indexType:MTLIndexTypeUInt32 indexBuffer:indices
                indexBufferOffset:offset instanceCount:1 baseVertex:0 baseInstance:0];
        readv("draw", "drawIndexedPrimitives: returned");
    }
    [enc endEncoding];
    commit_and_read(cb, "command-buffer");
    read_record(rec, "indices-fetched");
    const unsigned char *g = (const unsigned char *)guard.contents;
    NSUInteger stomped = 0;
    for (NSUInteger i = 0; i < kGuardBytes; i++) {
        if (g[i] != 0xEE) { stomped++; }
    }
    readv("guard", "bytes other than 0xEE %lu", (unsigned long)stomped);
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// `DrawProblem::NoTessellationFactorBuffer`: a patch draw while the encoder has no tessellation
// factor buffer, for which the refusal says Metal without its validation stops the process. The
// control is the same patch draw with the factor buffer set.

static int group_tessellation(const char *shape) {
    BOOL with_factors = strcmp(shape, "tess-with-factors") == 0;
    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"tess_post"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
    pd.maxTessellationFactor = 16;
    pd.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionConstant;
    pd.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
    pd.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
    pd.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
    pd.tessellationOutputWindingOrder = MTLWindingClockwise;
    NSError *err = nil;
    id<MTLRenderPipelineState> ps = [gDevice newRenderPipelineStateWithDescriptor:pd error:&err];
    if (ps == nil) {
        readv("pipeline", "nil error %s", err.localizedDescription.UTF8String);
        return 2;
    }
    id<MTLTexture> target = color_target(1, 64, YES);
    clear_color(target, -1.0f);
    id<MTLBuffer> pts = quad_points();
    // `MTLQuadTessellationFactorsHalf`: four edge factors and two inside ones, as halves.
    uint16_t factors[6] = {0x4000, 0x4000, 0x4000, 0x4000, 0x4000, 0x4000};  // 2.0
    id<MTLBuffer> factor_buffer = [gDevice newBufferWithBytes:factors length:sizeof(factors)
                                                     options:MTLResourceStorageModeShared];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    readv("draw", "patch draw, tessellation factor buffer %s",
          with_factors ? "set" : "NOT set");
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setVertexBuffer:pts offset:0 atIndex:0];
    if (with_factors) {
        [enc setTessellationFactorBuffer:factor_buffer offset:0 instanceStride:0];
    }
    [enc drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil
patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
    [enc endEncoding];
    commit_and_read(cb, "command-buffer");
    read_texels(target, "target-after");
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// The indirect command buffer's five refusals: `NoCommandTypes`, `BindCount`,
// `InheritedPipelineState`, `PipelineWithoutSupport` and `CommandType`, and
// `DrawProblem::IndexBufferRange`, which an indirect indexed draw carries. Every shape's legal
// neighbour is the same call on a buffer whose command types, inheritance or index buffer agree
// with it.

static id<MTLIndirectCommandBuffer> icb(MTLIndirectCommandType types, BOOL inherit,
                                        NSUInteger count, NSUInteger kernel_binds) {
    MTLIndirectCommandBufferDescriptor *d =
        [[MTLIndirectCommandBufferDescriptor alloc] init];
    d.commandTypes = types;
    d.inheritPipelineState = inherit;
    d.inheritBuffers = NO;
    d.maxVertexBufferBindCount = 4;
    d.maxFragmentBufferBindCount = 4;
    d.maxKernelBufferBindCount = kernel_binds;
    d.maxObjectBufferBindCount = 4;
    d.maxMeshBufferBindCount = 4;
    return [gDevice newIndirectCommandBufferWithDescriptor:d maxCommandCount:count
                                                   options:MTLResourceStorageModeShared];
}

static id<MTLRenderPipelineState> mesh_pipeline(id<MTLLibrary> lib, BOOL icb_support) {
    MTLMeshRenderPipelineDescriptor *pd = [[MTLMeshRenderPipelineDescriptor alloc] init];
    pd.meshFunction = [lib newFunctionWithName:@"mmain"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
    pd.supportIndirectCommandBuffers = icb_support;
    NSError *err = nil;
    id<MTLRenderPipelineState> ps =
        [gDevice newRenderPipelineStateWithMeshDescriptor:pd options:MTLPipelineOptionNone
                                              reflection:nil error:&err];
    if (ps == nil) {
        readv("mesh-pipeline", "nil error %s", err.localizedDescription.UTF8String);
    }
    return ps;
}

// Execute `buffer`'s commands 0..count in a pass over a 64 by 64 target, with `pts` and `indices`
// resident, and read the target's first texels back.
static void execute_icb(id<MTLIndirectCommandBuffer> buffer, NSUInteger count,
                        NSArray<id<MTLResource>> *resident) {
    id<MTLTexture> target = color_target(1, 64, YES);
    clear_color(target, -1.0f);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    for (id<MTLResource> r in resident) {
        [enc useResource:r usage:MTLResourceUsageRead
                     stages:MTLRenderStageVertex | MTLRenderStageFragment];
    }
    [enc executeCommandsInBuffer:buffer withRange:NSMakeRange(0, count)];
    [enc endEncoding];
    commit_and_read(cb, "command-buffer");
    read_texels(target, "target-after");
}

static int group_icb(const char *shape) {
    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }
    id<MTLBuffer> pts = quad_points();

    // --- `BindCount`: more kernel binds than the runtime allows, and the counts beside it. The
    // reading is the heap bytes the buffer takes, which `maxCommandCount` must move for the
    // reading to be live at all.
    if (strncmp(shape, "icb-binds-", 10) == 0 || strncmp(shape, "icb-count-", 10) == 0) {
        NSUInteger binds = 4, count = 8;
        if (strcmp(shape, "icb-binds-65536") == 0) { binds = 65536; }
        else if (strcmp(shape, "icb-binds-2p31") == 0) { binds = 1u << 31; }
        else if (strcmp(shape, "icb-binds-max") == 0) { binds = 0xFFFFFFFFu; }
        else if (strcmp(shape, "icb-count-1") == 0) { count = 1; }
        else if (strcmp(shape, "icb-count-64") == 0) { count = 64; }
        else if (strcmp(shape, "icb-binds-4") != 0) { return 3; }
        MTLIndirectCommandBufferDescriptor *d =
            [[MTLIndirectCommandBufferDescriptor alloc] init];
        d.commandTypes = MTLIndirectCommandTypeDraw;
        d.inheritPipelineState = NO;
        d.inheritBuffers = NO;
        d.maxVertexBufferBindCount = 4;
        d.maxFragmentBufferBindCount = 4;
        d.maxKernelBufferBindCount = binds;
        readv("descriptor", "maxKernelBufferBindCount %lu maxCommandCount %lu",
              (unsigned long)binds, (unsigned long)count);
        // The bytes the buffer takes, read off the device's own allocated size: the one reading
        // of an indirect command buffer's storage Metal offers. `maxCommandCount` moves it, which
        // is what makes it a live reading rather than a constant.
        NSUInteger before = gDevice.currentAllocatedSize;
        id<MTLIndirectCommandBuffer> b =
            [gDevice newIndirectCommandBufferWithDescriptor:d maxCommandCount:count
                                                   options:MTLResourceStorageModeShared];
        NSUInteger after = gDevice.currentAllocatedSize;
        readv("allocated", "before %lu after %lu delta %ld made %s", (unsigned long)before,
              (unsigned long)after, (long)after - (long)before, b ? "yes" : "nil");
        if (b) { readv("buffer", "commands %lu", (unsigned long)b.size); }
        done(shape);
        return 0;
    }

    // --- `NoCommandTypes`: every call the refusal names, on a buffer of no command types, and the
    // same call on a buffer of `Draw` beside it.
    if (strncmp(shape, "icb-notypes-", 12) == 0 || strncmp(shape, "icb-draw-", 9) == 0) {
        BOOL none = strncmp(shape, "icb-notypes-", 12) == 0;
        const char *call = strrchr(shape, '-') + 1;
        id<MTLIndirectCommandBuffer> b =
            icb(none ? 0 : MTLIndirectCommandTypeDraw, NO, 4, 4);
        readv("buffer", "commandTypes %s made %s size %lu", none ? "none" : "Draw",
              b ? "yes" : "nil", b ? (unsigned long)b.size : 0ul);
        if (b == nil) { return 2; }
        if (strcmp(call, "make") == 0) {
            done(shape);
            return 0;
        }
        if (strcmp(call, "setter") == 0) {
            id<MTLRenderPipelineState> ps =
                draw_pipeline(lib, MTLPixelFormatR32Float, 1, YES, MTLPixelFormatInvalid);
            if (ps == nil) { return 2; }
            id<MTLIndirectRenderCommand> cmd = [b indirectRenderCommandAtIndex:0];
            readv("command", "got %s", cmd ? "yes" : "nil");
            [cmd setRenderPipelineState:ps];
            readv("setter", "setRenderPipelineState: returned");
            done(shape);
            return 0;
        }
        if (strcmp(call, "reset") == 0) {
            [b resetWithRange:NSMakeRange(0, 4)];
            readv("reset", "resetWithRange: returned");
            done(shape);
            return 0;
        }
        if (strcmp(call, "exec") == 0) {
            execute_icb(b, 4, @[]);
            done(shape);
            return 0;
        }
        if (strcmp(call, "optimize") == 0) {
            id<MTLCommandBuffer> cb = [gQueue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit optimizeIndirectCommandBuffer:b withRange:NSMakeRange(0, 4)];
            [blit endEncoding];
            commit_and_read(cb, "command-buffer");
            done(shape);
            return 0;
        }
        if (strcmp(call, "copy") == 0) {
            id<MTLIndirectCommandBuffer> to = icb(MTLIndirectCommandTypeDraw, NO, 4, 4);
            id<MTLCommandBuffer> cb = [gQueue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyIndirectCommandBuffer:b sourceRange:NSMakeRange(0, 4) destination:to
                          destinationIndex:0];
            [blit endEncoding];
            commit_and_read(cb, "command-buffer");
            done(shape);
            return 0;
        }
        if (strcmp(call, "argenc") == 0) {
            MTLArgumentDescriptor *ad = [MTLArgumentDescriptor argumentDescriptor];
            ad.dataType = MTLDataTypeIndirectCommandBuffer;
            ad.index = 0;
            ad.access = MTLBindingAccessReadOnly;
            id<MTLArgumentEncoder> ae = [gDevice newArgumentEncoderWithArguments:@[ad]];
            id<MTLBuffer> arg = [gDevice newBufferWithLength:ae.encodedLength
                                                    options:MTLResourceStorageModeShared];
            [ae setArgumentBuffer:arg offset:0];
            [ae setIndirectCommandBuffer:b atIndex:0];
            read_bytes("argument-buffer", arg.contents, 16);
            done(shape);
            return 0;
        }
        return 3;
    }

    // --- `InheritedPipelineState` and `PipelineWithoutSupport`: a pipeline state set on a command
    // of a buffer that inherits it, and a pipeline made without `supportIndirectCommandBuffers`.
    if (strncmp(shape, "icb-inherit-", 12) == 0 || strncmp(shape, "icb-support-", 12) == 0) {
        BOOL inherit = strcmp(shape, "icb-inherit-yes") == 0;
        BOOL support = strcmp(shape, "icb-support-no") != 0;
        id<MTLRenderPipelineState> ps =
            draw_pipeline(lib, MTLPixelFormatR32Float, 1, support, MTLPixelFormatInvalid);
        if (ps == nil) { return 2; }
        readv("pipeline", "supportIndirectCommandBuffers %d", (int)support);
        id<MTLIndirectCommandBuffer> b = icb(MTLIndirectCommandTypeDraw, inherit, 4, 4);
        readv("buffer", "inheritPipelineState %d made %s", (int)inherit, b ? "yes" : "nil");
        if (b == nil) { return 2; }
        id<MTLIndirectRenderCommand> cmd = [b indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:ps];
        readv("setter", "setRenderPipelineState: returned");
        [cmd setVertexBuffer:pts offset:0 atIndex:0];
        [cmd drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4
              instanceCount:1 baseInstance:0];
        readv("draw", "drawPrimitives: returned");
        execute_icb(b, 1, @[pts]);
        done(shape);
        return 0;
    }

    // --- `CommandType`: a draw of a kind the buffer's records cannot hold, and the same draw on a
    // buffer whose command types hold it.
    if (strncmp(shape, "icb-kind-", 9) == 0) {
        const char *kind = shape + 9;
        MTLIndirectCommandType types = MTLIndirectCommandTypeDraw;
        BOOL mesh = NO;
        if (strcmp(kind, "indexed-on-indexed") == 0) {
            types = MTLIndirectCommandTypeDrawIndexed;
        } else if (strcmp(kind, "patch-on-patch") == 0) {
            types = MTLIndirectCommandTypeDrawPatches;
        } else if (strcmp(kind, "mesh-on-mesh") == 0) {
            types = MTLIndirectCommandTypeDrawMeshThreadgroups;
            mesh = YES;
        } else if (strcmp(kind, "draw-on-mesh") == 0) {
            types = MTLIndirectCommandTypeDrawMeshThreadgroups;
            mesh = YES;
        } else if (strcmp(kind, "indexed-on-draw") != 0 && strcmp(kind, "patch-on-draw") != 0 &&
                   strcmp(kind, "mesh-on-draw") != 0 && strcmp(kind, "draw-on-draw") != 0) {
            return 3;
        }
        if (strcmp(kind, "mesh-on-draw") == 0) { mesh = YES; }
        id<MTLRenderPipelineState> ps =
            mesh ? mesh_pipeline(lib, YES)
                 : draw_pipeline(lib, MTLPixelFormatR32Float, 1, YES, MTLPixelFormatInvalid);
        if (ps == nil) { return 2; }
        id<MTLIndirectCommandBuffer> b = icb(types, NO, 4, 4);
        readv("buffer", "commandTypes 0x%lx made %s", (unsigned long)types, b ? "yes" : "nil");
        if (b == nil) { return 2; }
        id<MTLIndirectRenderCommand> cmd = [b indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:ps];
        readv("setter", "setRenderPipelineState: returned");
        id<MTLBuffer> indices = [gDevice newBufferWithLength:256
                                                    options:MTLResourceStorageModeShared];
        {
            uint32_t *w = (uint32_t *)indices.contents;
            for (NSUInteger i = 0; i < 64; i++) { w[i] = (uint32_t)(i & 3); }
        }
        if (strncmp(kind, "indexed", 7) == 0) {
            [cmd setVertexBuffer:pts offset:0 atIndex:0];
            [cmd drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:4
                            indexType:MTLIndexTypeUInt32 indexBuffer:indices
                      indexBufferOffset:0 instanceCount:1 baseVertex:0 baseInstance:0];
        } else if (strncmp(kind, "patch", 5) == 0) {
            id<MTLBuffer> factors = [gDevice newBufferWithLength:64
                                                        options:MTLResourceStorageModeShared];
            [cmd drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil
patchIndexBufferOffset:0 instanceCount:1 baseInstance:0
     tessellationFactorBuffer:factors tessellationFactorBufferOffset:0
tessellationFactorBufferInstanceStride:0];
        } else if (strncmp(kind, "mesh", 4) == 0) {
            [cmd drawMeshThreadgroups:MTLSizeMake(1, 1, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
        } else {
            [cmd setVertexBuffer:pts offset:0 atIndex:0];
            [cmd drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4
                  instanceCount:1 baseInstance:0];
        }
        readv("draw", "the draw's setter returned");
        execute_icb(b, 1, @[pts, indices]);
        done(shape);
        return 0;
    }

    // --- `DrawProblem::IndexBufferRange`: an indirect indexed draw whose indices run past its
    // index buffer, and the same draw with a buffer that holds them.
    if (strncmp(shape, "icb-idx-", 8) == 0) {
        BOOL past = strncmp(shape, "icb-idx-past", 12) == 0;
        // `icb-idx-degenerate` holds four indices that draw nothing, and asks for six: anything
        // drawn came from the two indices past the buffer's end.
        BOOL degenerate = strcmp(shape, "icb-idx-degenerate") == 0;
        id<MTLRenderPipelineState> ps =
            draw_pipeline(lib, MTLPixelFormatR32Float, 1, YES, MTLPixelFormatInvalid);
        if (ps == nil) { return 2; }
        // Four 32-bit indices make 16 bytes; the offending draw asks for 6 of them.
        NSUInteger length = past ? 16 : 64;
        uint32_t idx[16] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
        if (degenerate) { idx[0] = idx[1] = idx[2] = idx[3] = 0; }
        id<MTLBuffer> indices = [gDevice newBufferWithBytes:idx length:length
                                                   options:MTLResourceStorageModeShared];
        readv("index-buffer", "length %lu indexCount 6 bytes-needed 24",
              (unsigned long)indices.length);
        id<MTLIndirectCommandBuffer> b = icb(MTLIndirectCommandTypeDrawIndexed, NO, 4, 4);
        id<MTLIndirectRenderCommand> cmd = [b indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:ps];
        [cmd setVertexBuffer:pts offset:0 atIndex:0];
        [cmd drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:6
                        indexType:MTLIndexTypeUInt32 indexBuffer:indices indexBufferOffset:0
                    instanceCount:1 baseVertex:0 baseInstance:0];
        readv("draw", "drawIndexedPrimitives: returned");
        execute_icb(b, 1, @[pts, indices]);
        done(shape);
        return 0;
    }
    return 3;
}

// ---------------------------------------------------------------------------------------------

static const char *const kShapes[] = {
    // HeapProblem::Offset, HeapProblem::PastEnd
    "heap-buffer-aligned", "heap-buffer-off-align", "heap-buffer-past-end",
    "heap-texture-aligned", "heap-texture-off-align", "heap-texture-past-end",
    "heap-auto-fits", "heap-auto-longer",
    // SparseProblem::TextureType
    "sparse-2d", "sparse-1d", "sparse-1d-array", "sparse-buffer",
    // RateMapProblem::EmptyScreen, RateMapProblem::NotCompiled
    "rate-plain-s2p", "rate-plain-p2s", "rate-empty-s2p", "rate-empty-p2s",
    "rate-mutable-s2p", "rate-mutable-p2s",
    // TextureProblem::Rotation, TextureProblem::RotationWithoutFramebufferOnly
    "rot-size-r0-nofb", "rot-size-r1-fb", "rot-size-r4-fb", "rot-size-r5-fb",
    "rot-size-r1-nofb", "rot-iosurface-r1", "rot-iosurface-r5", "rot-iosurface-r1-nofb",
    // RenderPassProblem::DefaultRasterSampleCount
    "draster-1", "draster-4", "draster-8", "draster-15", "draster-130", "draster-256",
    "draster-260",
    // CopyProblem::SampleCount, CopyProblem::Size
    "copy-samples-1-to-1", "copy-samples-4-to-1", "copy-whole-8-to-4", "copy-whole-8-to-3",
    "copy-levels-plain", "copy-levels-plain-equal", "copy-levels-compressed",
    "copy-levels-compressed-equal",
    // RenderPassProblem::ResolveSize, ResolveSamples, DepthStencilResolveMix
    "resolve-size-equal", "resolve-size-past", "resolve-samples-color-other",
    "resolve-samples-color-same", "resolve-samples-depth-single", "resolve-samples-depth-msaa",
    "resolve-mix-both-stored", "resolve-mix-stencil-resolved",
    // DrawProblem::ThreadsPerTilePastTile
    "tile-threads-8", "tile-threads-16", "tile-threads-32",
    // DrawProblem::ThreadsPerTilePastTile, counting every visit to every position
    "tilec-16x16", "tilec-8x8", "tilec-32x1", "tilec-17x1", "tilec-24x1", "tilec-16x32",
    "tilec-32x32", "tilec-64x16", "tilec-16x64", "tilec-33x31", "tilec-32x64", "tilec-64x64",
    // DrawProblem::IndexBufferRange, and the index fetch past the buffer's end
    "idxv-inside", "idxv-past", "idxv-at-end", "idxv-past-end-start", "idxv-tail-510",
    "idxv-tail-509", "idxc-inside", "idxc-past",
    // DrawProblem::NoTessellationFactorBuffer
    "tess-with-factors", "tess-without-factors",
    // IndirectCommandBufferProblem::BindCount
    "icb-binds-4", "icb-binds-65536", "icb-binds-2p31", "icb-binds-max", "icb-count-1",
    "icb-count-64",
    // IndirectCommandBufferProblem::NoCommandTypes
    "icb-draw-make", "icb-notypes-make", "icb-draw-setter", "icb-notypes-setter",
    "icb-draw-reset", "icb-notypes-reset", "icb-draw-exec", "icb-notypes-exec",
    "icb-draw-optimize", "icb-notypes-optimize", "icb-draw-copy", "icb-notypes-copy",
    "icb-draw-argenc", "icb-notypes-argenc",
    // IndirectCommandBufferProblem::InheritedPipelineState, PipelineWithoutSupport
    "icb-inherit-no", "icb-inherit-yes", "icb-support-yes", "icb-support-no",
    // IndirectCommandBufferProblem::CommandType
    "icb-kind-draw-on-draw", "icb-kind-indexed-on-indexed", "icb-kind-indexed-on-draw",
    "icb-kind-patch-on-patch", "icb-kind-patch-on-draw", "icb-kind-mesh-on-mesh",
    "icb-kind-mesh-on-draw", "icb-kind-draw-on-mesh",
    // DrawProblem::IndexBufferRange
    "icb-idx-inside", "icb-idx-past-end", "icb-idx-degenerate",
    // The legs added for a differentiated control
    "copy-distinct-to-4", "copy-distinct-to-2", "copy-distinct-to-1",
    "resolve-mix-both-stored-d25", "resolve-mix-stencil-resolved-d25",
};

int main(int argc, const char *argv[]) {
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < sizeof(kShapes) / sizeof(kShapes[0]); i++) {
            printf("%s\n", kShapes[i]);
        }
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s <shape>|--list\n", argv[0]);
        return 2;
    }
    const char *shape = argv[1];
    @autoreleasepool {
        gDevice = MTLCreateSystemDefaultDevice();
        if (gDevice == nil) {
            fprintf(stderr, "no Metal device\n");
            return 2;
        }
        gQueue = [gDevice newCommandQueue];
        head();
        printf("SHAPE %s\n", shape);
        fflush(stdout);

        if (strncmp(shape, "heap-", 5) == 0) { return group_heap(shape); }
        if (strncmp(shape, "sparse-", 7) == 0) { return group_sparse(shape); }
        if (strncmp(shape, "rate-", 5) == 0) { return group_rate(shape); }
        if (strncmp(shape, "rot-", 4) == 0) { return group_rotation(shape); }
        if (strncmp(shape, "draster-", 8) == 0) {
            return group_default_raster(shape, (NSUInteger)atol(shape + 8));
        }
        if (strncmp(shape, "copy-", 5) == 0) { return group_copy(shape); }
        if (strncmp(shape, "resolve-", 8) == 0) { return group_resolve(shape); }
        if (strncmp(shape, "tilec-", 6) == 0) { return group_tile_visits(shape); }
        if (strncmp(shape, "tile-", 5) == 0) { return group_tile(shape); }
        if (strncmp(shape, "idxv-", 5) == 0 || strncmp(shape, "idxc-", 5) == 0) {
            return group_index_fetch(shape);
        }
        if (strncmp(shape, "tess-", 5) == 0) { return group_tessellation(shape); }
        if (strncmp(shape, "icb-", 4) == 0) { return group_icb(shape); }
        fprintf(stderr, "no such shape: %s\n", shape);
        return 2;
    }
}
