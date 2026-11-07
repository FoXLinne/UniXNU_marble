// Plain Metal's own answer, with the API validation layer OFF, for the two copies
// `CopyProblem::SampleCount` and `CopyProblem::Size` refuse: a texture-to-texture copy between
// different sample counts, and a copy between levels of different sizes (compressed and not,
// through the level-counted selector and through the whole-texture one). One shape per process,
// so a process Metal ends is attributable to the shape that ended it, and every process prints
// the device it ran on and the release, so each reading carries its own attribution.
//
// Build (no GPU needed):
//
//   clang -fobjc-arc -fmodules -framework Metal -framework Foundation \
//       -o copy_sample_counts_and_block_sizes copy_sample_counts_and_block_sizes.m
//
// Run one shape per process, WITHOUT MTL_DEBUG_LAYER (the layer is not passive: it asserts, and
// it has been measured to change what a shader reads):
//
//   ./copy_sample_counts_and_block_sizes <shape>
//   ./copy_sample_counts_and_block_sizes --list
//
// Every shape prints `DEVICE <name> / RELEASE <os>`, then `SHAPE <shape>`, then `READ <key>
// <value>` lines, then `DONE <shape>`. A shape whose process Metal ends prints no `DONE`, and its
// exit status is the signal: that is the reading.
//
// Every destination is placed at offset 0 of its own placement heap, with a buffer of `0xee` at
// the next 256-aligned offset past the destination's heap size: a write past the destination
// lands in the guard and is read back, so an overrun is a reading and not an absence.
//
// The sample shapes stamp every (texel, sample) of the source with a value that names it,
// `(y * 8 + x) * 16 + sample + 1`, each exact in a 32-bit float, so a destination value decodes
// to the one source texel and sample it came from. The size shapes stamp every block of the
// source with its own index in the first two bytes, so a destination block decodes to the one
// source block it came from (and `0xdd`, the destination's own fill, decodes to "untouched").

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static id<MTLDevice> gDevice = nil;
static id<MTLCommandQueue> gQueue = nil;

static const NSUInteger kHeapBytes = 8u << 20;
static const NSUInteger kGuardBytes = 8192;

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
// The destination's placement heap and its `0xee` guard.

static id<MTLHeap> gHeap = nil;
static id<MTLBuffer> gGuard = nil;
static NSUInteger gGuardOffset = 0;

// A destination of `d` at offset 0 of a fresh placement heap, with the guard right after it.
static id<MTLTexture> guarded_destination(MTLTextureDescriptor *d) {
    d.storageMode = MTLStorageModePrivate;
    MTLHeapDescriptor *hd = [[MTLHeapDescriptor alloc] init];
    hd.type = MTLHeapTypePlacement;
    hd.size = kHeapBytes;
    hd.storageMode = MTLStorageModePrivate;
    gHeap = [gDevice newHeapWithDescriptor:hd];
    if (gHeap == nil) {
        readv("heap", "nil");
        return nil;
    }
    MTLSizeAndAlign sa = [gDevice heapTextureSizeAndAlignWithDescriptor:d];
    id<MTLTexture> t = [gHeap newTextureWithDescriptor:d offset:0];
    gGuardOffset = ((sa.size + 255) / 256) * 256;
    gGuard = [gHeap newBufferWithLength:kGuardBytes
                                options:MTLResourceStorageModePrivate
                                 offset:gGuardOffset];
    readv("guard", "destination heap size %lu align %lu guard at %lu bytes %lu %s",
          (unsigned long)sa.size, (unsigned long)sa.align, (unsigned long)gGuardOffset,
          (unsigned long)kGuardBytes, gGuard ? "placed" : "nil");
    if (t == nil || gGuard == nil) {
        readv("destination", "texture %s guard %s", t ? "made" : "nil", gGuard ? "made" : "nil");
        return nil;
    }
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:gGuard range:NSMakeRange(0, kGuardBytes) value:0xee];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return t;
}

// The guard's bytes after the copy: the count that are no longer `0xee`, and the first run.
static void read_guard(void) {
    if (gGuard == nil) {
        return;
    }
    id<MTLBuffer> out = [gDevice newBufferWithLength:kGuardBytes
                                            options:MTLResourceStorageModeShared];
    memset(out.contents, 0, kGuardBytes);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:gGuard sourceOffset:0 toBuffer:out destinationOffset:0 size:kGuardBytes];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const unsigned char *p = (const unsigned char *)out.contents;
    NSUInteger changed = 0;
    long first = -1;
    for (NSUInteger i = 0; i < kGuardBytes; i++) {
        if (p[i] != 0xee) {
            changed++;
            if (first < 0) {
                first = (long)i;
            }
        }
    }
    if (first < 0) {
        readv("guard-after", "intact %lu bytes of 0xee", (unsigned long)kGuardBytes);
        return;
    }
    char line[256];
    size_t at = 0;
    for (NSUInteger i = (NSUInteger)first; i < kGuardBytes && i < (NSUInteger)first + 32; i++) {
        at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x", p[i]);
    }
    line[at] = 0;
    readv("guard-after", "changed %lu bytes first at %ld (heap offset %lu) bytes %s",
          (unsigned long)changed, first, (unsigned long)(gGuardOffset + (NSUInteger)first), line);
}

// ---------------------------------------------------------------------------------------------
// The sample shapes.

static const char *const kSampleShaders =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 position [[position]]; };\n"
"vertex VOut vmain(uint vid [[vertex_id]], const device float2 *pts [[buffer(0)]]) {\n"
"    VOut o; o.position = float4(pts[vid], 0.0, 1.0); return o;\n"
"}\n"
"fragment float4 fstamp(VOut in [[stage_in]], uint sid [[sample_id]]) {\n"
"    uint x = uint(in.position.x);\n"
"    uint y = uint(in.position.y);\n"
"    float v = float((y * 8u + x) * 16u + sid + 1u);\n"
"    return float4(v, 0.0, 0.0, 1.0);\n"
"}\n"
"kernel void read_ms(texture2d_ms<float> src [[texture(0)]],\n"
"                    device float *out [[buffer(0)]],\n"
"                    uint2 gid [[thread_position_in_grid]]) {\n"
"    uint n = src.get_num_samples();\n"
"    for (uint s = 0; s < n; s++) { out[(gid.y * 8u + gid.x) * 8u + s] = src.read(gid, s).r; }\n"
"}\n"
"kernel void read_1s(texture2d<float> src [[texture(0)]],\n"
"                    device float *out [[buffer(0)]],\n"
"                    uint2 gid [[thread_position_in_grid]]) {\n"
"    out[(gid.y * 8u + gid.x) * 8u] = src.read(gid).r;\n"
"}\n";

static id<MTLLibrary> library(void) {
    NSError *err = nil;
    id<MTLLibrary> lib = [gDevice newLibraryWithSource:
                              [NSString stringWithUTF8String:kSampleShaders]
                                              options:nil error:&err];
    if (lib == nil) {
        readv("library", "nil error %s", err.localizedDescription.UTF8String);
    }
    return lib;
}

static id<MTLBuffer> quad_points(void) {
    const float pts[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    return [gDevice newBufferWithBytes:pts length:sizeof(pts)
                              options:MTLResourceStorageModeShared];
}

// An 8 by 8 `R32Float` texture of `samples` whose every (texel, sample) holds the value that
// names it. `heap` places it in the guarded heap instead of its own allocation.
static id<MTLTexture> stamped(id<MTLLibrary> lib, NSUInteger samples, int where, float seed) {
    MTLTextureDescriptor *d = tex2d(MTLPixelFormatR32Float, 8, 8, 1,
                                    where == 2 ? MTLStorageModeShared : MTLStorageModePrivate,
                                    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = samples;
    }
    id<MTLTexture> t = where == 1 ? guarded_destination(d) : [gDevice newTextureWithDescriptor:d];
    if (t == nil) {
        return nil;
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(seed, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (where == 0) {
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fstamp"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
        pd.rasterSampleCount = samples;
        NSError *err = nil;
        id<MTLRenderPipelineState> ps = [gDevice newRenderPipelineStateWithDescriptor:pd
                                                                               error:&err];
        if (ps == nil) {
            readv("pipeline", "nil error %s", err.localizedDescription.UTF8String);
            [enc endEncoding];
            [cb commit];
            return nil;
        }
        [enc setRenderPipelineState:ps];
        [enc setVertexBuffer:quad_points() offset:0 atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return t;
}

// Every sample of every texel of `t`, each decoded to the source texel and sample whose stamp it
// is: `t.s` for the stamp of texel `t` sample `s`, `.` for the destination's own seed of -1, and
// `?<value>` for anything else.
static void read_samples(id<MTLLibrary> lib, id<MTLTexture> t, const char *key) {
    NSUInteger samples = t.sampleCount;
    NSError *err = nil;
    NSString *name = samples > 1 ? @"read_ms" : @"read_1s";
    id<MTLComputePipelineState> cps =
        [gDevice newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&err];
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
    [enc setTexture:t atIndex:0];
    [enc setBuffer:out offset:0 atIndex:0];
    [enc dispatchThreads:MTLSizeMake(8, 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const float *f = (const float *)out.contents;
    for (NSUInteger y = 0; y < 8; y++) {
        char line[1024];
        size_t at = 0;
        for (NSUInteger x = 0; x < 8; x++) {
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%s", x ? " " : "");
            for (NSUInteger s = 0; s < samples; s++) {
                float v = f[(y * 8 + x) * 8 + s];
                if (v == -1.0f) {
                    at += (size_t)snprintf(line + at, sizeof(line) - at, "%s.", s ? "," : "");
                } else if (v >= 1.0f && v == (float)(long)v && ((long)v % 16) != 0 &&
                           ((long)v % 16) <= 8) {
                    long i = (long)v;
                    at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%ld.%ld",
                                           s ? "," : "", i / 16, i % 16 - 1);
                } else {
                    at += (size_t)snprintf(line + at, sizeof(line) - at, "%s?%g",
                                           s ? "," : "", (double)v);
                }
            }
        }
        line[at] = 0;
        readv(key, "row %lu: %s", (unsigned long)y, line);
    }
}

static NSUInteger sample_counts_from(const char *shape, NSUInteger *dst) {
    // `sm-<n>-to-<m>`
    const char *p = shape + 3;
    NSUInteger n = (NSUInteger)atol(p);
    const char *to = strstr(p, "-to-");
    *dst = to ? (NSUInteger)atol(to + 4) : 0;
    return n;
}

static int group_samples(const char *shape) {
    NSUInteger dsamples = 0;
    NSUInteger ssamples = sample_counts_from(shape, &dsamples);
    if (ssamples == 0 || dsamples == 0) {
        return 3;
    }
    readv("counts", "source %lu destination %lu supported source %d destination %d",
          (unsigned long)ssamples, (unsigned long)dsamples,
          [gDevice supportsTextureSampleCount:ssamples] ? 1 : 0,
          [gDevice supportsTextureSampleCount:dsamples] ? 1 : 0);
    id<MTLLibrary> lib = library();
    if (lib == nil) {
        return 2;
    }
    id<MTLTexture> src = stamped(lib, ssamples, 0, 0.0f);
    if (src == nil) {
        readv("source", "nil at %lu samples", (unsigned long)ssamples);
        return 2;
    }
    // `-shared` puts the destination in a shared allocation of its own and `-private` in a
    // private one, both outside the guarded heap: whether the destination's storage mode or its
    // heap placement is what the reading turns on.
    int where = 1;
    if (strstr(shape, "-shared") != NULL) { where = 2; }
    else if (strstr(shape, "-private") != NULL) { where = 0; }
    readv("destination-placement", "%s",
          where == 1 ? "private in a guarded placement heap"
                     : (where == 2 ? "shared, its own allocation" : "private, its own allocation"));
    id<MTLTexture> dst = stamped(lib, dsamples, where, -1.0f);
    if (dst == nil) {
        readv("destination", "nil at %lu samples", (unsigned long)dsamples);
        return 2;
    }
    read_samples(lib, src, "source");
    read_samples(lib, dst, "destination-before");
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(8, 8, 1) toTexture:dst destinationSlice:0
         destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    commit_and_read(cb, "command-buffer");
    read_samples(lib, dst, "destination-after");
    read_guard();
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// The size shapes.

struct SizeCase {
    const char *name;
    MTLPixelFormat pf;
    const char *format;
    NSUInteger bw, bh, bbytes; // the block's texel extent and its bytes
    NSUInteger sw, sh, slevels;
    NSUInteger dw, dh, dlevels;
    NSUInteger slevel, dlevel, levels; // the copy's levels, `levels` 0 for the whole selector
};

#define BC1 MTLPixelFormatBC1_RGBA, "BC1_RGBA", 4, 4, 8
#define BC3 MTLPixelFormatBC3_RGBA, "BC3_RGBA", 4, 4, 16
#define BC4 MTLPixelFormatBC4_RUnorm, "BC4_RUnorm", 4, 4, 8
#define BC7 MTLPixelFormatBC7_RGBAUnorm, "BC7_RGBAUnorm", 4, 4, 16
#define ASTC MTLPixelFormatASTC_6x5_LDR, "ASTC_6x5_LDR", 6, 5, 16
#define EAC MTLPixelFormatEAC_RGBA8, "EAC_RGBA8", 4, 4, 16
#define ETC2 MTLPixelFormatETC2_RGB8, "ETC2_RGB8", 4, 4, 8
#define PLAIN MTLPixelFormatRGBA8Unorm, "RGBA8Unorm", 1, 1, 4

static const struct SizeCase kSizeCases[] = {
    // The level-counted selector between level 0 of two textures of different extents.
    {"lv-bc1-16-to-16", BC1, 16, 16, 1, 16, 16, 1, 0, 0, 1},
    {"lv-bc1-16-to-8", BC1, 16, 16, 1, 8, 8, 1, 0, 0, 1},
    {"lv-bc1-16-to-12", BC1, 16, 16, 1, 12, 12, 1, 0, 0, 1},
    {"lv-bc1-16-to-6", BC1, 16, 16, 1, 6, 6, 1, 0, 0, 1},
    {"lv-bc1-16-to-4", BC1, 16, 16, 1, 4, 4, 1, 0, 0, 1},
    {"lv-bc1-16-to-20", BC1, 16, 16, 1, 20, 20, 1, 0, 0, 1},
    {"lv-bc1-18-to-6", BC1, 18, 18, 1, 6, 6, 1, 0, 0, 1},
    {"lv-bc1-16x8-to-8x16", BC1, 16, 8, 1, 8, 16, 1, 0, 0, 1},
    {"lv-bc3-16-to-8", BC3, 16, 16, 1, 8, 8, 1, 0, 0, 1},
    {"lv-bc4-16-to-8", BC4, 16, 16, 1, 8, 8, 1, 0, 0, 1},
    {"lv-bc7-16-to-8", BC7, 16, 16, 1, 8, 8, 1, 0, 0, 1},
    {"lv-eac-16-to-8", EAC, 16, 16, 1, 8, 8, 1, 0, 0, 1},
    {"lv-etc2-16-to-8", ETC2, 16, 16, 1, 8, 8, 1, 0, 0, 1},
    {"lv-astc65-18x15-to-18x15", ASTC, 18, 15, 1, 18, 15, 1, 0, 0, 1},
    {"lv-astc65-18x15-to-12x10", ASTC, 18, 15, 1, 12, 10, 1, 0, 0, 1},
    {"lv-astc65-18x15-to-9x8", ASTC, 18, 15, 1, 9, 8, 1, 0, 0, 1},
    {"lv-astc65-18x15-to-6x5", ASTC, 18, 15, 1, 6, 5, 1, 0, 0, 1},
    {"lv-plain-16-to-8", PLAIN, 16, 16, 1, 8, 8, 1, 0, 0, 1},
    {"lv-plain-16-to-6", PLAIN, 16, 16, 1, 6, 6, 1, 0, 0, 1},
    // A level other than 0, and more than one level at once.
    {"lv-bc1-32l2-to-16l2-level1", BC1, 32, 32, 2, 16, 16, 2, 1, 1, 1},
    {"lv-bc1-32l2-to-16l2-levels2", BC1, 32, 32, 2, 16, 16, 2, 0, 0, 2},
    {"lv-bc1-32l2-to-32l2-levels2", BC1, 32, 32, 2, 32, 32, 2, 0, 0, 2},
    // The whole-texture selector: `levels` 0.
    {"wh-bc1-16-to-8", BC1, 16, 16, 1, 8, 8, 1, 0, 0, 0},
    {"wh-bc1-16l2-to-8l1", BC1, 16, 16, 2, 8, 8, 1, 0, 0, 0},
    {"wh-bc1-16-to-16", BC1, 16, 16, 1, 16, 16, 1, 0, 0, 0},
    {"wh-plain-16-to-8", PLAIN, 16, 16, 1, 8, 8, 1, 0, 0, 0},
    {"wh-plain-16l2-to-8l1", PLAIN, 16, 16, 2, 8, 8, 1, 0, 0, 0},
    {"wh-astc65-18x15-to-12x10", ASTC, 18, 15, 1, 12, 10, 1, 0, 0, 0},
    {"wh-bc1-16-to-4", BC1, 16, 16, 1, 4, 4, 1, 0, 0, 0},
    {"wh-bc1-16-to-32", BC1, 16, 16, 1, 32, 32, 1, 0, 0, 0},
    {"wh-bc1-16l3-to-4l1", BC1, 16, 16, 3, 4, 4, 1, 0, 0, 0},
    {"wh-bc1-16l5-to-4l1", BC1, 16, 16, 5, 4, 4, 1, 0, 0, 0},
    {"wh-plain-16l5-to-1l1", PLAIN, 16, 16, 5, 1, 1, 1, 0, 0, 0},
    {"wh-plain-8x8-to-8x4", PLAIN, 8, 8, 1, 8, 4, 1, 0, 0, 0},
    {"wh-plain-8l4-to-3l2", PLAIN, 8, 8, 4, 3, 3, 2, 0, 0, 0},
    {"wh-bc1-32l2-to-16l2", BC1, 32, 32, 2, 16, 16, 2, 0, 0, 0},
    {"wh-bc1-32-to-4", BC1, 32, 32, 1, 4, 4, 1, 0, 0, 0},
    {"wh-plain-32-to-8", PLAIN, 32, 32, 1, 8, 8, 1, 0, 0, 0},
    {"wh-plain-16-to-4", PLAIN, 16, 16, 1, 4, 4, 1, 0, 0, 0},
    {"wh-bc1-16l2-to-4l1", BC1, 16, 16, 2, 4, 4, 1, 0, 0, 0},
    {"wh-bc1-4-to-16", BC1, 4, 4, 1, 16, 16, 1, 0, 0, 0},
    {"wh-plain-4-to-16", PLAIN, 4, 4, 1, 16, 16, 1, 0, 0, 0},
    {"wh-plain-4l2-to-16l2", PLAIN, 4, 4, 2, 16, 16, 2, 0, 0, 0},
};

static NSUInteger blocks_wide(const struct SizeCase *c, NSUInteger level, BOOL dst) {
    NSUInteger w = (dst ? c->dw : c->sw) >> level;
    if (w == 0) {
        w = 1;
    }
    return (w + c->bw - 1) / c->bw;
}

static NSUInteger blocks_high(const struct SizeCase *c, NSUInteger level, BOOL dst) {
    NSUInteger h = (dst ? c->dh : c->sh) >> level;
    if (h == 0) {
        h = 1;
    }
    return (h + c->bh - 1) / c->bh;
}

// Every block of level `level` of `t`, decoded to the source block its stamp names: `<n>` for
// source block `n`, `.` for the destination's own `0xdd` fill, and `?<hex>` for anything else.
static void read_blocks(const struct SizeCase *c, id<MTLTexture> t, NSUInteger level, BOOL dst,
                        const char *key) {
    NSUInteger bx = blocks_wide(c, level, dst);
    NSUInteger by = blocks_high(c, level, dst);
    NSUInteger row = bx * c->bbytes;
    NSUInteger bytes = row * by;
    id<MTLBuffer> out = [gDevice newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    memset(out.contents, 0, bytes);
    NSUInteger w = (dst ? c->dw : c->sw) >> level;
    NSUInteger h = (dst ? c->dh : c->sh) >> level;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t sourceSlice:0 sourceLevel:level
             sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w ? w : 1, h ? h : 1, 1)
                 toBuffer:out destinationOffset:0 destinationBytesPerRow:row
destinationBytesPerImage:bytes];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const unsigned char *p = (const unsigned char *)out.contents;
    for (NSUInteger y = 0; y < by; y++) {
        char line[2048];
        size_t at = 0;
        for (NSUInteger x = 0; x < bx; x++) {
            const unsigned char *b = p + y * row + x * c->bbytes;
            BOOL fill = YES;
            for (NSUInteger i = 0; i < c->bbytes; i++) {
                if (b[i] != 0xdd) {
                    fill = NO;
                }
            }
            if (fill) {
                at += (size_t)snprintf(line + at, sizeof(line) - at, "%s.", x ? " " : "");
                continue;
            }
            NSUInteger id_ = (NSUInteger)b[0] | ((NSUInteger)b[1] << 8);
            BOOL stamped_ = YES;
            for (NSUInteger i = 2; i < c->bbytes; i++) {
                if (b[i] != (unsigned char)(0xc0 + i)) {
                    stamped_ = NO;
                }
            }
            if (stamped_) {
                at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%lu", x ? " " : "",
                                       (unsigned long)id_);
            } else {
                at += (size_t)snprintf(line + at, sizeof(line) - at, "%s?", x ? " " : "");
                for (NSUInteger i = 0; i < c->bbytes && at + 3 < sizeof(line); i++) {
                    at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x", b[i]);
                }
            }
        }
        line[at] = 0;
        readv(key, "level %lu row %lu (%lu x %lu blocks): %s", (unsigned long)level,
              (unsigned long)y, (unsigned long)bx, (unsigned long)by, line);
    }
}

// Every block of every level of `t` filled: block `n` of level `l` holds `l * 256 + n` in its
// first two bytes, little endian, and `0xc0 + i` in byte `i` after them, so a destination block
// names the source level and block it came from. A destination holds `0xdd` throughout.
static void fill_texture(const struct SizeCase *c, id<MTLTexture> t, BOOL dst, BOOL seed) {
    NSUInteger levels = dst ? c->dlevels : c->slevels;
    for (NSUInteger level = 0; level < levels; level++) {
        NSUInteger bx = blocks_wide(c, level, dst);
        NSUInteger by = blocks_high(c, level, dst);
        NSUInteger row = bx * c->bbytes;
        unsigned char *bytes = malloc(row * by);
        for (NSUInteger y = 0; y < by; y++) {
            for (NSUInteger x = 0; x < bx; x++) {
                unsigned char *b = bytes + y * row + x * c->bbytes;
                if (seed) {
                    memset(b, 0xdd, c->bbytes);
                    continue;
                }
                // The level in the high byte, so a destination block names the source level it
                // came from as well as the block: level 1's first block is 256, not 0.
                NSUInteger id_ = level * 256 + y * bx + x;
                b[0] = (unsigned char)(id_ & 0xff);
                b[1] = (unsigned char)((id_ >> 8) & 0xff);
                for (NSUInteger i = 2; i < c->bbytes; i++) {
                    b[i] = (unsigned char)(0xc0 + i);
                }
            }
        }
        NSUInteger w = (dst ? c->dw : c->sw) >> level;
        NSUInteger h = (dst ? c->dh : c->sh) >> level;
        [t replaceRegion:MTLRegionMake2D(0, 0, w ? w : 1, h ? h : 1) mipmapLevel:level slice:0
               withBytes:bytes bytesPerRow:row bytesPerImage:row * by];
        free(bytes);
    }
}

static int group_size(const char *shape) {
    const struct SizeCase *c = NULL;
    for (size_t i = 0; i < sizeof(kSizeCases) / sizeof(kSizeCases[0]); i++) {
        if (strcmp(kSizeCases[i].name, shape) == 0) {
            c = &kSizeCases[i];
        }
    }
    if (c == NULL) {
        return 3;
    }
    readv("case", "format %s block %lu x %lu of %lu bytes source %lu x %lu levels %lu "
                  "destination %lu x %lu levels %lu copy %s source level %lu destination level "
                  "%lu levels %lu",
          c->format, (unsigned long)c->bw, (unsigned long)c->bh, (unsigned long)c->bbytes,
          (unsigned long)c->sw, (unsigned long)c->sh, (unsigned long)c->slevels,
          (unsigned long)c->dw, (unsigned long)c->dh, (unsigned long)c->dlevels,
          c->levels ? "level-counted" : "whole-texture", (unsigned long)c->slevel,
          (unsigned long)c->dlevel, (unsigned long)c->levels);
    MTLTextureDescriptor *sd = tex2d(c->pf, c->sw, c->sh, c->slevels, MTLStorageModeShared,
                                     MTLTextureUsageShaderRead);
    id<MTLTexture> src = [gDevice newTextureWithDescriptor:sd];
    if (src == nil) {
        readv("source", "nil");
        return 2;
    }
    // The destination is filled through a shared twin first, then placed in the guarded heap and
    // written from that twin: a private texture takes no `replaceRegion`.
    MTLTextureDescriptor *td = tex2d(c->pf, c->dw, c->dh, c->dlevels, MTLStorageModeShared,
                                     MTLTextureUsageShaderRead);
    id<MTLTexture> seedTex = [gDevice newTextureWithDescriptor:td];
    MTLTextureDescriptor *gd = tex2d(c->pf, c->dw, c->dh, c->dlevels, MTLStorageModePrivate,
                                     MTLTextureUsageShaderRead);
    id<MTLTexture> dst = guarded_destination(gd);
    if (seedTex == nil || dst == nil) {
        readv("destination", "seed %s guarded %s", seedTex ? "made" : "nil", dst ? "made" : "nil");
        return 2;
    }
    fill_texture(c, src, NO, NO);
    fill_texture(c, seedTex, YES, YES);
    {
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:seedTex sourceSlice:0 sourceLevel:0 toTexture:dst
             destinationSlice:0 destinationLevel:0 sliceCount:1 levelCount:c->dlevels];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
    for (NSUInteger level = 0; level < c->slevels; level++) {
        read_blocks(c, src, level, NO, "source");
    }
    for (NSUInteger level = 0; level < c->dlevels; level++) {
        read_blocks(c, dst, level, YES, "destination-before");
    }
    readv("copy", "issuing %s", c->levels ? "level-counted" : "whole-texture");
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (c->levels) {
        [blit copyFromTexture:src sourceSlice:0 sourceLevel:c->slevel toTexture:dst
             destinationSlice:0 destinationLevel:c->dlevel sliceCount:1 levelCount:c->levels];
    } else {
        [blit copyFromTexture:src toTexture:dst];
    }
    [blit endEncoding];
    commit_and_read(cb, "command-buffer");
    for (NSUInteger level = 0; level < c->dlevels; level++) {
        read_blocks(c, dst, level, YES, "destination-after");
    }
    read_guard();
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------

static const char *const kSampleShapes[] = {
    "sm-1-to-1", "sm-1-to-2", "sm-1-to-4", "sm-1-to-8",
    "sm-2-to-1", "sm-2-to-2", "sm-2-to-4", "sm-2-to-8",
    "sm-4-to-1", "sm-4-to-2", "sm-4-to-4", "sm-4-to-8",
    "sm-8-to-1", "sm-8-to-2", "sm-8-to-4", "sm-8-to-8",
    "sm-4-to-1-shared", "sm-4-to-1-private", "sm-2-to-1-shared", "sm-2-to-1-private",
    "sm-4-to-2-private", "sm-2-to-4-private", "sm-4-to-4-private",
};

int main(int argc, const char *argv[]) {
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < sizeof(kSampleShapes) / sizeof(kSampleShapes[0]); i++) {
            printf("%s\n", kSampleShapes[i]);
        }
        for (size_t i = 0; i < sizeof(kSizeCases) / sizeof(kSizeCases[0]); i++) {
            printf("%s\n", kSizeCases[i].name);
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
        if (strncmp(shape, "sm-", 3) == 0) {
            return group_samples(shape);
        }
        return group_size(shape);
    }
}
