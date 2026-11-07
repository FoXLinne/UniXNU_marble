// What plain Metal does with `setVertexBufferOffset:atIndex:` at an index where NO BUFFER IS
// BOUND, and what the draw after it reads there.
//
// The runtime takes that call in both of its render lowering policies, on the ground that plain
// Metal takes it (`ArgumentTable::take_buffer_offset`). That ground was measured once, at offset
// 64 and index 0; this probe measures the shape a test asserts about -- offset 0 at an index that
// never held a buffer -- and keeps the controls beside it, since "the setter was taken" and "the
// setter did nothing" read identically at an index with nothing to read.
//
// ONE CASE PER PROCESS, chosen on the command line, and every case is GRADED AT THE COMMIT: a
// render-encoder call that returns can still fail the command buffer, so the setter's answer is
// only half the reading.
//
// Each case reports, in this order: the device it ran on and the release; whether the encoder was
// made and whether the setter returned; whether `endEncoding` returned; the command buffer's
// status and error after a bounded wait; the vertex and fragment counts the stages wrote into
// atomics, which is the positive evidence that the pass RAN and which no status gives; and the
// word the vertex function read from `src[0]`, the slot the setter was aimed at.
//
// The cases:
//
//   unbound-offset-0     nothing bound at index 3, then `setVertexBufferOffset:0 atIndex:3`
//   unbound-offset-64    nothing bound at index 3, then `setVertexBufferOffset:64 atIndex:3`
//   never-bound          nothing bound at index 3 and NO setter at all -- the baseline the two
//                        above must read, byte for byte, if the setter changed nothing
//   bound-then-offset    a buffer of distinct words bound at index 3 at offset 0, then
//                        `setVertexBufferOffset:64 atIndex:3` -- the DIFFERENTIATING control: it
//                        must read the word at 64, or the probe has measured nothing about what
//                        the setter does
//   bound-plain          the same buffer bound at index 3 at offset 0 with no setter -- reads the
//                        word at 0, so `bound-then-offset` is read against it
//
// Build (no cargo target needed):
//   clang -fobjc-arc -O1 -fmodules -o buffer_offset_at_an_unbound_index \
//     buffer_offset_at_an_unbound_index.m -framework Foundation -framework Metal
//
// Measuring runs keep `MTL_DEBUG_LAYER` OUT of the environment: the layer is not passive, it
// asserts, and it has been measured to change what a shader reads. Layer messages are taken in
// separate runs with `MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog` and reported as layer
// messages, never as plain Metal's answer.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static void P(const char *fmt, ...) { va_list ap; va_start(ap,fmt);
  fputs("RESULT ",stdout); vfprintf(stdout,fmt,ap); fputc('\n',stdout); fflush(stdout); va_end(ap); }

static id<MTLDevice> D; static id<MTLCommandQueue> Q; static id<MTLLibrary> L;

// `src` is vertex buffer 3, the slot every case aims at. The vertex stage counts itself, reads
// `src[0]` and stores it, and the fragment stage counts itself, so a pass that ran says so.
static const char *SRC =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float4 c; };\n"
"vertex VOut vmain(uint vid [[vertex_id]], device atomic_uint* rep [[buffer(1)]],\n"
"                  const device uint* src [[buffer(3)]]) {\n"
"  float2 p[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n"
"  VOut o; o.pos = float4(p[vid], 0.5, 1.0);\n"
"  atomic_fetch_add_explicit(&rep[1], 1u, memory_order_relaxed);\n"
"  uint w = src[0];\n"
"  atomic_store_explicit(&rep[3], w, memory_order_relaxed);\n"
"  o.c = float4(1.0, 0.5, 0.25, 1.0); return o; }\n"
"fragment float4 fmain(VOut in [[stage_in]], device atomic_uint* rep [[buffer(1)]]) {\n"
"  atomic_fetch_add_explicit(&rep[2], 1u, memory_order_relaxed); return in.c; }\n";

static const char *STATUS[] = {"NotEnqueued","Enqueued","Committed","Scheduled","Completed","Error"};

static void boot(void) {
  D = MTLCreateSystemDefaultDevice(); if (!D) { P("fatal=no-device"); exit(3); }
  P("DEVICE %s / RELEASE %s", [[D name] UTF8String],
    [[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String]);
  Q = [D newCommandQueue];
  NSError *e = nil;
  L = [D newLibraryWithSource:[NSString stringWithUTF8String:SRC] options:nil error:&e];
  if (!L) { P("fatal=library %s", [[e description] UTF8String]); exit(3); }
}

int main(int argc, const char **argv) {
  if (argc != 2) { P("usage: %s <case>", argv[0]); return 2; }
  const char *shape = argv[1];
  int unbound0 = !strcmp(shape, "unbound-offset-0");
  int unbound64 = !strcmp(shape, "unbound-offset-64");
  int never = !strcmp(shape, "never-bound");
  int boundOffset = !strcmp(shape, "bound-then-offset");
  int boundPlain = !strcmp(shape, "bound-plain");
  if (!(unbound0 || unbound64 || never || boundOffset || boundPlain)) {
    P("fatal=unknown-case %s", shape); return 2;
  }
  @autoreleasepool {
    boot();

    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [L newFunctionWithName:@"vmain"];
    pd.fragmentFunction = [L newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError *e = nil;
    id<MTLRenderPipelineState> ps = [D newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) { P("fatal=pipeline %s", [[e description] UTF8String]); return 3; }

    MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];
    td.pixelFormat = MTLPixelFormatRGBA8Unorm; td.width = 16; td.height = 16;
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [D newTextureWithDescriptor:td];

    // The report: [0] unused, [1] vertices, [2] fragments, [3] the word read from `src[0]`.
    id<MTLBuffer> rep = [D newBufferWithLength:64 options:MTLResourceStorageModeShared];
    memset(rep.contents, 0, 64);

    // 256 bytes of distinct words: word 0 is 0x11111111, the word at byte 64 is 0x22222222.
    id<MTLBuffer> src = [D newBufferWithLength:256 options:MTLResourceStorageModeShared];
    unsigned *w = (unsigned *)src.contents;
    for (unsigned i = 0; i < 64; i++) w[i] = 0x33333333u;
    w[0] = 0x11111111u; w[16] = 0x22222222u;

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLCommandBuffer> cb = [Q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    P("encoder=%s", enc ? "made" : "nil");
    if (!enc) return 3;
    [enc setRenderPipelineState:ps];
    [enc setVertexBuffer:rep offset:0 atIndex:1];
    [enc setFragmentBuffer:rep offset:0 atIndex:1];
    if (boundOffset || boundPlain) { [enc setVertexBuffer:src offset:0 atIndex:3]; }
    if (unbound0) { [enc setVertexBufferOffset:0 atIndex:3]; P("setter=returned offset=0 bound=no"); }
    if (unbound64) { [enc setVertexBufferOffset:64 atIndex:3]; P("setter=returned offset=64 bound=no"); }
    if (boundOffset) { [enc setVertexBufferOffset:64 atIndex:3]; P("setter=returned offset=64 bound=yes"); }
    if (never || boundPlain) { P("setter=none"); }
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    P("endEncoding=returned");

    dispatch_semaphore_t s = dispatch_semaphore_create(0);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; dispatch_semaphore_signal(s); }];
    [cb commit];
    P("commit=returned");
    long r = dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(10 * NSEC_PER_SEC)));
    if (r != 0) { P("TIMEOUT after 10s status=%s", ((unsigned)cb.status < 6) ? STATUS[cb.status] : "?"); return 4; }
    NSError *err = cb.error;
    P("status=%s error=%s", ((unsigned)cb.status < 6) ? STATUS[cb.status] : "?",
      err ? [[NSString stringWithFormat:@"domain=%@ code=%ld desc=%@", err.domain, (long)err.code,
             err.localizedDescription] UTF8String] : "none");
    unsigned *rr = (unsigned *)rep.contents;
    P("READ vertices %u", rr[1]);
    P("READ fragments %u", rr[2]);
    P("READ src0 0x%08x", rr[3]);
    P("DONE %s", shape);
  }
  return 0;
}
