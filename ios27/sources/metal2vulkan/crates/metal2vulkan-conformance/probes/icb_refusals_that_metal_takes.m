// Plain Metal's own answer, with the API validation layer OFF, for the four indirect-command-buffer
// refusals the runtime raises where a measurement said plain Metal takes the input:
// `NoCommandTypes`, `BindCount`, `PipelineWithoutSupport` and `CommandType`. Three of the four
// cover an accepted shape and an aborting one at once, so each group sweeps BOTH sides of the line
// rather than one shape of it.
//
// Build (no GPU needed to build):
//
//   clang -fobjc-arc -fmodules -framework Metal -framework Foundation \
//       -o icb_refusals_that_metal_takes icb_refusals_that_metal_takes.m
//
// Run ONE SHAPE PER PROCESS, WITHOUT MTL_DEBUG_LAYER (the layer is not passive: it asserts, and it
// has been measured to change what a shader reads). A shape whose process Metal ends prints no
// `DONE`, and its exit status is the signal: that is the reading.
//
//   ./icb_refusals_that_metal_takes <shape>
//   ./icb_refusals_that_metal_takes --list
//
// Every shape prints `DEVICE <name> / RELEASE <os>` first, then `SHAPE`, then `READ <key> <value>`
// lines, then `DONE <shape>`. Each offending shape has a legal neighbour beside it whose readings
// must differ, so a run where everything reads the same has measured nothing.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

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

// ---------------------------------------------------------------------------------------------
// The shaders: one draw, one mesh pipeline and one marking kernel.

static NSString *const kShaders = @"\
#include <metal_stdlib>\n\
using namespace metal;\n\
struct VOut { float4 position [[position]]; };\n\
vertex VOut vmain(uint vid [[vertex_id]], const device float2 *pts [[buffer(0)]]) {\n\
    VOut o; o.position = float4(pts[vid], 0.0, 1.0); return o;\n\
}\n\
fragment float4 fmain() { return float4(0.75, 0.5, 0.25, 1.0); }\n\
using MeshT = mesh<VOut, void, 3, 1, topology::triangle>;\n\
[[mesh]] void mmain(MeshT m, uint tid [[thread_index_in_threadgroup]]) {\n\
    if (tid != 0) { return; }\n\
    VOut v; v.position = float4(0.0, 0.0, 0.0, 1.0);\n\
    m.set_vertex(0, v); m.set_vertex(1, v); m.set_vertex(2, v);\n\
    m.set_index(0, 0); m.set_index(1, 1); m.set_index(2, 2);\n\
    m.set_primitive_count(1);\n\
}\n\
kernel void mark(device uint *out [[buffer(0)]], uint gid [[thread_position_in_grid]]) {\n\
    out[gid] = 0xC0DE0000u | gid;\n\
}\n\
";

static id<MTLLibrary> library(void) {
    NSError *err = nil;
    id<MTLLibrary> lib = [gDevice newLibraryWithSource:kShaders
                                              options:[[MTLCompileOptions alloc] init]
                                                error:&err];
    if (lib == nil) { readv("library", "nil error %s", err.localizedDescription.UTF8String); }
    return lib;
}

static id<MTLRenderPipelineState> draw_pipeline(id<MTLLibrary> lib, BOOL icb_support) {
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
    pd.supportIndirectCommandBuffers = icb_support;
    NSError *err = nil;
    id<MTLRenderPipelineState> ps = [gDevice newRenderPipelineStateWithDescriptor:pd error:&err];
    if (ps == nil) { readv("pipeline", "nil error %s", err.localizedDescription.UTF8String); }
    return ps;
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
    if (ps == nil) { readv("mesh-pipeline", "nil error %s", err.localizedDescription.UTF8String); }
    return ps;
}

static id<MTLComputePipelineState> mark_pipeline(id<MTLLibrary> lib, BOOL icb_support) {
    MTLComputePipelineDescriptor *cd = [[MTLComputePipelineDescriptor alloc] init];
    cd.computeFunction = [lib newFunctionWithName:@"mark"];
    cd.supportIndirectCommandBuffers = icb_support;
    NSError *err = nil;
    id<MTLComputePipelineState> ps =
        [gDevice newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionNone
                                           reflection:nil error:&err];
    if (ps == nil) {
        readv("compute-pipeline", "nil error %s", err.localizedDescription.UTF8String);
    }
    return ps;
}

// The four corners of the whole target, as a triangle strip's vertices.
static id<MTLBuffer> quad_points(void) {
    const float pts[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    return [gDevice newBufferWithBytes:pts length:sizeof(pts)
                              options:MTLResourceStorageModeShared];
}

static id<MTLTexture> color_target(void) {
    MTLTextureDescriptor *d = [[MTLTextureDescriptor alloc] init];
    d.textureType = MTLTextureType2D;
    d.pixelFormat = MTLPixelFormatR32Float;
    d.width = 64;
    d.height = 64;
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [gDevice newTextureWithDescriptor:d];
}

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

static void read_texels(id<MTLTexture> t, const char *key) {
    size_t row = t.width * 4;
    void *bytes = malloc(row);
    [t getBytes:bytes bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, t.width, 1) mipmapLevel:0];
    const float *f = (const float *)bytes;
    readv(key, "texels 0-3: %.5f %.5f %.5f %.5f", f[0], f[1], f[2], f[3]);
    free(bytes);
}

// ---------------------------------------------------------------------------------------------
// A 0xEE guard next to a destination, so an invisible overrun becomes a reading: the destination
// at offset 0 of a placement heap and the guard at the next 256-aligned offset past it.

static const NSUInteger kGuardBytes = 1024;

typedef struct {
    id<MTLHeap> heap;
    id<MTLBuffer> target;
    id<MTLBuffer> guard;
} Guarded;

static Guarded guarded_buffer(NSUInteger length) {
    Guarded g = {nil, nil, nil};
    NSUInteger at = (length + 255) & ~(NSUInteger)255;
    MTLHeapDescriptor *hd = [[MTLHeapDescriptor alloc] init];
    hd.type = MTLHeapTypePlacement;
    hd.size = at + kGuardBytes + 4096;
    hd.storageMode = MTLStorageModeShared;
    g.heap = [gDevice newHeapWithDescriptor:hd];
    if (g.heap == nil) { return g; }
    g.target = [g.heap newBufferWithLength:length options:MTLResourceStorageModeShared offset:0];
    g.guard = [g.heap newBufferWithLength:kGuardBytes options:MTLResourceStorageModeShared
                                   offset:at];
    if (g.guard) { memset(g.guard.contents, 0xEE, kGuardBytes); }
    readv("guard", "target %lu bytes at 0, guard %lu bytes at %lu", (unsigned long)length,
          (unsigned long)kGuardBytes, (unsigned long)at);
    return g;
}

static void read_guard(Guarded g, const char *key) {
    if (g.guard == nil) { readv(key, "no guard"); return; }
    const unsigned char *p = (const unsigned char *)g.guard.contents;
    NSUInteger bad = 0, first = 0;
    for (NSUInteger i = 0; i < kGuardBytes; i++) {
        if (p[i] != 0xEE) {
            if (bad == 0) { first = i; }
            bad++;
        }
    }
    readv(key, "bytes-not-0xEE %lu first-at %lu", (unsigned long)bad,
          bad ? (unsigned long)first : 0ul);
}

// ---------------------------------------------------------------------------------------------
// `BindCount`: a descriptor's buffer and threadgroup-length counts. The reading is Metal's own
// `allocatedSize` of the buffer, which tells apart a count Metal reserved storage for from one it
// ignored: the kernel count moves a COMPUTE buffer's layout and is ignored by a RENDER buffer's,
// so the same count on a render buffer is the control that says "ignored", not "accepted".

// The count a shape's word names. The word may be followed by more of the shape's name, so each
// spelling is matched as a PREFIX: `keep-2p31-at-40` passes "2p31-at-40" here, and reading that
// with `atoll` alone would silently measure a count of 2.
static NSUInteger count_of(const char *word) {
    if (strncmp(word, "2p32m1", 6) == 0) { return 0xFFFFFFFFull; }
    if (strncmp(word, "2p31", 4) == 0) { return 1ull << 31; }
    if (strncmp(word, "2p26", 4) == 0) { return 1ull << 26; }
    if (strncmp(word, "2p25", 4) == 0) { return 1ull << 25; }
    if (strncmp(word, "2p24", 4) == 0) { return 1ull << 24; }
    if (strncmp(word, "2p20", 4) == 0) { return 1ull << 20; }
    return (NSUInteger)atoll(word);
}

static int group_bind(const char *shape) {
    // bind-<field>-on-<kind>-<count>
    const char *rest = shape + 5;
    const char *field_end = strchr(rest, '-');
    if (field_end == NULL) { return 3; }
    char field[16] = {0};
    strncpy(field, rest, (size_t)(field_end - rest) < 15 ? (size_t)(field_end - rest) : 15);
    if (strncmp(field_end, "-on-", 4) != 0) { return 3; }
    const char *kind = field_end + 4;
    const char *kind_end = strchr(kind, '-');
    if (kind_end == NULL) { return 3; }
    BOOL compute = strncmp(kind, "compute", 7) == 0;
    NSUInteger count = count_of(kind_end + 1);

    MTLIndirectCommandBufferDescriptor *d = [[MTLIndirectCommandBufferDescriptor alloc] init];
    d.commandTypes = compute ? MTLIndirectCommandTypeConcurrentDispatch
                             : MTLIndirectCommandTypeDraw;
    d.inheritPipelineState = NO;
    d.inheritBuffers = NO;
    if (strcmp(field, "kernel") == 0) { d.maxKernelBufferBindCount = count; }
    else if (strcmp(field, "vertex") == 0) { d.maxVertexBufferBindCount = count; }
    else if (strcmp(field, "fragment") == 0) { d.maxFragmentBufferBindCount = count; }
    else if (strcmp(field, "object") == 0) { d.maxObjectBufferBindCount = count; }
    else if (strcmp(field, "mesh") == 0) { d.maxMeshBufferBindCount = count; }
    else if (strcmp(field, "tglen") == 0) { d.maxKernelThreadgroupMemoryBindCount = count; }
    else { return 3; }
    readv("descriptor", "commandTypes %s max%sBufferBindCount %llu maxCommandCount 8",
          compute ? "ConcurrentDispatch" : "Draw", field, (unsigned long long)count);

    NSUInteger before = gDevice.currentAllocatedSize;
    id<MTLIndirectCommandBuffer> b =
        [gDevice newIndirectCommandBufferWithDescriptor:d maxCommandCount:8
                                               options:MTLResourceStorageModeShared];
    NSUInteger after = gDevice.currentAllocatedSize;
    readv("made", "%s", b ? "yes" : "nil");
    readv("device-allocated", "before %lu after %lu delta %ld", (unsigned long)before,
          (unsigned long)after, (long)after - (long)before);
    if (b) {
        readv("buffer", "size %lu allocatedSize %lu", (unsigned long)b.size,
              (unsigned long)b.allocatedSize);
    }
    done(shape);
    return 0;
}

// --- Does a bind at an absurd index actually land? A compute buffer of `count` kernel buffers,
// the marking kernel bound at index 0 and the same buffer bound again at high indices, then
// command 0 dispatched: the marker proves the command ran and index 0's bind was kept, and the
// guard proves no high-index setter wrote outside its own storage.

static int group_keep(const char *shape) {
    // keep-<count>-at-<index>
    NSUInteger count = count_of(shape + 5);
    const char *at = strstr(shape, "-at-");
    if (at == NULL) { return 3; }
    NSUInteger index = count_of(at + 4);
    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }
    id<MTLComputePipelineState> ps = mark_pipeline(lib, YES);
    if (ps == nil) { return 2; }

    MTLIndirectCommandBufferDescriptor *d = [[MTLIndirectCommandBufferDescriptor alloc] init];
    d.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
    d.maxKernelBufferBindCount = count;
    readv("descriptor", "maxKernelBufferBindCount %llu, bind also at index %llu",
          (unsigned long long)count, (unsigned long long)index);
    id<MTLIndirectCommandBuffer> b =
        [gDevice newIndirectCommandBufferWithDescriptor:d maxCommandCount:2
                                               options:MTLResourceStorageModeShared];
    readv("made", "%s allocatedSize %lu", b ? "yes" : "nil",
          b ? (unsigned long)b.allocatedSize : 0ul);
    if (b == nil) { return 2; }

    Guarded g = guarded_buffer(256);
    if (g.target == nil) { readv("heap", "nil"); return 2; }
    memset(g.target.contents, 0, 256);

    id<MTLIndirectComputeCommand> cmd = [b indirectComputeCommandAtIndex:0];
    [cmd setComputePipelineState:ps];
    [cmd setKernelBuffer:g.target offset:0 atIndex:0];
    readv("bind-0", "setKernelBuffer:atIndex:0 returned");
    [cmd setKernelBuffer:g.target offset:0 atIndex:index];
    readv("bind-high", "setKernelBuffer:atIndex:%llu returned", (unsigned long long)index);
    [cmd concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1)
                 threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
    readv("dispatch", "concurrentDispatchThreadgroups: returned");

    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc useResource:g.target usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [enc executeCommandsInBuffer:b withRange:NSMakeRange(0, 1)];
    [enc endEncoding];
    commit_and_read(cb, "command-buffer");
    const uint32_t *w = (const uint32_t *)g.target.contents;
    readv("marker", "words 0-3: %08x %08x %08x %08x", w[0], w[1], w[2], w[3]);
    read_guard(g, "guard-after");
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// `NoCommandTypes`: each call the refusal covers, on a buffer of no command types, with the same
// call on a buffer of the matching command type beside it.

static id<MTLIndirectCommandBuffer> icb(MTLIndirectCommandType types, BOOL inherit,
                                        NSUInteger count) {
    MTLIndirectCommandBufferDescriptor *d = [[MTLIndirectCommandBufferDescriptor alloc] init];
    d.commandTypes = types;
    d.inheritPipelineState = inherit;
    d.inheritBuffers = NO;
    d.maxVertexBufferBindCount = 4;
    d.maxFragmentBufferBindCount = 4;
    d.maxKernelBufferBindCount = 4;
    d.maxObjectBufferBindCount = 4;
    d.maxMeshBufferBindCount = 4;
    return [gDevice newIndirectCommandBufferWithDescriptor:d maxCommandCount:count
                                                  options:MTLResourceStorageModeShared];
}

// Execute `buffer`'s commands 0..count in a render pass over a 64 by 64 target, with `resident`
// resident, and read the target's first texels back.
static void execute_render(id<MTLIndirectCommandBuffer> buffer, NSUInteger count,
                           NSArray<id<MTLResource>> *resident) {
    id<MTLTexture> target = color_target();
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

static int group_types(const char *shape) {
    // types-<none|draw|disp>-<call>
    const char *rest = shape + 6;
    const char *dash = strchr(rest, '-');
    if (dash == NULL) { return 3; }
    char kind[8] = {0};
    strncpy(kind, rest, (size_t)(dash - rest) < 7 ? (size_t)(dash - rest) : 7);
    const char *call = dash + 1;
    MTLIndirectCommandType types = 0;
    if (strcmp(kind, "draw") == 0) { types = MTLIndirectCommandTypeDraw; }
    else if (strcmp(kind, "disp") == 0) { types = MTLIndirectCommandTypeConcurrentDispatch; }
    else if (strcmp(kind, "none") != 0) { return 3; }

    id<MTLIndirectCommandBuffer> b = icb(types, NO, 4);
    readv("buffer", "commandTypes 0x%lx made %s size %lu allocatedSize %lu",
          (unsigned long)types, b ? "yes" : "nil", b ? (unsigned long)b.size : 0ul,
          b ? (unsigned long)b.allocatedSize : 0ul);
    if (b == nil) { return 2; }

    if (strcmp(call, "make") == 0) { done(shape); return 0; }

    if (strcmp(call, "rangereset") == 0 || strcmp(call, "rangereset0") == 0) {
        NSUInteger length = strcmp(call, "rangereset0") == 0 ? 0 : 4;
        readv("reset", "resetWithRange: location 0 length %lu, about to call",
              (unsigned long)length);
        [b resetWithRange:NSMakeRange(0, length)];
        readv("reset", "resetWithRange: returned");
        done(shape);
        return 0;
    }

    if (strcmp(call, "getter-render") == 0) {
        id<MTLIndirectRenderCommand> cmd = [b indirectRenderCommandAtIndex:0];
        readv("getter", "indirectRenderCommandAtIndex:0 -> %s", cmd ? "an object" : "nil");
        done(shape);
        return 0;
    }
    if (strcmp(call, "getter-compute") == 0) {
        id<MTLIndirectComputeCommand> cmd = [b indirectComputeCommandAtIndex:0];
        readv("getter", "indirectComputeCommandAtIndex:0 -> %s", cmd ? "an object" : "nil");
        done(shape);
        return 0;
    }

    if (strcmp(call, "exec") == 0) {
        execute_render(b, 4, @[]);
        done(shape);
        return 0;
    }
    // Every call this buffer takes, then an execution and a commit: a setter that returns can
    // still fail at commit, so the taken calls are read at commit too and not at the call alone.
    if (strcmp(call, "taken-exec") == 0) {
        id<MTLBuffer> scratch = [gDevice newBufferWithLength:256
                                                    options:MTLResourceStorageModeShared];
        id<MTLIndirectComputeCommand> c = [b indirectComputeCommandAtIndex:0];
        [c setKernelBuffer:scratch offset:0 atIndex:0];
        [c setKernelBuffer:scratch offset:0 attributeStride:16 atIndex:1];
        [c setThreadgroupMemoryLength:256 atIndex:0];
        [c setStageInRegion:MTLRegionMake2D(0, 0, 8, 8)];
        [c setImageblockWidth:8 height:8];
        [c setBarrier];
        [c clearBarrier];
        id<MTLIndirectRenderCommand> r = [b indirectRenderCommandAtIndex:0];
        [r setFragmentBuffer:scratch offset:0 atIndex:0];
        [r setBarrier];
        [r clearBarrier];
        [r reset];
        [b resetWithRange:NSMakeRange(0, 0)];
        readv("setters", "every call this buffer takes returned");
        execute_render(b, 4, @[scratch]);
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
    if (strncmp(call, "copy-", 5) == 0) {
        // copy-to-draw, copy-from-draw, copy-to-none
        BOOL to_draw = strcmp(call, "copy-to-draw") == 0;
        BOOL from_draw = strcmp(call, "copy-from-draw") == 0;
        MTLIndirectCommandType other = (to_draw || from_draw) ? MTLIndirectCommandTypeDraw : 0;
        id<MTLIndirectCommandBuffer> o = icb(other, NO, 4);
        if (o == nil) { readv("other", "nil"); return 2; }
        id<MTLIndirectCommandBuffer> src = from_draw ? o : b;
        id<MTLIndirectCommandBuffer> dst = from_draw ? b : o;
        readv("copy", "source commandTypes 0x%lx destination commandTypes 0x%lx",
              (unsigned long)(from_draw ? other : types),
              (unsigned long)(from_draw ? types : other));
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyIndirectCommandBuffer:src sourceRange:NSMakeRange(0, 4) destination:dst
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
        memset(arg.contents, 0, ae.encodedLength);
        [ae setArgumentBuffer:arg offset:0];
        [ae setIndirectCommandBuffer:b atIndex:0];
        const unsigned char *p = (const unsigned char *)arg.contents;
        char line[64];
        size_t at = 0;
        for (size_t i = 0; i < 8; i++) {
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x", p[i]);
        }
        readv("argument-buffer", "%s (the ID Metal wrote)", line);
        done(shape);
        return 0;
    }

    // The per-command setters. Each is reached through the getter first, which is its own reading.
    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }
    id<MTLBuffer> scratch = [gDevice newBufferWithLength:256
                                                options:MTLResourceStorageModeShared];
    if (strncmp(call, "r-", 2) == 0) {
        id<MTLIndirectRenderCommand> cmd = [b indirectRenderCommandAtIndex:0];
        readv("getter", "indirectRenderCommandAtIndex:0 -> %s", cmd ? "an object" : "nil");
        const char *which = call + 2;
        if (strcmp(which, "pipeline") == 0) {
            id<MTLRenderPipelineState> ps = draw_pipeline(lib, YES);
            if (ps == nil) { return 2; }
            [cmd setRenderPipelineState:ps];
            readv("setter", "setRenderPipelineState: returned");
        } else if (strcmp(which, "vbuf") == 0) {
            [cmd setVertexBuffer:scratch offset:0 atIndex:0];
            readv("setter", "setVertexBuffer:offset:atIndex: returned");
        } else if (strcmp(which, "draw") == 0) {
            [cmd drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4
                  instanceCount:1 baseInstance:0];
            readv("setter", "drawPrimitives: returned");
        } else if (strcmp(which, "fbuf") == 0) {
            [cmd setFragmentBuffer:scratch offset:0 atIndex:0];
            readv("setter", "setFragmentBuffer:offset:atIndex: returned");
        } else if (strcmp(which, "obuf") == 0) {
            [cmd setObjectBuffer:scratch offset:0 atIndex:0];
            readv("setter", "setObjectBuffer:offset:atIndex: returned");
        } else if (strcmp(which, "mbuf") == 0) {
            [cmd setMeshBuffer:scratch offset:0 atIndex:0];
            readv("setter", "setMeshBuffer:offset:atIndex: returned");
        } else if (strcmp(which, "idraw") == 0) {
            [cmd drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:4
                            indexType:MTLIndexTypeUInt32 indexBuffer:scratch indexBufferOffset:0
                        instanceCount:1 baseVertex:0 baseInstance:0];
            readv("setter", "drawIndexedPrimitives: returned");
        } else if (strcmp(which, "mdraw") == 0) {
            [cmd drawMeshThreadgroups:MTLSizeMake(1, 1, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
            readv("setter", "drawMeshThreadgroups: returned");
        } else if (strcmp(which, "barrier") == 0) {
            [cmd setBarrier];
            readv("setter", "setBarrier returned");
        } else if (strcmp(which, "clearbarrier") == 0) {
            [cmd clearBarrier];
            readv("setter", "clearBarrier returned");
        } else if (strcmp(which, "objtglen") == 0) {
            [cmd setObjectThreadgroupMemoryLength:256 atIndex:0];
            readv("setter", "setObjectThreadgroupMemoryLength:atIndex: returned");
        } else if (strcmp(which, "reset") == 0) {
            [cmd reset];
            readv("setter", "the command's reset returned");
        } else {
            return 3;
        }
        done(shape);
        return 0;
    }
    if (strncmp(call, "c-", 2) == 0) {
        id<MTLIndirectComputeCommand> cmd = [b indirectComputeCommandAtIndex:0];
        readv("getter", "indirectComputeCommandAtIndex:0 -> %s", cmd ? "an object" : "nil");
        const char *which = call + 2;
        if (strcmp(which, "pipeline") == 0) {
            id<MTLComputePipelineState> ps = mark_pipeline(lib, YES);
            if (ps == nil) { return 2; }
            [cmd setComputePipelineState:ps];
            readv("setter", "setComputePipelineState: returned");
        } else if (strcmp(which, "kbuf") == 0) {
            [cmd setKernelBuffer:scratch offset:0 atIndex:0];
            readv("setter", "setKernelBuffer:offset:atIndex: returned");
        } else if (strcmp(which, "dispatch") == 0) {
            [cmd concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1)
                          threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            readv("setter", "concurrentDispatchThreadgroups: returned");
        } else if (strcmp(which, "tglen") == 0) {
            [cmd setThreadgroupMemoryLength:256 atIndex:0];
            readv("setter", "setThreadgroupMemoryLength:atIndex: returned");
        } else if (strcmp(which, "barrier") == 0) {
            [cmd setBarrier];
            readv("setter", "setBarrier returned");
        } else if (strcmp(which, "clearbarrier") == 0) {
            [cmd clearBarrier];
            readv("setter", "clearBarrier returned");
        } else if (strcmp(which, "dthreads") == 0) {
            [cmd concurrentDispatchThreads:MTLSizeMake(8, 1, 1)
                     threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
            readv("setter", "concurrentDispatchThreads: returned");
        } else if (strcmp(which, "stagein") == 0) {
            [cmd setStageInRegion:MTLRegionMake2D(0, 0, 8, 8)];
            readv("setter", "setStageInRegion: returned");
        } else if (strcmp(which, "imageblock") == 0) {
            [cmd setImageblockWidth:8 height:8];
            readv("setter", "setImageblockWidth:height: returned");
        } else if (strcmp(which, "kbufstride") == 0) {
            [cmd setKernelBuffer:scratch offset:0 attributeStride:16 atIndex:0];
            readv("setter", "setKernelBuffer:offset:attributeStride:atIndex: returned");
        } else if (strcmp(which, "reset") == 0) {
            [cmd reset];
            readv("setter", "the command's reset returned");
        } else {
            return 3;
        }
        done(shape);
        return 0;
    }
    return 3;
}

// ---------------------------------------------------------------------------------------------
// `PipelineWithoutSupport`: a pipeline state made without `supportIndirectCommandBuffers`, set on
// a command. The legs tell the setter apart from the execution, and ask whether a command buffer
// submitted after the fault still completes.

static int group_support(const char *shape) {
    // support-<yes|no>-<render|compute>-<setter|exec|again>
    BOOL support = strncmp(shape + 8, "yes", 3) == 0;
    const char *rest = strchr(shape + 8, '-');
    if (rest == NULL) { return 3; }
    rest++;
    BOOL compute = strncmp(rest, "compute", 7) == 0;
    const char *leg = strchr(rest, '-');
    if (leg == NULL) { return 3; }
    leg++;
    readv("pipeline", "supportIndirectCommandBuffers %d kind %s leg %s", (int)support,
          compute ? "compute" : "render", leg);

    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }

    if (compute) {
        id<MTLComputePipelineState> ps = mark_pipeline(lib, support);
        if (ps == nil) { return 2; }
        id<MTLIndirectCommandBuffer> b = icb(MTLIndirectCommandTypeConcurrentDispatch, NO, 4);
        if (b == nil) { readv("buffer", "nil"); return 2; }
        Guarded g = guarded_buffer(256);
        if (g.target == nil) { readv("heap", "nil"); return 2; }
        memset(g.target.contents, 0, 256);
        id<MTLIndirectComputeCommand> cmd = [b indirectComputeCommandAtIndex:0];
        [cmd setComputePipelineState:ps];
        readv("setter", "setComputePipelineState: returned");
        [cmd setKernelBuffer:g.target offset:0 atIndex:0];
        [cmd concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1)
                      threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
        readv("dispatch", "concurrentDispatchThreadgroups: returned");
        if (strcmp(leg, "setter") == 0) { done(shape); return 0; }
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc useResource:g.target usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [enc executeCommandsInBuffer:b withRange:NSMakeRange(0, 1)];
        [enc endEncoding];
        commit_and_read(cb, "command-buffer");
        const uint32_t *w = (const uint32_t *)g.target.contents;
        readv("marker", "words 0-3: %08x %08x %08x %08x", w[0], w[1], w[2], w[3]);
        read_guard(g, "guard-after");
        if (strcmp(leg, "again") == 0) {
            // A second, plainly valid command buffer after the fault: does the queue still work?
            id<MTLBuffer> out = [gDevice newBufferWithLength:256
                                                    options:MTLResourceStorageModeShared];
            memset(out.contents, 0, 256);
            id<MTLComputePipelineState> plain = mark_pipeline(lib, NO);
            id<MTLCommandBuffer> cb2 = [gQueue commandBuffer];
            id<MTLComputeCommandEncoder> e2 = [cb2 computeCommandEncoder];
            [e2 setComputePipelineState:plain];
            [e2 setBuffer:out offset:0 atIndex:0];
            [e2 dispatchThreadgroups:MTLSizeMake(1, 1, 1)
                 threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
            [e2 endEncoding];
            commit_and_read(cb2, "second-command-buffer");
            const uint32_t *v = (const uint32_t *)out.contents;
            readv("second-marker", "words 0-3: %08x %08x %08x %08x", v[0], v[1], v[2], v[3]);
        }
        done(shape);
        return 0;
    }

    id<MTLRenderPipelineState> ps = draw_pipeline(lib, support);
    if (ps == nil) { return 2; }
    id<MTLBuffer> pts = quad_points();
    id<MTLIndirectCommandBuffer> b = icb(MTLIndirectCommandTypeDraw, NO, 4);
    if (b == nil) { readv("buffer", "nil"); return 2; }
    id<MTLIndirectRenderCommand> cmd = [b indirectRenderCommandAtIndex:0];
    [cmd setRenderPipelineState:ps];
    readv("setter", "setRenderPipelineState: returned");
    [cmd setVertexBuffer:pts offset:0 atIndex:0];
    [cmd drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4
          instanceCount:1 baseInstance:0];
    readv("draw", "drawPrimitives: returned");
    if (strcmp(leg, "setter") == 0) { done(shape); return 0; }
    execute_render(b, 1, @[pts]);
    if (strcmp(leg, "again") == 0) {
        id<MTLTexture> t = color_target();
        clear_color(t, -1.0f);
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderPipelineState> plain = draw_pipeline(lib, NO);
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:plain];
        [enc setVertexBuffer:pts offset:0 atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
        commit_and_read(cb, "second-command-buffer");
        read_texels(t, "second-target-after");
    }
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// `CommandType`: a draw of a kind the buffer's command types lack. The index buffer sits in a
// placement heap with a 0xEE guard next to it, so an indexed draw that paints nothing but reads or
// writes past its index buffer is still a reading. Two controls: the same indexed draw on a buffer
// of `DrawIndexed`, which must paint; and a draw of primitives on the same `Draw` buffer, which
// must paint. A run where all three read the same has measured nothing.

static int group_kind(const char *shape) {
    // kind-<draw|indexed|patch|mesh>-on-<draw|indexed|patches|mesh>
    const char *which = shape + 5;
    const char *on = strstr(which, "-on-");
    if (on == NULL) { return 3; }
    char drawn[16] = {0};
    strncpy(drawn, which, (size_t)(on - which) < 15 ? (size_t)(on - which) : 15);
    const char *buffer_kind = on + 4;

    MTLIndirectCommandType types;
    if (strcmp(buffer_kind, "draw") == 0) { types = MTLIndirectCommandTypeDraw; }
    else if (strcmp(buffer_kind, "indexed") == 0) { types = MTLIndirectCommandTypeDrawIndexed; }
    else if (strcmp(buffer_kind, "patches") == 0) { types = MTLIndirectCommandTypeDrawPatches; }
    else if (strcmp(buffer_kind, "mesh") == 0) {
        types = MTLIndirectCommandTypeDrawMeshThreadgroups;
    } else { return 3; }
    BOOL mesh_state = strcmp(drawn, "mesh") == 0 || strcmp(buffer_kind, "mesh") == 0;

    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }
    id<MTLRenderPipelineState> ps = mesh_state ? mesh_pipeline(lib, YES) : draw_pipeline(lib, YES);
    if (ps == nil) { return 2; }
    id<MTLBuffer> pts = quad_points();
    id<MTLIndirectCommandBuffer> b = icb(types, NO, 4);
    readv("buffer", "commandTypes 0x%lx made %s", (unsigned long)types, b ? "yes" : "nil");
    if (b == nil) { return 2; }

    // 64 bytes of indices -- 16 of them -- with a guard right after.
    Guarded g = guarded_buffer(64);
    if (g.target == nil) { readv("heap", "nil"); return 2; }
    {
        uint32_t *w = (uint32_t *)g.target.contents;
        for (NSUInteger i = 0; i < 16; i++) { w[i] = (uint32_t)(i & 3); }
    }

    id<MTLIndirectRenderCommand> cmd = [b indirectRenderCommandAtIndex:0];
    readv("getter", "indirectRenderCommandAtIndex:0 -> %s", cmd ? "an object" : "nil");
    [cmd setRenderPipelineState:ps];
    readv("setter", "setRenderPipelineState: returned");
    if (strcmp(drawn, "indexed") == 0) {
        [cmd setVertexBuffer:pts offset:0 atIndex:0];
        [cmd drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:4
                        indexType:MTLIndexTypeUInt32 indexBuffer:g.target indexBufferOffset:0
                    instanceCount:1 baseVertex:0 baseInstance:0];
        readv("draw", "drawIndexedPrimitives: returned");
    } else if (strcmp(drawn, "patch") == 0) {
        id<MTLBuffer> factors = [gDevice newBufferWithLength:64
                                                    options:MTLResourceStorageModeShared];
        [cmd drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil
patchIndexBufferOffset:0 instanceCount:1 baseInstance:0
     tessellationFactorBuffer:factors tessellationFactorBufferOffset:0
tessellationFactorBufferInstanceStride:0];
        readv("draw", "drawPatches: returned");
    } else if (strcmp(drawn, "mesh") == 0) {
        [cmd drawMeshThreadgroups:MTLSizeMake(1, 1, 1)
      threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
        threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
        readv("draw", "drawMeshThreadgroups: returned");
    } else if (strcmp(drawn, "draw") == 0) {
        [cmd setVertexBuffer:pts offset:0 atIndex:0];
        [cmd drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4
              instanceCount:1 baseInstance:0];
        readv("draw", "drawPrimitives: returned");
    } else {
        return 3;
    }
    execute_render(b, 1, @[pts, g.target]);
    read_guard(g, "guard-after");
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------

static const char *const kShapes[] = {
    // BindCount: the count on the buffer kind whose layout it moves, and on the kind that ignores
    // it (the control that says "ignored", not "accepted").
    "bind-kernel-on-compute-4", "bind-kernel-on-compute-31", "bind-kernel-on-compute-32",
    "bind-kernel-on-compute-33", "bind-kernel-on-compute-1024",
    "bind-kernel-on-compute-65535", "bind-kernel-on-compute-65536",
    "bind-kernel-on-compute-65537", "bind-kernel-on-compute-70000",
    "bind-kernel-on-compute-2p20", "bind-kernel-on-compute-2p24",
    "bind-kernel-on-compute-2p25", "bind-kernel-on-compute-2p26",
    "bind-kernel-on-compute-2p31", "bind-kernel-on-compute-2p32m1",
    "bind-kernel-on-draw-4", "bind-kernel-on-draw-65536", "bind-kernel-on-draw-2p31",
    "bind-vertex-on-draw-4", "bind-vertex-on-draw-1024", "bind-vertex-on-draw-65536",
    "bind-vertex-on-draw-65537", "bind-vertex-on-draw-2p31", "bind-vertex-on-draw-2p32m1",
    "bind-fragment-on-draw-65536", "bind-fragment-on-draw-2p31",
    "bind-object-on-draw-65536", "bind-mesh-on-draw-65536",
    "bind-tglen-on-compute-31", "bind-tglen-on-compute-32", "bind-tglen-on-compute-1024",
    "bind-tglen-on-compute-65536", "bind-tglen-on-compute-65537",
    "bind-tglen-on-compute-2p31", "bind-tglen-on-compute-2p32m1",
    // Does a bind past the record's 32 slots, and past the count, land at all?
    "keep-1024-at-0", "keep-1024-at-40", "keep-1024-at-1000", "keep-1024-at-2000",
    "keep-70000-at-40", "keep-70000-at-69999", "keep-65537-at-65536",
    "keep-2p20-at-1048575", "keep-2p31-at-40", "keep-2p31-at-2p31",
    // NoCommandTypes: the getter, each setter, the CPU reset, the execution, the optimize, the
    // copies and the argument-buffer encoding, on a buffer of no command types and on one of the
    // matching type.
    "types-none-make", "types-draw-make", "types-disp-make",
    "types-none-getter-render", "types-draw-getter-render",
    "types-none-getter-compute", "types-disp-getter-compute",
    "types-none-r-pipeline", "types-draw-r-pipeline",
    "types-none-r-vbuf", "types-draw-r-vbuf",
    "types-none-r-draw", "types-draw-r-draw",
    "types-none-r-reset", "types-draw-r-reset",
    "types-none-r-fbuf", "types-draw-r-fbuf",
    "types-none-r-obuf", "types-draw-r-obuf",
    "types-none-r-mbuf", "types-draw-r-mbuf",
    "types-none-r-idraw", "types-draw-r-idraw",
    "types-none-r-mdraw", "types-draw-r-mdraw",
    "types-none-r-barrier", "types-draw-r-barrier",
    "types-none-r-clearbarrier", "types-draw-r-clearbarrier",
    "types-none-r-objtglen", "types-draw-r-objtglen",
    "types-none-c-pipeline", "types-disp-c-pipeline",
    "types-none-c-kbuf", "types-disp-c-kbuf",
    "types-none-c-dispatch", "types-disp-c-dispatch",
    "types-none-c-tglen", "types-disp-c-tglen",
    "types-none-c-barrier", "types-disp-c-barrier",
    "types-none-c-reset", "types-disp-c-reset",
    "types-none-c-clearbarrier", "types-disp-c-clearbarrier",
    "types-none-c-dthreads", "types-disp-c-dthreads",
    "types-none-c-stagein", "types-disp-c-stagein",
    "types-none-c-imageblock", "types-disp-c-imageblock",
    "types-none-c-kbufstride", "types-disp-c-kbufstride",
    "types-none-rangereset", "types-draw-rangereset",
    "types-none-rangereset0", "types-draw-rangereset0",
    "types-none-exec", "types-draw-exec",
    "types-none-taken-exec", "types-draw-taken-exec",
    "types-none-optimize", "types-draw-optimize",
    "types-none-copy-to-draw", "types-none-copy-from-draw", "types-none-copy-to-none",
    "types-draw-copy-to-draw",
    "types-none-argenc", "types-draw-argenc",
    // PipelineWithoutSupport: the setter apart from the execution, render and compute, and whether
    // the next command buffer still completes.
    "support-yes-render-setter", "support-no-render-setter",
    "support-yes-render-exec", "support-no-render-exec",
    "support-yes-render-again", "support-no-render-again",
    "support-yes-compute-setter", "support-no-compute-setter",
    "support-yes-compute-exec", "support-no-compute-exec",
    "support-yes-compute-again", "support-no-compute-again",
    // CommandType: a draw of a kind the buffer's types lack, with both controls.
    "kind-draw-on-draw", "kind-indexed-on-indexed", "kind-indexed-on-draw",
    "kind-indexed-on-patches", "kind-patch-on-draw", "kind-patch-on-patches",
    "kind-mesh-on-mesh", "kind-mesh-on-draw", "kind-draw-on-mesh",
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

        if (strncmp(shape, "bind-", 5) == 0) { return group_bind(shape); }
        if (strncmp(shape, "keep-", 5) == 0) { return group_keep(shape); }
        if (strncmp(shape, "types-", 6) == 0) { return group_types(shape); }
        if (strncmp(shape, "support-", 8) == 0) { return group_support(shape); }
        if (strncmp(shape, "kind-", 5) == 0) { return group_kind(shape); }
        fprintf(stderr, "no such shape: %s\n", shape);
        return 2;
    }
}
