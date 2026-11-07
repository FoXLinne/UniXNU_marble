// Plain Metal's own answer, with the API validation layer OFF, for the two indirect-command-buffer
// refusals that reach past it: the argument-buffer encoding of a buffer of no command types
// (`argument_encoder.rs`) and the blit copy between a buffer of no command types and one of
// command types (`blit_indirect.rs`).
//
// Both refusals were first read from a measurement whose shapes each ran in their own process, and
// which read a copy's completion status alone. This probe reads the two things that reading could
// not say:
//
//  - the ID bytes of a buffer of no command types and of a `Draw` buffer SIDE BY SIDE IN ONE
//    PROCESS, with a pair of `Draw` buffers beside them as the control that says what the bytes
//    are (a per-process counter, or something the descriptor decides);
//  - what a copy ACTUALLY MOVED, by reading the destination back through a painting execution:
//    a destination of command types pre-filled with a real draw (so a copy that moves the
//    source's records destroys it, and one that moves nothing leaves it painting), and a round
//    trip Draw -> none -> Draw (so a copy into a buffer of no command types that really carried
//    the records paints at the far end).
//
// Metal places no indirect command buffer in a placement heap (`MTLHeap` has no indirect-command
// -buffer allocation), so no 0xEE guard can sit beside a copy's destination. The guard here sits
// beside the vertex buffer the draws read, at the next 256-aligned offset of a placement heap, and
// every executing shape reads it back.
//
// Build (no GPU needed to build):
//
//   clang -fobjc-arc -fmodules -framework Metal -framework Foundation \
//       -o icb_argument_encode_and_copy_destinations icb_argument_encode_and_copy_destinations.m
//
// Run ONE SHAPE PER PROCESS, WITHOUT MTL_DEBUG_LAYER (the layer is not passive: it asserts). A
// shape whose process Metal ends prints no `DONE`, and its exit status is the reading.
//
//   ./icb_argument_encode_and_copy_destinations <shape>
//   ./icb_argument_encode_and_copy_destinations --list

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

static NSString *const kShaders = @"\
#include <metal_stdlib>\n\
#include <metal_command_buffer>\n\
using namespace metal;\n\
struct VOut { float4 position [[position]]; };\n\
vertex VOut vmain(uint vid [[vertex_id]], const device float2 *pts [[buffer(0)]]) {\n\
    VOut o; o.position = float4(pts[vid], 0.0, 1.0); return o;\n\
}\n\
fragment float4 fmain() { return float4(0.75, 0.5, 0.25, 1.0); }\n\
struct ICBArg { command_buffer cb; };\n\
kernel void cb_size(device ICBArg &ab [[buffer(0)]], device uint *out [[buffer(1)]]) {\n\
    out[0] = 0xC0DE0000u | (uint)ab.cb.size();\n\
}\n\
kernel void cb_reset(device ICBArg &ab [[buffer(0)]], device uint *out [[buffer(1)]]) {\n\
    out[0] = 0xC0DE0000u | (uint)ab.cb.size();\n\
    render_command c(ab.cb, 0);\n\
    c.reset();\n\
    out[1] = 0xBEEF0001u;\n\
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

static id<MTLRenderPipelineState> draw_pipeline(id<MTLLibrary> lib) {
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
    pd.supportIndirectCommandBuffers = YES;
    NSError *err = nil;
    id<MTLRenderPipelineState> ps = [gDevice newRenderPipelineStateWithDescriptor:pd error:&err];
    if (ps == nil) { readv("pipeline", "nil error %s", err.localizedDescription.UTF8String); }
    return ps;
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

// A 0xEE guard beside the vertex buffer the draws read: the buffer at offset 0 of a placement heap
// and the guard at the next 256-aligned offset past it.

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

// The four corners of the whole target, as a triangle strip's vertices, in a guarded placement
// heap.
static Guarded quad_points(void) {
    Guarded g = guarded_buffer(32);
    if (g.target == nil) { return g; }
    const float pts[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    memcpy(g.target.contents, pts, sizeof(pts));
    return g;
}

static id<MTLIndirectCommandBuffer> icb(MTLIndirectCommandType types, NSUInteger count) {
    MTLIndirectCommandBufferDescriptor *d = [[MTLIndirectCommandBufferDescriptor alloc] init];
    d.commandTypes = types;
    d.inheritPipelineState = NO;
    d.inheritBuffers = NO;
    d.maxVertexBufferBindCount = 4;
    d.maxFragmentBufferBindCount = 4;
    d.maxKernelBufferBindCount = 4;
    return [gDevice newIndirectCommandBufferWithDescriptor:d maxCommandCount:count
                                                  options:MTLResourceStorageModeShared];
}

// ---------------------------------------------------------------------------------------------
// The encoding. `enc-<spelling>`: the ID bytes of each buffer the spelling names, encoded in ONE
// process, into the slots of ONE argument buffer. `pair` is the reading the refusal needs; `draws`
// is the control that says what the bytes are.

static void dump_words(id<MTLBuffer> arg, NSUInteger slots, NSUInteger stride) {
    const unsigned char *p = (const unsigned char *)arg.contents;
    for (NSUInteger s = 0; s < slots; s++) {
        char line[64];
        size_t at = 0;
        for (size_t i = 0; i < 8; i++) {
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x", p[s * stride + i]);
        }
        char key[32];
        snprintf(key, sizeof(key), "argument-slot-%lu", (unsigned long)s);
        readv(key, "%s", line);
    }
}

static int group_enc(const char *shape) {
    const char *which = shape + 4;
    MTLIndirectCommandType first, second;
    if (strcmp(which, "pair") == 0) {
        first = 0;
        second = MTLIndirectCommandTypeDraw;
    } else if (strcmp(which, "pair-draw-first") == 0) {
        first = MTLIndirectCommandTypeDraw;
        second = 0;
    } else if (strcmp(which, "draws") == 0) {
        first = MTLIndirectCommandTypeDraw;
        second = MTLIndirectCommandTypeDraw;
    } else if (strcmp(which, "nones") == 0) {
        first = 0;
        second = 0;
    } else if (strcmp(which, "solo-none") == 0) {
        first = 0;
        second = (MTLIndirectCommandType)~0ull;
    } else if (strcmp(which, "solo-draw") == 0) {
        first = MTLIndirectCommandTypeDraw;
        second = (MTLIndirectCommandType)~0ull;
    } else {
        return 3;
    }
    BOOL solo = second == (MTLIndirectCommandType)~0ull;

    MTLArgumentDescriptor *a0 = [MTLArgumentDescriptor argumentDescriptor];
    a0.dataType = MTLDataTypeIndirectCommandBuffer;
    a0.index = 0;
    a0.access = MTLBindingAccessReadOnly;
    MTLArgumentDescriptor *a1 = [MTLArgumentDescriptor argumentDescriptor];
    a1.dataType = MTLDataTypeIndirectCommandBuffer;
    a1.index = 1;
    a1.access = MTLBindingAccessReadOnly;
    id<MTLArgumentEncoder> ae =
        [gDevice newArgumentEncoderWithArguments:solo ? @[a0] : @[a0, a1]];
    readv("encoder", "encodedLength %lu alignment %lu for %d slots",
          (unsigned long)ae.encodedLength, (unsigned long)ae.alignment, solo ? 1 : 2);
    id<MTLBuffer> arg = [gDevice newBufferWithLength:ae.encodedLength
                                            options:MTLResourceStorageModeShared];
    memset(arg.contents, 0, ae.encodedLength);
    [ae setArgumentBuffer:arg offset:0];

    // Made in the order the shape names, so a reading that is a per-process counter shows its
    // order and a reading the descriptor decides does not.
    id<MTLIndirectCommandBuffer> b0 = icb(first, 4);
    id<MTLIndirectCommandBuffer> b1 = solo ? nil : icb(second, 4);
    readv("buffer-0", "commandTypes 0x%lx made %s size %lu allocatedSize %lu",
          (unsigned long)first, b0 ? "yes" : "nil", b0 ? (unsigned long)b0.size : 0ul,
          b0 ? (unsigned long)b0.allocatedSize : 0ul);
    if (!solo) {
        readv("buffer-1", "commandTypes 0x%lx made %s size %lu allocatedSize %lu",
              (unsigned long)second, b1 ? "yes" : "nil", b1 ? (unsigned long)b1.size : 0ul,
              b1 ? (unsigned long)b1.allocatedSize : 0ul);
    }
    if (b0 == nil || (!solo && b1 == nil)) { return 2; }

    [ae setIndirectCommandBuffer:b0 atIndex:0];
    if (!solo) { [ae setIndirectCommandBuffer:b1 atIndex:1]; }
    NSUInteger stride = solo ? 8 : ae.encodedLength / 2;
    dump_words(arg, solo ? 1 : 2, stride);
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// What a shader does with the handle. `gpu-<size|reset>-<none|draw>`: the kernel takes the
// argument buffer the function's own encoder laid out, reads the buffer's `size()` and, for
// `reset`, builds `render_command(cb, 0)` and resets it. The host then executes the buffer in a
// pass: for `draw`, after the shader reset command 0, a draw the host set must be gone.

static int group_gpu(const char *shape) {
    const char *rest = shape + 4;
    BOOL do_reset = strncmp(rest, "reset", 5) == 0;
    const char *kind = strchr(rest, '-');
    if (kind == NULL) { return 3; }
    kind++;
    MTLIndirectCommandType types = strcmp(kind, "draw") == 0 ? MTLIndirectCommandTypeDraw : 0;
    readv("shape", "kernel %s buffer commandTypes 0x%lx", do_reset ? "cb_reset" : "cb_size",
          (unsigned long)types);

    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }
    id<MTLFunction> fn = [lib newFunctionWithName:do_reset ? @"cb_reset" : @"cb_size"];
    if (fn == nil) { readv("function", "nil"); return 2; }
    MTLComputePipelineDescriptor *cd = [[MTLComputePipelineDescriptor alloc] init];
    cd.computeFunction = fn;
    NSError *err = nil;
    id<MTLComputePipelineState> ps =
        [gDevice newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionNone
                                           reflection:nil error:&err];
    if (ps == nil) { readv("pipeline", "nil %s", err.localizedDescription.UTF8String); return 2; }

    id<MTLArgumentEncoder> ae = [fn newArgumentEncoderWithBufferIndex:0];
    readv("encoder", "from the function, encodedLength %lu", (unsigned long)ae.encodedLength);
    id<MTLBuffer> arg = [gDevice newBufferWithLength:ae.encodedLength
                                            options:MTLResourceStorageModeShared];
    memset(arg.contents, 0, ae.encodedLength);
    id<MTLIndirectCommandBuffer> b = icb(types, 4);
    readv("buffer", "made %s size %lu allocatedSize %lu", b ? "yes" : "nil",
          b ? (unsigned long)b.size : 0ul, b ? (unsigned long)b.allocatedSize : 0ul);
    if (b == nil) { return 2; }

    // A `Draw` buffer carries a real draw the shader's reset must remove; a buffer of no command
    // types takes no draw (its setters end the process), so it carries none.
    Guarded g = quad_points();
    if (g.target == nil) { readv("heap", "nil"); return 2; }
    id<MTLRenderPipelineState> rps = draw_pipeline(lib);
    if (rps == nil) { return 2; }
    if (types != 0) {
        id<MTLIndirectRenderCommand> cmd = [b indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:rps];
        [cmd setVertexBuffer:g.target offset:0 atIndex:0];
        [cmd drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4
              instanceCount:1 baseInstance:0];
        readv("host-draw", "command 0 holds a draw of the whole target");
    }

    [ae setArgumentBuffer:arg offset:0];
    [ae setIndirectCommandBuffer:b atIndex:0];
    dump_words(arg, 1, 8);

    id<MTLBuffer> out = [gDevice newBufferWithLength:256 options:MTLResourceStorageModeShared];
    memset(out.contents, 0, 256);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setBuffer:arg offset:0 atIndex:0];
    [enc setBuffer:out offset:0 atIndex:1];
    [enc useResource:b usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding];
    commit_and_read(cb, "kernel-command-buffer");
    const uint32_t *w = (const uint32_t *)out.contents;
    readv("kernel-out", "words 0-3: %08x %08x %08x %08x", w[0], w[1], w[2], w[3]);

    // Then the execution: the draw the host set, as the shader left it.
    id<MTLTexture> target = color_target();
    clear_color(target, -1.0f);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb2 = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> renc = [cb2 renderCommandEncoderWithDescriptor:rp];
    [renc useResource:g.target usage:MTLResourceUsageRead
               stages:MTLRenderStageVertex | MTLRenderStageFragment];
    [renc executeCommandsInBuffer:b withRange:NSMakeRange(0, 1)];
    [renc endEncoding];
    commit_and_read(cb2, "render-command-buffer");
    read_texels(target, "target-after");
    read_guard(g, "guard-after");
    done(shape);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// The copy, read by what the destination then paints.
//
// `copy-<shape>`:
//   into-draw-from-none     a filled `Draw` destination, copied over from a buffer of no command
//                           types: 0.75 says the copy moved nothing, -1 says it moved the
//                           source's records over the draw.
//   into-draw-from-reset    the control for that: the same copy from a FRESH `Draw` buffer, whose
//                           records are reset, which must leave -1.
//   into-draw-from-drawn    the other control: a copy from a `Draw` buffer that holds the draw,
//                           into a destination whose command 0 is reset, which must paint 0.75.
//   roundtrip-through-none  Draw(drawn) -> none -> Draw(reset), then the far end executes: 0.75
//                           says the records really travelled through the buffer of no command
//                           types, -1 says they did not arrive.
//   roundtrip-through-draw  the control for the round trip, through a `Draw` buffer, which must
//                           paint 0.75.
//   none-to-none            two buffers of no command types: the status, and the sizes that say
//                           whether any storage was there to copy.

static void fill_draw(id<MTLIndirectCommandBuffer> b, id<MTLRenderPipelineState> ps,
                      id<MTLBuffer> pts, const char *key) {
    id<MTLIndirectRenderCommand> cmd = [b indirectRenderCommandAtIndex:0];
    [cmd setRenderPipelineState:ps];
    [cmd setVertexBuffer:pts offset:0 atIndex:0];
    [cmd drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4
          instanceCount:1 baseInstance:0];
    readv(key, "command 0 holds a draw of the whole target");
}

static void blit_copy(id<MTLIndirectCommandBuffer> src, NSUInteger count,
                      id<MTLIndirectCommandBuffer> dst, const char *key) {
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyIndirectCommandBuffer:src sourceRange:NSMakeRange(0, count) destination:dst
                  destinationIndex:0];
    [blit endEncoding];
    commit_and_read(cb, key);
}

static void execute_and_read(id<MTLIndirectCommandBuffer> b, Guarded g) {
    id<MTLTexture> target = color_target();
    clear_color(target, -1.0f);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc useResource:g.target usage:MTLResourceUsageRead
              stages:MTLRenderStageVertex | MTLRenderStageFragment];
    [enc executeCommandsInBuffer:b withRange:NSMakeRange(0, 1)];
    [enc endEncoding];
    commit_and_read(cb, "render-command-buffer");
    read_texels(target, "target-after");
    read_guard(g, "guard-after");
}

static int group_copy(const char *shape) {
    const char *which = shape + 5;
    id<MTLIndirectCommandBuffer> none = icb(0, 4);
    id<MTLIndirectCommandBuffer> draw = icb(MTLIndirectCommandTypeDraw, 4);
    readv("sizes", "none size %lu allocatedSize %lu / draw size %lu allocatedSize %lu",
          (unsigned long)none.size, (unsigned long)none.allocatedSize,
          (unsigned long)draw.size, (unsigned long)draw.allocatedSize);

    if (strcmp(which, "none-to-none") == 0) {
        id<MTLIndirectCommandBuffer> other = icb(0, 4);
        blit_copy(none, 4, other, "command-buffer");
        readv("after", "nothing a public call reads: a buffer of no command types has no setter, "
                       "no read-back and an execution that paints nothing");
        done(shape);
        return 0;
    }

    id<MTLLibrary> lib = library();
    if (lib == nil) { return 2; }
    id<MTLRenderPipelineState> ps = draw_pipeline(lib);
    if (ps == nil) { return 2; }
    Guarded g = quad_points();
    if (g.target == nil) { readv("heap", "nil"); return 2; }

    if (strcmp(which, "into-draw-from-none") == 0) {
        fill_draw(draw, ps, g.target, "destination-before");
        blit_copy(none, 4, draw, "command-buffer");
        execute_and_read(draw, g);
        done(shape);
        return 0;
    }
    if (strcmp(which, "into-draw-from-reset") == 0) {
        fill_draw(draw, ps, g.target, "destination-before");
        id<MTLIndirectCommandBuffer> fresh = icb(MTLIndirectCommandTypeDraw, 4);
        readv("source", "a fresh `Draw` buffer, its records reset");
        blit_copy(fresh, 4, draw, "command-buffer");
        execute_and_read(draw, g);
        done(shape);
        return 0;
    }
    if (strcmp(which, "into-draw-from-drawn") == 0) {
        id<MTLIndirectCommandBuffer> src = icb(MTLIndirectCommandTypeDraw, 4);
        fill_draw(src, ps, g.target, "source-before");
        readv("destination", "a fresh `Draw` buffer, its records reset");
        blit_copy(src, 4, draw, "command-buffer");
        execute_and_read(draw, g);
        done(shape);
        return 0;
    }
    if (strncmp(which, "roundtrip-through-", 18) == 0) {
        BOOL through_none = strcmp(which + 18, "none") == 0;
        id<MTLIndirectCommandBuffer> middle =
            through_none ? none : icb(MTLIndirectCommandTypeDraw, 4);
        id<MTLIndirectCommandBuffer> src = icb(MTLIndirectCommandTypeDraw, 4);
        fill_draw(src, ps, g.target, "source-before");
        readv("middle", "commandTypes 0x%lx", (unsigned long)(through_none ? 0ul : 1ul));
        blit_copy(src, 4, middle, "first-command-buffer");
        blit_copy(middle, 4, draw, "second-command-buffer");
        execute_and_read(draw, g);
        done(shape);
        return 0;
    }
    return 3;
}

// ---------------------------------------------------------------------------------------------

static const char *const kShapes[] = {
    // The encoding, side by side in one process, with the controls that say what the bytes are.
    "enc-pair", "enc-pair-draw-first", "enc-draws", "enc-nones",
    "enc-solo-none", "enc-solo-draw",
    // What a shader does with the handle it reads from the argument buffer.
    "gpu-size-none", "gpu-size-draw", "gpu-reset-none", "gpu-reset-draw",
    // What a copy actually moved, read through the destination's own painting.
    "copy-into-draw-from-none", "copy-into-draw-from-reset", "copy-into-draw-from-drawn",
    "copy-roundtrip-through-none", "copy-roundtrip-through-draw", "copy-none-to-none",
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

        if (strncmp(shape, "enc-", 4) == 0) { return group_enc(shape); }
        if (strncmp(shape, "gpu-", 4) == 0) { return group_gpu(shape); }
        if (strncmp(shape, "copy-", 5) == 0) { return group_copy(shape); }
        fprintf(stderr, "no such shape: %s\n", shape);
        return 2;
    }
}
