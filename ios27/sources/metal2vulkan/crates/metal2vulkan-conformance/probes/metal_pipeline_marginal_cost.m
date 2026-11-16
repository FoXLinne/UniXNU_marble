// Whether METAL amortises a render pipeline state across the family of states that share one
// vertex-and-fragment pair, and over which kinds of variation it does.
//
// THE QUESTION. `known-slow-calls.tsv`'s per-case figures say Metal's pipeline creation gets
// CHEAPER as a case makes more pipelines -- a median 122.3 us in cases making one, 31.0 us in
// cases making 5 to 15, 26.1 us in cases making 36 or more -- while ours reads flat to within
// 1.4x. A sibling probe (`pipeline_cache_across_blend_states.c`) measured MoltenVK's
// `pipelineCompile` activity at 32 calls over 32 blend states in EVERY cache arrangement and
// concluded that part is the floor: Metal builds a distinct `MTLRenderPipelineState` per blend
// state whatever any cache holds.
//
// Those two readings disagree, and only one of them was taken on Metal's own API. The 4.7x is a
// comparison of CASE BUCKETS, not of a first pipeline against its own siblings: the cases making
// many pipelines are not the cases making one, so case mix alone could produce it. This probe
// asks Metal directly, in one process: what does a state's FIRST pipeline cost, and what does its
// SECOND cost when the only thing that changed is blend state, or rasterizer state, or the vertex
// descriptor, or a function constant, or the functions themselves?
//
// WHAT IT PRINTS, per arrangement: every creation's wall microseconds, then the first, the mean of
// the rest, the median of the rest, and MARGINAL/FIRST as a ratio. A ratio within one arrangement
// of one process survives a loaded host where no absolute figure does.
//
// ARRANGEMENTS (argv[1], or `all` for every one in this order):
//   blend        N states over one function pair, differing only in (source, destination) colour
//                blend factors with blending enabled. The shape `known-slow-calls.tsv`'s
//                blend-sweep cases make and the shape the sibling probe measured.
//   raster       N states differing only in fields that reach no shader at all on any
//                implementation: `rasterizationEnabled` stays YES and the cull mode, winding and
//                fill mode are not even on the descriptor (they are encoder state in Metal), so
//                what varies here is the sample count and the write mask -- the cheapest
//                variation a descriptor can carry.
//   writemask    N states differing only in `writeMask`, with blending off. On an Apple GPU the
//                write mask reaches the fragment epilogue, so this separates "any descriptor
//                change costs a compile" from "a change that reaches the epilogue does".
//   vertexdesc   N states differing only in one vertex attribute's format. Vertex fetch is in the
//                shader on an Apple GPU, so this should cost what a recompile costs.
//   fnconst      N states whose fragment function is specialized from the same AIR with a
//                different `[[function_constant]]` value: a distinct MTLFunction each, so Metal
//                cannot share a compiled function across them. The control for "a genuinely new
//                function costs more than a new state over an old one".
//   distinctfn   N states each from its OWN fragment source text (the nonce differs), so no two
//                share a library, a function or anything Metal could have cached. The upper
//                control: this is what N unamortised first pipelines cost.
//   same         N creations of the IDENTICAL descriptor. The lower control: whatever Metal
//                memoises for a repeat is the floor no arrangement can beat, and if Metal returns
//                distinct objects it says that too (it prints whether the pointers differ).
//
// argv[2], optional: N, the states an arrangement makes (default 32).
// argv[3], optional: the fragment function's size in statements (default 0, a trivial return).
//          Conversion and compile cost scale with the function, and a trivial one measures the
//          per-state overhead alone. The sibling probe's large leg was 88235 SPIR-V words; 2000
//          statements here is the same shape of body.
//
// MTLCompilerService IS A CROSS-PROCESS CACHE and it is the confounder that decides this
// measurement. It keeps compiled output keyed by the source text, so a second process over the
// same shader text compiles nothing. Every source this probe builds therefore carries a NONCE --
// the pid, the wall clock and the arrangement's name -- in a comment and in a live constant, so
// no run of it reads another run's compile. That is why `distinctfn` is a real control and not a
// cache hit, and why the first arrangement of a process is not systematically dearer than the
// rest.
//
// TAKE THE RATIOS, NOT THE MICROSECONDS. This machine is shared. Every arrangement runs back to
// back in one process so each meets the same load, and the verdict is read off MARGINAL/FIRST
// inside an arrangement and off one arrangement's marginal against another's, never off an
// absolute figure.
//
// WHAT IT READ, Apple M3 Max, macOS 27.0.1, MoltenVK not involved (plain Metal), other work on
// the machine throughout (load averages 18 to 119 over the runs). Every figure is a MEDIAN inside
// one process and every verdict is a ratio taken inside that process; the microseconds are what was
// read on a shared host and are not a result.
//
// READ THE COLD COLUMN. The two columns differ in ONE thing: whether an earlier process had already
// compiled the same AIR. `cold` is this probe as it stands, its nonce a live constant, so every
// arrangement's compile is its own. `warm` is the same binary with the nonce in a COMMENT alone --
// which is how it was first written, and the mistake is kept here because the gap between the
// columns is the most useful thing it measured.
//
//   arrangement   cold, trivial fragment   warm, trivial   2000 statements   what varies
//   blend             4052 / 4134 us          27 / 57 us    3.1 to 8.6 s     colour blend factors
//   writemask         3644 / 3618 us               26 us    3.1 to 3.4 s     the write mask
//   raster            3990 / 3630 us               27 us    3.3 to 3.5 s     samples, a2c, a2one
//   vertexdesc           70 / 45 us               31 us          40 us       an attribute's format
//   fnconst           4783 / 4526 us               27 us         2.95 s      a specialized function
//   distinctfn        4664 / 4803 us              188 us         1016 us     its own library each
//   same (a repeat)         5 us            5 to 11 us            10 us      nothing
//
// (The 2000-statement column was taken with the comment nonce. Its `blend`, `writemask`, `raster`
// and `fnconst` rows are nevertheless COLD, because no earlier process had compiled those states;
// its `distinctfn` row is WARM, and that is why 1016 us sits beside a 125.7 ms library compile.)
//
// SO A BLEND STATE IS PART OF THE FRAGMENT PROGRAM ON AN APPLE GPU, AND CHANGING IT COSTS A
// FUNCTION COMPILE. Cold, over a trivial fragment, a distinct blend state costs 4052 us and a
// whole distinct specialized MTLFunction costs 4783 us -- the same work to within 1.2x. Over a
// 2000-statement fragment the blend state costs 3.1 to 8.6 SECONDS. The cost follows the fragment
// function by three orders of magnitude and equals the cost of compiling a new function, which is
// what a recompile does and what reading a compiled pipeline out of a cache does not. The write
// mask and the sample state read the same, because they reach the same epilogue.
//
// A CHANGE THAT REACHES ONE STAGE RECOMPILES THAT STAGE ALONE. `vertexdesc` costs 45 to 70 us cold
// over a trivial fragment and 40 us over a 2000-statement one, against 3.1 seconds for a blend
// state over the same pair: the vertex descriptor reaches the (trivial) vertex function and nothing
// else. So the recompile is per stage and keyed by what the state reaches, not per pipeline, and
// ~70x separates a vertex-side change from a fragment-side one at equal fragment size.
//
// AN EXACT REPEAT IS 5 TO 11 US AND IS A DISTINCT OBJECT (32 of 32, and 16 of 16). Metal memoises
// the compiled state by the descriptor's content and hands back a new `MTLRenderPipelineState`
// around it. With `distinctfn` cold at 4664 us -- a library compiled at 4912 us and then a pipeline
// costing as much again -- the fragment program is finalized AT PIPELINE CREATION every time, and
// the exact-descriptor memo is the only thing Metal offers that skips it.
//
// WHERE THAT CACHE IS, and what it means for every later measurement. Three processes minutes
// apart in one batch, under one load: this binary with its live nonce read a marginal blend state
// at 4474 us, a SECOND run of it (a new nonce, so cold again) at 4942 us, and the comment-nonce
// binary -- whose AIR every run of it shares, and which an earlier process had compiled 35 minutes
// before -- at 67 us. 66x to 74x, with nothing differing but whether some earlier process had
// compiled that AIR, and with every one of those earlier processes long exited. So the cache
// outlives the process that filled it and the MTLCompilerService instance that served it, and
// `killall MTLCompilerService` would not have made anything cold.
//
// The files are at `$(getconf DARWIN_USER_CACHE_DIR)com.apple.metal/<driver id>/`, as
// `functions*.data` and `functions*.list` -- about 1.9 GiB of them on this machine, whose mtimes
// advance while a cold compile runs. Append-structured, so the directory's ENTRY COUNT does not
// move (322 before and after three runs here, which is what made the first attempt at this look
// negative). Not isolated from other lanes' concurrent Metal work, so the mtimes are not proof
// that a particular write was this probe's; the 66x cold-against-warm reading above is the
// measurement, and the files are where it lives.
//
// THE RULE THAT FOLLOWS: on this machine a given AIR is cold ONCE, EVER. A probe that wants a cold
// Metal compile must vary a LIVE constant on every run -- this one takes its pid and the wall
// clock -- and may never reuse a source it has already run. Waiting does not restore cold.
//
// MTLCompilerService KEYS ITS COMPILED OUTPUT ON THE AIR AND KEEPS IT ACROSS PROCESSES. That is the
// whole of the cold/warm gap above: 150x on `blend`, 177x on `fnconst`, 25x on `distinctfn`, and
// 1.5x on `vertexdesc`, whose work is small enough that the call's own overhead dominates. A nonce
// in a comment does not reach the AIR and buys nothing. ANY MEASUREMENT OF A METAL COMPILE IN THIS
// REPOSITORY MUST SAY WHICH COLUMN IT IS IN, and a probe that wants the cold one must vary a LIVE
// constant.
//
// AND THAT IS THE FIRST-PROCESS ANOMALY. A sibling probe (`pipeline_cache_across_blend_states.c`)
// recorded the first process of a batch spending about 8.3 SECONDS a pipeline over an 88235-word
// fragment where every later process spent about 1 ms, tracking process position and not
// arrangement, and could not explain it. Its processes all handed the driver the same SPIR-V, so
// the same MSL and the same AIR: the first paid real compiles and the rest read that
// cross-process cache. The same shape, measured here with the mechanism isolated by a one-character
// change to a comment, is 4052 us against 27 us at a trivial fragment -- 150x, where theirs was
// 8000x at a module 960 times larger. It is not an anomaly and nothing about it is MoltenVK's; it
// is this cache, and the rule it implies is the paragraph above.
//
// TWO READINGS THAT LOOK WRONG AND ARE THE MEMO. `writemask`'s last creation reads 13 to 27 us
// where its siblings read thousands: the mask it sets (15, every channel) is the one the shared
// base descriptor already used, so Metal answered it out of the memo. `vertexdesc`'s FIRST reads 5
// to 10 us for the same reason (its format 0 is the base's). Neither is an arrangement's cost; both
// are the memo measured by accident, and they agree with `same`.
//
// WHAT THAT SETTLES, against `known-slow-calls.tsv`'s reading that Metal's median render pipeline
// creation falls 4.7x as a case makes more of them (122.3 us at one, 31.0 at 5-15, 26.1 at 36 or
// more) while ours reads flat: THE FALL IS NOT A PER-PIPELINE AMORTISATION THAT A VULKAN DRIVER
// COULD BE FAILING TO USE. Metal has exactly three cheap answers -- an identical descriptor (5 to
// 11 us), a change that reaches only a small stage (45 us), and an AIR some process has compiled
// before (27 us against 4052) -- and a case making 36 distinct fragment-side states on a cold
// machine gets none of them. Every figure in that 26.1 us column is one of the three. A sibling
// probe's finding that MoltenVK's `pipelineCompile` runs 32 times over 32 blend states in every
// cache arrangement is therefore right about the floor as well as the count: Metal charges 32
// fragment recompiles for them on its own API, with no Vulkan in the process.
//
// BUILD AND RUN:
//   clang -O2 -fobjc-arc -framework Metal -framework Foundation \
//     -o /tmp/mpmc crates/metal2vulkan-conformance/probes/metal_pipeline_marginal_cost.m
//   /tmp/mpmc all 32 0
//   /tmp/mpmc all 32 2000
//
// Run it WITHOUT MTL_DEBUG_LAYER: plain Metal is the oracle here.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdlib.h>

static double now_us(void) {
  return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1e3;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/// The source of one vertex and one fragment function, with `nonce` in a comment and in a live
/// constant so MTLCompilerService holds no earlier run's compile of it, a body of `statements`
/// fused multiply-adds, and a `[[function_constant(0)]]` the fragment reads.
static NSString *sourceWithNonce(NSString *nonce, int statements) {
  NSMutableString *s = [NSMutableString string];
  [s appendFormat:@"// nonce %@\n", nonce];
  [s appendString:@"#include <metal_stdlib>\n"
                   "using namespace metal;\n"
                   "constant float kc [[function_constant(0)]];\n"];
  // The nonce in a LIVE constant, not only the comment above it. A comment does not reach the AIR
  // and MTLCompilerService keys its compiled output on the AIR, so two sources differing only in a
  // comment read one compile across processes: measured here, where a specialized function cost
  // 3707 us in one process and 27 us in the next over a source whose only difference was that
  // comment. With the nonce live, every arrangement's compile is this run's own.
  [s appendFormat:@"constant float kn = %.1f;\n", (double)(nonce.hash % 100000) + 0.5];
  [s appendString:@""
                   "struct VIn { float4 pos [[attribute(0)]]; float4 uv [[attribute(1)]]; };\n"
                   "struct VOut { float4 pos [[position]]; float4 col; };\n"
                   "vertex VOut vmain(VIn in [[stage_in]]) {\n"
                   "  VOut o; o.pos = in.pos; o.col = in.uv * 0.5 + 0.5; return o;\n"
                   "}\n"
                   "fragment float4 fmain(VOut in [[stage_in]]) {\n"
                   "  float4 a = in.col + kc + kn * 1e-9;\n"];
  for (int i = 0; i < statements; i++) {
    [s appendFormat:@"  a = fma(a, float4(%d.0), sin(a.yzwx * %d.5)) * 0.5;\n", (i % 7) + 1,
                    (i % 11) + 1];
  }
  [s appendString:@"  return a;\n}\n"];
  return s;
}

/// `source` compiled, or exits.
static id<MTLLibrary> libraryOf(id<MTLDevice> device, NSString *source, double *took_us) {
  MTLCompileOptions *options = [MTLCompileOptions new];
  NSError *error = nil;
  double at = now_us();
  id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
  *took_us = now_us() - at;
  if (!library) {
    fprintf(stderr, "newLibraryWithSource: %s\n", error.localizedDescription.UTF8String);
    exit(2);
  }
  return library;
}

/// The two attributes and one buffer layout every arrangement's descriptor fetches, `format` the
/// second attribute's.
static MTLVertexDescriptor *vertexDescriptor(MTLVertexFormat format) {
  MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
  vd.attributes[0].format = MTLVertexFormatFloat4;
  vd.attributes[0].offset = 0;
  vd.attributes[0].bufferIndex = 0;
  vd.attributes[1].format = format;
  vd.attributes[1].offset = 16;
  vd.attributes[1].bufferIndex = 0;
  vd.layouts[0].stride = 64;
  vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
  return vd;
}

/// The median of `us[from..to)`, which it sorts a copy of.
static double median_of(const double *us, int from, int to) {
  int n = to - from;
  if (n <= 0) {
    return 0;
  }
  double *sorted = malloc(sizeof(double) * (size_t)n);
  memcpy(sorted, us + from, sizeof(double) * (size_t)n);
  qsort(sorted, (size_t)n, sizeof(double), cmp_double);
  double median = sorted[n / 2];
  free(sorted);
  return median;
}

/// Report `n` creation times under `name`, where the arrangement has only `distinct` descriptors
/// and creation `distinct` onward repeats one Metal has already been handed. Three figures: the
/// FIRST creation (which carries whatever MTLCompilerService had to warm), the median of the other
/// DISTINCT ones (Metal's marginal NEW pipeline), and the median of the REPEATS (Metal's memo).
/// The ratios are read inside this one process, so both arms met the same load.
static void report(const char *name, double *us, int n, int distinct, const char *note) {
  printf("%-11s each us:", name);
  for (int i = 0; i < n; i++) {
    printf("%s %.0f", i == distinct ? " |" : "", us[i]);
  }
  printf("\n");
  if (distinct > n) {
    distinct = n;
  }
  double marginal = median_of(us, 1, distinct);
  printf("%-11s first %.0f us", name, us[0]);
  if (distinct > 1) {
    printf("  marginal of %d more distinct: median %.0f us (marginal/first %.3f)", distinct - 1,
           marginal, marginal / us[0]);
  }
  if (n > distinct) {
    double repeat = median_of(us, distinct, n);
    double against = distinct > 1 ? marginal : us[0];
    printf("  repeats of %d: median %.0f us (repeat/new %.4f)", n - distinct, repeat,
           repeat / against);
  }
  printf("%s%s\n", note ? "  " : "", note ? note : "");
}

int main(int argc, const char **argv) {
  @autoreleasepool {
    const char *which = argc > 1 ? argv[1] : "all";
    int n = argc > 2 ? atoi(argv[2]) : 32;
    int statements = argc > 3 ? atoi(argv[3]) : 0;
    if (n < 2) {
      fprintf(stderr, "N must be 2 or more\n");
      return 2;
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      fprintf(stderr, "no Metal device\n");
      return 2;
    }
    printf("device %s, N %d, fragment statements %d, pid %d\n", device.name.UTF8String, n,
           statements, getpid());

    NSString *stamp =
        [NSString stringWithFormat:@"%d-%llu", getpid(), clock_gettime_nsec_np(CLOCK_REALTIME)];
    double *us = malloc(sizeof(double) * (size_t)n);
    BOOL all = strcmp(which, "all") == 0;

    // The shared library every arrangement but `distinctfn` draws its functions from. Its compile
    // is where Metal compiles the AIR, and it is reported because it is the cost our leg does not
    // pay at creation (we translate per variant) and Metal's per-case total holds.
    double library_us = 0;
    NSString *shared_source =
        sourceWithNonce([NSString stringWithFormat:@"%@-shared", stamp], statements);
    id<MTLLibrary> shared = libraryOf(device, shared_source, &library_us);
    printf("library     newLibraryWithSource %.0f us (%lu source bytes)\n", library_us,
           (unsigned long)shared_source.length);

    MTLFunctionConstantValues *zero = [MTLFunctionConstantValues new];
    float kc = 0.25f;
    [zero setConstantValue:&kc type:MTLDataTypeFloat atIndex:0];
    NSError *error = nil;
    id<MTLFunction> vertex = [shared newFunctionWithName:@"vmain"
                                          constantValues:zero
                                                   error:&error];
    id<MTLFunction> fragment = [shared newFunctionWithName:@"fmain"
                                            constantValues:zero
                                                     error:&error];
    if (!vertex || !fragment) {
      fprintf(stderr, "newFunctionWithName: %s\n", error.localizedDescription.UTF8String);
      return 2;
    }

    // What every arrangement shares: the functions, one RGBA8Unorm target, the vertex descriptor.
    MTLRenderPipelineDescriptor *(^base)(void) = ^MTLRenderPipelineDescriptor *(void) {
      MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
      d.vertexFunction = vertex;
      d.fragmentFunction = fragment;
      d.vertexDescriptor = vertexDescriptor(MTLVertexFormatFloat4);
      d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
      return d;
    };

    // Each arrangement's `i`th descriptor, and the pipeline states kept so no creation reuses a
    // released state's place.
    NSMutableArray *kept = [NSMutableArray array];

    const MTLBlendFactor factors[8] = {
        MTLBlendFactorZero,             MTLBlendFactorOne,
        MTLBlendFactorSourceColor,      MTLBlendFactorOneMinusSourceColor,
        MTLBlendFactorSourceAlpha,      MTLBlendFactorOneMinusSourceAlpha,
        MTLBlendFactorDestinationColor, MTLBlendFactorDestinationAlpha,
    };
    // Only formats a `float4 [[attribute]]` may read: Metal refuses an integer format there
    // ("cannot be read using MTLAttributeFormatInt4"), which would end the process.
    const MTLVertexFormat formats[6] = {
        MTLVertexFormatFloat4,           MTLVertexFormatHalf4,
        MTLVertexFormatUChar4Normalized, MTLVertexFormatChar4Normalized,
        MTLVertexFormatUShort4Normalized, MTLVertexFormatShort4Normalized,
    };

    // A creation, timed, kept, exiting on a refusal: an arrangement Metal refuses says so rather
    // than reporting the cost of a failure.
    double (^create)(MTLRenderPipelineDescriptor *, const char *) =
        ^double(MTLRenderPipelineDescriptor *d, const char *name) {
          NSError *e = nil;
          double at = now_us();
          id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d
                                                                                   error:&e];
          double took = now_us() - at;
          if (!state) {
            fprintf(stderr, "%s: newRenderPipelineStateWithDescriptor: %s\n", name,
                    e.localizedDescription.UTF8String);
            exit(2);
          }
          [kept addObject:state];
          return took;
        };

    if (all || strcmp(which, "blend") == 0) {
      for (int i = 0; i < n; i++) {
        MTLRenderPipelineDescriptor *d = base();
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = factors[i % 8];
        d.colorAttachments[0].destinationRGBBlendFactor = factors[(i / 8) % 8];
        d.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        us[i] = create(d, "blend");
      }
      // 64 distinct (source, destination) pairs, so no creation here repeats one.
      report("blend", us, n, n < 64 ? n : 64, "every one a distinct blend state");
    }

    if (all || strcmp(which, "raster") == 0) {
      // The sample count is the only field here that is not a shader's business on any
      // implementation and that Metal takes on the descriptor.
      const NSUInteger samples[2] = {1, 4};
      for (int i = 0; i < n; i++) {
        MTLRenderPipelineDescriptor *d = base();
        d.rasterSampleCount = samples[i % 2];
        d.alphaToCoverageEnabled = ((i / 2) % 2) != 0;
        d.alphaToOneEnabled = ((i / 4) % 2) != 0;
        us[i] = create(d, "raster");
      }
      report("raster", us, n, 8, "sample count / alpha-to-coverage / alpha-to-one; 8 distinct");
    }

    if (all || strcmp(which, "writemask") == 0) {
      for (int i = 0; i < n; i++) {
        MTLRenderPipelineDescriptor *d = base();
        d.colorAttachments[0].writeMask = (MTLColorWriteMask)(i % 16);
        us[i] = create(d, "writemask");
      }
      report("writemask", us, n, 16, "16 distinct masks");
    }

    if (all || strcmp(which, "vertexdesc") == 0) {
      for (int i = 0; i < n; i++) {
        MTLRenderPipelineDescriptor *d = base();
        d.vertexDescriptor = vertexDescriptor(formats[i % 6]);
        us[i] = create(d, "vertexdesc");
      }
      report("vertexdesc", us, n, 6, "6 distinct attribute formats");
    }

    if (all || strcmp(which, "fnconst") == 0) {
      for (int i = 0; i < n; i++) {
        MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
        float value = (float)i + 0.5f;
        [values setConstantValue:&value type:MTLDataTypeFloat atIndex:0];
        NSError *e = nil;
        id<MTLFunction> f = [shared newFunctionWithName:@"fmain" constantValues:values error:&e];
        if (!f) {
          fprintf(stderr, "fnconst: newFunctionWithName: %s\n", e.localizedDescription.UTF8String);
          return 2;
        }
        MTLRenderPipelineDescriptor *d = base();
        d.fragmentFunction = f;
        us[i] = create(d, "fnconst");
      }
      report("fnconst", us, n, n, "a distinct specialized MTLFunction each");
    }

    if (all || strcmp(which, "distinctfn") == 0) {
      double library_total = 0;
      for (int i = 0; i < n; i++) {
        NSString *nonce = [NSString stringWithFormat:@"%@-distinct-%d", stamp, i];
        double one = 0;
        id<MTLLibrary> own = libraryOf(device, sourceWithNonce(nonce, statements), &one);
        library_total += one;
        NSError *e = nil;
        id<MTLFunction> v = [own newFunctionWithName:@"vmain" constantValues:zero error:&e];
        id<MTLFunction> f = [own newFunctionWithName:@"fmain" constantValues:zero error:&e];
        if (!v || !f) {
          fprintf(stderr, "distinctfn: %s\n", e.localizedDescription.UTF8String);
          return 2;
        }
        MTLRenderPipelineDescriptor *d = base();
        d.vertexFunction = v;
        d.fragmentFunction = f;
        us[i] = create(d, "distinctfn");
      }
      printf("distinctfn  its %d libraries compiled in %.0f us (%.0f us each), which is NOT in the "
             "creation figures below\n",
             n, library_total, library_total / n);
      report("distinctfn", us, n, n, "no two share a library");
    }

    if (all || strcmp(which, "same") == 0) {
      NSMutableArray *states = [NSMutableArray array];
      for (int i = 0; i < n; i++) {
        MTLRenderPipelineDescriptor *d = base();
        NSError *e = nil;
        double at = now_us();
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&e];
        us[i] = now_us() - at;
        if (!state) {
          fprintf(stderr, "same: %s\n", e.localizedDescription.UTF8String);
          return 2;
        }
        [states addObject:state];
      }
      int distinct = 1;
      for (int i = 1; i < n; i++) {
        if (states[i] != states[0]) {
          distinct++;
        }
      }
      [kept addObjectsFromArray:states];
      char note[96];
      snprintf(note, sizeof note, "%d of %d are distinct objects", distinct, n);
      report("same", us, n, 1, note);
    }

    printf("kept %lu states\n", (unsigned long)kept.count);
    free(us);
  }
  return 0;
}
