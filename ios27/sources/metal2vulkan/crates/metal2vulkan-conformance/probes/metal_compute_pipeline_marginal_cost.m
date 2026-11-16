// Whether METAL amortises a COMPUTE pipeline state across the family of states that share one
// kernel and differ only in the stage-input descriptor, and over which fields of that descriptor.
//
// THE QUESTION. `known-slow-calls.tsv`'s `compute_pipeline_with_stage_input` row localises its
// 19.9x to the SECOND pipeline onward: over the 3 cases that make ONE such pipeline we cost
// 41.4 us a call against Metal's 83.4 and are twice as fast, and over the 2 cases that make 16 and
// 29 we cost 791.2 us a call against Metal's 34.5. So Metal's per-call cost reads 2.4x CHEAPER
// with siblings in the case while ours rises 19x, and the row says in its own words that this is a
// description of the figures and not a mechanism.
//
// The render path's identical-looking reading was measured on Metal's own API
// (`probes/metal_pipeline_marginal_cost.m`) and turned out to be three cheap answers of Metal's
// rather than any per-pipeline amortisation: an identical descriptor memoised at 5-11 us; a change
// that reaches only a small stage (45 us for a vertex format where a blend state over the same pair
// is 3.1 seconds); and an AIR that some earlier process compiled, since MTLCompilerService keys its
// output on the AIR and keeps it across processes (27 us warm against 4052 us cold). This probe
// asks the same question of the compute path, in one process, with no Vulkan in it: what does the
// FIRST such pipeline cost, and what does the SECOND cost when the only thing that moved is an
// attribute's format, or its offset, or the layout's stride, or the kernel itself?
//
// WHAT IT VARIES, AND WHY THOSE FIELDS. The cases the row rests on
// (`src/cases/attribute_formats.rs`) sweep ONE kernel, `stage_in_fetch_wide`, over 29 rows whose
// five `[[stage_in]]` attributes carry 29 distinct format tuples, under ONE constant stride, one
// buffer layout stepped by the thread's x position at rate 1. Each row's attribute offset moves
// with its format, because the records lay a slot out per format. So the fields that move between
// siblings are the attributes' FORMATS and their OFFSETS, and nothing else -- which is why
// `formats` and `offsets` are separate arrangements here: one of them being memo-cheap and the
// other dear is the difference between "the decode is the cost" and "any descriptor edit is".
//
// WHAT IT PRINTS, per arrangement: every creation's wall microseconds (and the `newFunctionWithName`
// lookup beside it, because the suite's timed call holds both), then the first, the median of the
// rest, and MARGINAL/FIRST as a ratio. A ratio within one arrangement of one process survives a
// loaded host where no absolute figure does. Load averages are printed before and after.
//
// ARRANGEMENTS (argv[1], or `all` for every one in this order):
//   freshdevice  ONE pipeline each on N devices from N separate `MTLCreateSystemDefaultDevice`
//                calls, which is the shape the SUITE measures in: its timing leg builds its Metal
//                side, and so calls that function, once per CASE. So a case making one such
//                pipeline makes it on a device that has made none, and a case making 29 makes 28
//                of them on a device that has. It prints whether the devices are distinct objects,
//                and the device and library creations are outside the reported figure.
//   same         N creations of the IDENTICAL stage-input descriptor. The lower control: whatever
//                Metal memoises for a repeat is the floor no arrangement can beat, and it prints
//                whether the returned objects are distinct. The suite's `attribute-stride-*` case
//                is this shape, since `MTLBufferLayoutStrideDynamic` leaves one descriptor.
//   released     `same` again, except that each state is released before the next creation, which
//                is what the suite does between cases: its timing leg drains an autorelease pool
//                around each case, so the state a case made is gone before the next one runs. This
//                is whether Metal's memo is a cache of LIVE states or outlives them.
//   formats      N descriptors differing only in the five attributes' FORMATS, each attribute
//                cycling its own numeric class's SDK list exactly as `attribute-format-fetches`
//                does, at fixed offsets. The arrangement the row is about.
//   onefmt       N descriptors differing only in ATTRIBUTE 0's format, the other four fixed. The
//                control for whether the cost follows the number of attributes that moved.
//   narrow       `formats` again over a kernel declaring ONE attribute instead of five, in the same
//                process. The suite's eight cases that make this call span 14.7 to 91.1 us a call
//                on Metal's leg with no order in their sibling counts, and the cheapest of them
//                (`stage-in-region-clamps`) declares one or two attributes where the dearest
//                declare five or more: this is whether the descriptor's WIDTH is what sets a
//                case's level.
//   dynamic      `formats` again with the layout's stride at `MTLBufferLayoutStrideDynamic`, the
//                form the four `attribute-stride-*` cases use -- all four of which make exactly
//                one such pipeline and all four of which read 79 to 86 us on Metal's leg.
//   offsets      N descriptors differing only in attribute 0's byte OFFSET, every format fixed.
//                A descriptor edit that changes no decode: this separates "any stage-input change
//                costs a compile" from "a change to the decode does".
//   stride       N descriptors differing only in the buffer layout's STRIDE, a constant each.
//   nostagein    N creations of a pipeline over a kernel with NO `[[stage_in]]` and no stage-input
//                descriptor, the same descriptor each. The floor of a compute pipeline creation on
//                this device, so the stage input's own share of the figures above is readable.
//   fnconst      N states whose kernel is specialized from the same AIR with a different
//                `[[function_constant]]` value: a distinct MTLFunction each, so Metal cannot share
//                a compiled function across them. The control for "a genuinely new function costs
//                more than a new state over an old one". It is its own entry point in the same
//                library, because a function that DECLARES a constant is unspecialized until one
//                is given and Metal refuses to build a pipeline from it, while every other
//                arrangement looks its function up as the suite's own call does.
//   distinctfn   N states each from its OWN kernel source text (the nonce differs), so no two share
//                a library, a function or anything Metal could have cached. The upper control:
//                what N unamortised first pipelines cost.
//
// argv[2], optional: N, the states an arrangement makes (default 32).
// argv[3], optional: the kernel's size in statements (default 0, a trivial body). Compile cost
//          scales with the function, and a trivial one measures the per-state overhead alone.
// argv[4], optional: THE NONCE. Default is the pid and the wall clock, so every run is COLD: no
//          earlier process has compiled this run's AIR. Passing a literal makes the source
//          reusable, so the FIRST run of a given literal on this machine is cold and every later
//          run of it is WARM -- which is how the two columns below were taken, with no edit to
//          this file.
//
// MTLCompilerService IS A CROSS-PROCESS CACHE and it is the confounder that decides this
// measurement. It keeps compiled output keyed by the AIR, so a second process over the same shader
// text compiles nothing, and `killall MTLCompilerService` does not make anything cold. Every source
// this probe builds therefore carries its nonce in a LIVE CONSTANT as well as a comment -- a
// comment does not reach the AIR and buys nothing. ON THIS MACHINE A GIVEN AIR IS COLD ONCE, EVER;
// waiting does not restore it, and a second reading of one source is a cache hit, never a cold one.
//
// TAKE THE RATIOS, NOT THE MICROSECONDS. This machine is shared. Every arrangement runs back to
// back in one process so each meets the same load, and the verdict is read off MARGINAL/FIRST
// inside an arrangement and off one arrangement's marginal against another's.
//
// WHAT IT READ, Apple M3 Max, macOS 27.0.1 (26A434), MoltenVK not involved (plain Metal), other
// lanes building and running suites on the machine throughout. Every figure is a MEDIAN inside one
// process and every verdict is a ratio taken inside that process; the microseconds are what was
// read on a shared host and are not a result. The warm column was read three times at a one-minute
// load average of 12.7 and three more times at 29.3 and 59.6, and it reproduced to within 3 us
// across all of them -- the work is small and mostly a cache read, so it is the COLD column that
// load moves (its `formats` marginal read 4558 us at load 29 and 7896 us at load 63).
//
// READ THE COLD COLUMN. The two columns differ in ONE thing: whether an earlier process had
// already compiled the same AIR. `cold` is a run whose nonce the machine has never seen; `warm` is
// a second run of the same nonce.
//
//   arrangement    cold            warm         what varies
//   freshdevice    20.4-29.8 ms    4.3-4.9 ms   the FIRST creation of the process; nothing else
//   freshdevice'   3 us            3 us         its 2nd..32nd, one per fresh device
//   same           2-5 us          2-3 us       nothing
//   released       2-3 us          2-3 us       nothing, and each state released before the next
//   formats        4558-7896 us    24-25 us     the five attributes' formats
//   onefmt         4418-6352 us    22-23 us     attribute 0's format alone
//   narrow         3740-5554 us    22-23 us     one attribute's format, on a 1-attribute kernel
//   dynamic        4768-7415 us    22 us        the five formats, under a DYNAMIC stride
//   offsets        4502-8896 us    22-26 us     attribute 0's BYTE OFFSET alone
//   stride         4340-6740 us    23 us        the layout's constant stride alone
//   nostagein      3448-5563 us    18-20 us     its first; no stage input at all
//   fnconst        4782-9466 us    24-25 us     a distinct specialized kernel each
//   distinctfn     5125-7138 us    24-25 us     its own library each
//
// METAL DOES NOT GET CHEAPER WITH SIBLINGS ON THIS PATH. Warm, every arrangement's marginal is
// 22 to 25 us whatever moved -- one format or five, an offset, a stride, a whole new specialized
// kernel, a whole new library -- and every warm marginal/first ratio is 0.71 to 1.04. Cold, every
// one is 3.7 to 9.5 ms. The only figures that FALL inside an arrangement are `formats`' and
// `onefmt`' and `offsets`' and `stride`'s FIRST creations, at 2 to 11 us, and they fall because
// that creation is a MEMO HIT -- its descriptor is the base tuple an earlier arrangement already
// made -- which is why their marginal/first prints 8x to 1643x, upside down. So
// `known-slow-calls.tsv`'s reading that Metal's per-call cost "reads 2.4x cheaper with siblings in
// the case" has no mechanism in Metal's own API: there is nothing here for a sibling to amortise.
//
// AND THE SMALL-STAGE ANSWER THE RENDER PATH HAS DOES NOT EXIST HERE. Of Metal's three cheap
// answers, two are on this path and the third is not:
//
//   - THE IDENTICAL-DESCRIPTOR MEMO, 2 to 3 us, which is content-keyed and hands back a distinct
//     `MTLComputePipelineState` (32 of 32). It survives a fresh `MTLLibrary` over the same AIR
//     (`freshdevice`'s 3 us) and it OUTLIVES THE STATE'S OWN RELEASE (`released`, 2-3 us), so it
//     is not a cache of live objects.
//   - THE CROSS-PROCESS AIR CACHE, 200x: `formats`' marginal is 4929 us cold and 24 us warm.
//   - A CHANGE THAT REACHES ONLY A SMALL STAGE HAS NO ANALOGUE. On the render path a vertex
//     format cost 45 us cold where a blend state over the same pair cost 3.1 seconds. Here, cold,
//     a stage-input descriptor costs a FULL KERNEL COMPILE whatever moved in it: 4929 us for five
//     formats, 4628 for one format, 5022 for ONE BYTE OFFSET and 4876 for a constant stride,
//     against 4984 us for a brand-new specialized kernel and 5334 us for a kernel out of its own
//     library -- the same work to within 1.1x. So the stage-input descriptor is part of the KERNEL
//     PROGRAM on an Apple GPU, the way a blend state is part of the fragment program, and a
//     compute pipeline has no cheaper stage to recompile.
//
// A FOURTH CHEAP ANSWER THE RENDER PROBE DID NOT NAME, and it is the one that looks like an
// amortisation. THE FIRST COMPUTE PIPELINE CREATION OF A PROCESS COSTS 4.3 TO 4.9 MS WARM and
// 20.4 to 29.8 ms cold, while the 31 after it over the same descriptor cost 3 us. So a process
// that makes ONE such pipeline pays a figure three orders of magnitude above the one a process
// making many reads as its median. THAT PREMIUM IS PER PROCESS AND NOT PER DEVICE:
// `MTLCreateSystemDefaultDevice` returns THE SAME OBJECT every time (1 of 32 distinct over 32
// calls, 82 to 141 us each), so a program that builds a Metal side per case -- which the suite's
// timing leg does -- is not getting a fresh device and is not paying this again.
//
// WHAT IS RULED OUT AS A LEVER ON THE LEVEL, each by a pair of medians inside one process: the
// descriptor's WIDTH (one attribute 22-23 us warm and 4012 cold, against five at 24-25 and 4929);
// `MTLBufferLayoutStrideDynamic` (22 us warm, 5057 cold, inside the band); a fresh `MTLLibrary`
// over the same AIR (3 us: the memo still hits); and the state's release (2-3 us).
//
// WHAT THAT LEAVES UNEXPLAINED, said plainly rather than guessed at. The suite's stored whole-set
// recordings have EIGHT cases making `compute_pipeline_with_stage_input`, and Metal's per-call
// figures over them are 14.7 us (28 calls), 24.6 (16), 30.6 (29), 79.4 / 79.8 / 83.4 / 85.5 (one
// each) and 91.1 (36) -- so the DEAREST per call of the eight is the case with the MOST siblings,
// and the row's correlation does not survive its own case set growing. But none of the levers
// above sets a level anywhere near 91 us, and the warm floor measured here for ANY new compute
// pipeline is 22 to 25 us with a memo hit at 2 to 3. Two candidates this probe does not reach:
// the suite's timed window also holds `newFunctionWithName` on an `MTLLibrary` built from metallib
// DATA, where every library here is built from source and its lookup reads 0 to 1 us; and the
// memo's behaviour across the ~1000 intervening cases of a whole-set pass, where nothing here is
// ever more than 32 creations from its neighbour. Both are unaudited.
//
// BUILD AND RUN:
//   clang -O2 -fobjc-arc -framework Metal -framework Foundation \
//     -o /tmp/mcpmc crates/metal2vulkan-conformance/probes/metal_compute_pipeline_marginal_cost.m
//   /tmp/mcpmc all 32 0                 # cold: a nonce of this pid and the wall clock
//   /tmp/mcpmc all 32 0 some-literal    # cold the first time that literal is used, warm after
//
// Run it WITHOUT MTL_DEBUG_LAYER: plain Metal is the oracle here.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdlib.h>
#include <string.h>

static double now_us(void) {
  return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1e3;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/// The one-minute, five-minute and fifteen-minute load averages, as the host reports them.
static void print_load(const char *when) {
  double load[3] = {0, 0, 0};
  if (getloadavg(load, 3) == 3) {
    printf("load %s: %.2f %.2f %.2f\n", when, load[0], load[1], load[2]);
  } else {
    printf("load %s: unavailable\n", when);
  }
}

/// The byte offset every arrangement but `offsets` puts each of the five attributes at, and the
/// stride every arrangement but `stride` gives the layout. 32 bytes apart is wider than the widest
/// format (16), so every attribute's bytes fit at every format the sweep gives it.
static const NSUInteger kOffsets[5] = {0, 32, 64, 96, 128};
static const NSUInteger kStride = 256;

/// The Metal buffer index the layout and every attribute read, and the index buffer's own slot,
/// both as `attribute-format-fetches` states them.
static const NSUInteger kSourceBuffer = 1;
static const NSUInteger kIndexBuffer = 2;

/// Every `MTLAttributeFormat` the `float4` attribute reads in its own numeric class: the normalized
/// integer codings, the halves, the floats, the packed small floats and BGRA. The list
/// `attribute-format-fetches` names its rows after, and the longest of the five.
static const MTLAttributeFormat kFloat4Formats[] = {
    MTLAttributeFormatUChar2Normalized,      MTLAttributeFormatUChar3Normalized,
    MTLAttributeFormatUChar4Normalized,      MTLAttributeFormatChar2Normalized,
    MTLAttributeFormatChar3Normalized,       MTLAttributeFormatChar4Normalized,
    MTLAttributeFormatUShort2Normalized,     MTLAttributeFormatUShort3Normalized,
    MTLAttributeFormatUShort4Normalized,     MTLAttributeFormatShort2Normalized,
    MTLAttributeFormatShort3Normalized,      MTLAttributeFormatShort4Normalized,
    MTLAttributeFormatHalf2,                 MTLAttributeFormatHalf3,
    MTLAttributeFormatHalf4,                 MTLAttributeFormatFloat,
    MTLAttributeFormatFloat2,                MTLAttributeFormatFloat3,
    MTLAttributeFormatFloat4,                MTLAttributeFormatInt1010102Normalized,
    MTLAttributeFormatUInt1010102Normalized, MTLAttributeFormatUChar4Normalized_BGRA,
    MTLAttributeFormatUCharNormalized,       MTLAttributeFormatCharNormalized,
    MTLAttributeFormatUShortNormalized,      MTLAttributeFormatShortNormalized,
    MTLAttributeFormatHalf,                  MTLAttributeFormatFloatRG11B10,
    MTLAttributeFormatFloatRGB9E5,
};
/// The `uint4` attribute's class: the unsigned 32-bit codings.
static const MTLAttributeFormat kUInt4Formats[] = {
    MTLAttributeFormatUInt,
    MTLAttributeFormatUInt2,
    MTLAttributeFormatUInt3,
    MTLAttributeFormatUInt4,
};
/// The `int` attribute's class: one component of a signed 32-bit coding, so one value.
static const MTLAttributeFormat kIntFormats[] = {MTLAttributeFormatInt};
/// The `half2` attribute's class.
static const MTLAttributeFormat kHalf2Formats[] = {MTLAttributeFormatHalf,
                                                   MTLAttributeFormatHalf2};
/// The `int4` attribute's class: the signed 32-bit codings in every width.
static const MTLAttributeFormat kInt4Formats[] = {
    MTLAttributeFormatInt,
    MTLAttributeFormatInt2,
    MTLAttributeFormatInt3,
    MTLAttributeFormatInt4,
};

/// Each declared attribute's own class list and its length, in attribute order.
static const MTLAttributeFormat *const kLists[5] = {kFloat4Formats, kUInt4Formats, kIntFormats,
                                                    kHalf2Formats, kInt4Formats};
static const int kListLengths[5] = {
    (int)(sizeof kFloat4Formats / sizeof kFloat4Formats[0]),
    (int)(sizeof kUInt4Formats / sizeof kUInt4Formats[0]),
    (int)(sizeof kIntFormats / sizeof kIntFormats[0]),
    (int)(sizeof kHalf2Formats / sizeof kHalf2Formats[0]),
    (int)(sizeof kInt4Formats / sizeof kInt4Formats[0]),
};

/// The source of one kernel with the fixture's five `[[stage_in]]` attributes and one with no stage
/// input at all, with `nonce` in a comment AND in a live constant so MTLCompilerService holds no
/// earlier run's compile of it, a body of `statements` fused multiply-adds, and a
/// `[[function_constant(0)]]` both kernels read.
///
/// The declarations are `kernel_stage_in.metal`'s `FetchedWide`: `float4`, `uint4`, `int`, `half2`,
/// `int4`, which is what makes each attribute's class list the one the suite's case sweeps.
static NSString *sourceWithNonce(NSString *nonce, int statements) {
  NSMutableString *s = [NSMutableString string];
  [s appendFormat:@"// nonce %@\n", nonce];
  [s appendString:@"#include <metal_stdlib>\n"
                   "using namespace metal;\n"
                   "constant float kc [[function_constant(0)]];\n"];
  // The nonce in a LIVE constant, not only the comment above it: a comment does not reach the AIR
  // and MTLCompilerService keys its compiled output on the AIR, so two sources differing only in a
  // comment read one compile across processes.
  [s appendFormat:@"constant float kn = %.1f;\n", (double)(nonce.hash % 100000) + 0.5];
  [s appendString:@""
                   "struct FetchedWide {\n"
                   "  float4 f [[attribute(0)]];\n"
                   "  uint4 u [[attribute(1)]];\n"
                   "  int i [[attribute(2)]];\n"
                   "  half2 h [[attribute(3)]];\n"
                   "  int4 v [[attribute(4)]];\n"
                   "};\n"
                   "kernel void stage_in_fetch_wide(FetchedWide in [[stage_in]],\n"
                   "                                device uint4 *out [[buffer(0)]],\n"
                   "                                uint2 gid [[thread_position_in_grid]],\n"
                   "                                uint2 grid [[threads_per_grid]]) {\n"
                   "  uint t = gid.y * grid.x + gid.x;\n"
                   "  float4 a = in.f + kn * 1e-9f;\n"];
  for (int i = 0; i < statements; i++) {
    [s appendFormat:@"  a = fma(a, float4(%d.0), sin(a.yzwx * %d.5)) * 0.5;\n", (i % 7) + 1,
                    (i % 11) + 1];
  }
  [s appendString:@""
                   "  out[4 * t] = as_type<uint4>(a);\n"
                   "  out[4 * t + 1] = in.u;\n"
                   "  out[4 * t + 2] = uint4(as_type<uint>(in.i), as_type<ushort>(in.h.x),\n"
                   "                         as_type<ushort>(in.h.y), t);\n"
                   "  out[4 * t + 3] = as_type<uint4>(in.v);\n"
                   "}\n"
                   // One attribute, nothing else changed: `narrow` sweeps this one against
                   // `formats`' five in the same process, so what the descriptor's WIDTH costs is
                   // read off two medians that met the same load.
                   "struct FetchedOne { float4 f [[attribute(0)]]; };\n"
                   "kernel void stage_in_fetch_one(FetchedOne in [[stage_in]],\n"
                   "                               device uint4 *out [[buffer(0)]],\n"
                   "                               uint t [[thread_position_in_grid]]) {\n"
                   "  out[t] = as_type<uint4>(in.f + kn * 1e-9f);\n"
                   "}\n"
                   "kernel void no_stage_in(device uint4 *out [[buffer(0)]],\n"
                   "                        uint t [[thread_position_in_grid]]) {\n"
                   "  float4 a = float4(kn * 1e-9f);\n"];
  for (int i = 0; i < statements; i++) {
    [s appendFormat:@"  a = fma(a, float4(%d.0), sin(a.yzwx * %d.5)) * 0.5;\n", (i % 7) + 1,
                    (i % 11) + 1];
  }
  [s appendString:@""
                   "  out[t] = as_type<uint4>(a);\n"
                   "}\n"
                   // The one entry point that reads the `[[function_constant]]`, and so the one
                   // `fnconst` can specialize. It is its own kernel because a function that
                   // declares a constant is UNSPECIALIZED until one is given, and Metal refuses to
                   // build a pipeline from an unspecialized function -- while every other
                   // arrangement must look its function up the way the suite's own call does,
                   // with plain `newFunctionWithName:`.
                   "kernel void stage_in_fetch_specialized(FetchedWide in [[stage_in]],\n"
                   "                                       device uint4 *out [[buffer(0)]],\n"
                   "                                       uint2 gid [[thread_position_in_grid]],\n"
                   "                                       uint2 grid [[threads_per_grid]]) {\n"
                   "  uint t = gid.y * grid.x + gid.x;\n"
                   "  out[4 * t] = as_type<uint4>(in.f + kc + kn * 1e-9f);\n"
                   "  out[4 * t + 1] = in.u;\n"
                   "  out[4 * t + 2] = uint4(as_type<uint>(in.i), as_type<ushort>(in.h.x),\n"
                   "                         as_type<ushort>(in.h.y), t);\n"
                   "  out[4 * t + 3] = as_type<uint4>(in.v);\n"
                   "}\n"];
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

/// The stage-input descriptor the cases state: five attributes at `formats` and `offsets`, all
/// reading one buffer whose layout is `stride` bytes stepped by the thread's x position at rate 1,
/// and the index buffer named whether a layout steps through it or not.
static MTLStageInputOutputDescriptor *stageInputOf(const MTLAttributeFormat *formats,
                                                   const NSUInteger *offsets, NSUInteger stride,
                                                   NSUInteger count) {
  MTLStageInputOutputDescriptor *d = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
  for (NSUInteger i = 0; i < count; i++) {
    MTLAttributeDescriptor *a = d.attributes[i];
    a.format = formats[i];
    a.offset = offsets[i];
    a.bufferIndex = kSourceBuffer;
  }
  MTLBufferLayoutDescriptor *layout = d.layouts[kSourceBuffer];
  layout.stride = stride;
  layout.stepFunction = MTLStepFunctionThreadPositionInGridX;
  layout.stepRate = 1;
  d.indexType = MTLIndexTypeUInt32;
  d.indexBufferIndex = kIndexBuffer;
  return d;
}

/// The five-attribute stage input every arrangement but `narrow` states.
static MTLStageInputOutputDescriptor *stageInput(const MTLAttributeFormat *formats,
                                                 const NSUInteger *offsets, NSUInteger stride) {
  return stageInputOf(formats, offsets, stride, 5);
}

/// The median of `us[from..to)`, skipping the entries `skip` marks, which it sorts a copy of.
static double median_of(const double *us, const BOOL *skip, int from, int to) {
  int n = 0;
  double *sorted = malloc(sizeof(double) * (size_t)(to - from > 0 ? to - from : 1));
  for (int i = from; i < to; i++) {
    if (!skip || !skip[i]) {
      sorted[n++] = us[i];
    }
  }
  if (n == 0) {
    free(sorted);
    return 0;
  }
  qsort(sorted, (size_t)n, sizeof(double), cmp_double);
  double median = sorted[n / 2];
  free(sorted);
  return median;
}

/// Report `n` creations under `name`, where the arrangement has only `distinct` descriptors and
/// creation `distinct` onward repeats one Metal has already been handed, and `refused[i]` marks a
/// creation Metal would not make (whose figure is the cost of a refusal and is left out of every
/// median). `lookup` is the `newFunctionWithName:` beside each creation, which the suite's timed
/// call holds too.
///
/// Three figures: the FIRST creation (which carries whatever the kernel's own compile costs), the
/// median of the other DISTINCT ones (Metal's marginal NEW pipeline over an AIR it has in hand),
/// and the median of the REPEATS (Metal's memo). The ratios are read inside this one process, so
/// both arms met the same load.
static void report(const char *name, const double *us, const double *lookup, const BOOL *refused,
                   int n, int distinct, const char *note) {
  printf("%-11s each us:", name);
  for (int i = 0; i < n; i++) {
    printf("%s %.0f%s", i == distinct ? " |" : "", us[i], refused[i] ? "R" : "");
  }
  printf("\n%-11s lookup us:", name);
  for (int i = 0; i < n; i++) {
    printf("%s %.0f", i == distinct ? " |" : "", lookup[i]);
  }
  printf("\n");
  if (distinct > n) {
    distinct = n;
  }
  int made = 0, denied = 0;
  for (int i = 0; i < n; i++) {
    refused[i] ? denied++ : made++;
  }
  double marginal = median_of(us, refused, 1, distinct);
  printf("%-11s first %.0f us", name, us[0]);
  if (distinct > 1) {
    printf("  marginal of %d more distinct: median %.0f us (marginal/first %.3f)", distinct - 1,
           marginal, us[0] > 0 ? marginal / us[0] : 0);
  }
  if (n > distinct) {
    double repeat = median_of(us, refused, distinct, n);
    double against = distinct > 1 ? marginal : us[0];
    printf("  repeats of %d: median %.0f us (repeat/new %.4f)", n - distinct, repeat,
           against > 0 ? repeat / against : 0);
  }
  printf("  lookup median %.0f us", median_of(lookup, NULL, 0, n));
  if (denied > 0) {
    printf("  REFUSED %d of %d (marked R, left out of the medians)", denied, n);
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

    // The nonce decides whether this run's compiles are cold. Default: the pid and the wall clock,
    // so no earlier process has this AIR. A literal given on the command line is cold on its FIRST
    // use on this machine and warm on every later one.
    NSString *stamp =
        argc > 4 ? [NSString stringWithUTF8String:argv[4]]
                 : [NSString stringWithFormat:@"%d-%llu", getpid(),
                                              clock_gettime_nsec_np(CLOCK_REALTIME)];
    printf("device %s, N %d, kernel statements %d, pid %d, nonce %s (%s)\n",
           device.name.UTF8String, n, statements, getpid(), stamp.UTF8String,
           argc > 4 ? "GIVEN: cold only on its first use on this machine" : "live: cold");
    print_load("before");

    double *us = malloc(sizeof(double) * (size_t)n);
    double *lookup = malloc(sizeof(double) * (size_t)n);
    BOOL *refused = malloc(sizeof(BOOL) * (size_t)n);
    BOOL all = strcmp(which, "all") == 0;

    // The shared library every arrangement but `distinctfn` draws its kernels from. Its compile is
    // where Metal compiles the AIR, and it is reported because it is the cost our leg does not pay
    // at creation (we translate per variant) and Metal's per-case total holds.
    double library_us = 0;
    NSString *shared_source =
        sourceWithNonce([NSString stringWithFormat:@"%@-shared", stamp], statements);
    id<MTLLibrary> shared = libraryOf(device, shared_source, &library_us);
    printf("library     newLibraryWithSource %.0f us (%lu source bytes)\n", library_us,
           (unsigned long)shared_source.length);

    MTLFunctionConstantValues *zero = [MTLFunctionConstantValues new];
    float kc = 0.25f;
    [zero setConstantValue:&kc type:MTLDataTypeFloat atIndex:0];

    // The states kept so no creation reuses a released state's place.
    NSMutableArray *kept = [NSMutableArray array];

    // One timed creation, as the suite times it: the function lookup and then the creation, both
    // recorded, with a refusal written down rather than ended on -- 29 formats on five declared
    // attributes is exactly the sweep where Metal may decline one, and the suite records that at
    // its own site and carries on.
    void (^create)(id<MTLLibrary>, NSString *, MTLFunctionConstantValues *,
                   MTLStageInputOutputDescriptor *, int) =
        ^(id<MTLLibrary> library, NSString *entry, MTLFunctionConstantValues *constants,
          MTLStageInputOutputDescriptor *stage_input, int at) {
          NSError *e = nil;
          double began = now_us();
          id<MTLFunction> function =
              constants ? [library newFunctionWithName:entry constantValues:constants error:&e]
                        : [library newFunctionWithName:entry];
          lookup[at] = now_us() - began;
          if (!function) {
            fprintf(stderr, "newFunctionWithName %s: %s\n", entry.UTF8String,
                    e.localizedDescription.UTF8String);
            exit(2);
          }
          MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
          d.computeFunction = function;
          if (stage_input) {
            d.stageInputDescriptor = stage_input;
          }
          double at_us = now_us();
          id<MTLComputePipelineState> state =
              [device newComputePipelineStateWithDescriptor:d
                                                    options:MTLPipelineOptionNone
                                                 reflection:NULL
                                                      error:&e];
          us[at] = now_us() - at_us;
          refused[at] = state == nil;
          if (state) {
            [kept addObject:state];
          } else {
            printf("%-11s creation %d refused: %s\n", "refusal", at,
                   e.localizedDescription.UTF8String);
          }
        };

    // The five attributes' formats for creation `i` of `formats`: each attribute cycling its own
    // class's list, which is `attribute-format-fetches`' own rule.
    void (^formats_at)(int, MTLAttributeFormat *) = ^(int i, MTLAttributeFormat *into) {
      for (int a = 0; a < 5; a++) {
        into[a] = kLists[a][i % kListLengths[a]];
      }
    };
    MTLAttributeFormat base_formats[5];
    formats_at(0, base_formats);

    if (all || strcmp(which, "freshdevice") == 0) {
      // ONE pipeline each on N FRESH DEVICES, which is the shape the suite measures in: its
      // timing leg builds a `MetalSide` -- and so calls `MTLCreateSystemDefaultDevice` -- once per
      // CASE, so a case that makes one such pipeline makes it on a device that has made none, and
      // a case that makes 29 makes 28 of them on a device that has.
      //
      // The device and the library are created outside the reported figure (both are printed
      // separately), so what is reported is the first compute pipeline creation on a device that
      // has made none, over an AIR this process already holds.
      double device_total = 0, library_total = 0;
      NSMutableArray *devices = [NSMutableArray array];
      for (int i = 0; i < n; i++) {
        double at = now_us();
        id<MTLDevice> own = MTLCreateSystemDefaultDevice();
        device_total += now_us() - at;
        if (!own) {
          fprintf(stderr, "freshdevice: MTLCreateSystemDefaultDevice returned nil\n");
          return 2;
        }
        [devices addObject:own];
        double one = 0;
        id<MTLLibrary> lib = libraryOf(own, shared_source, &one);
        library_total += one;
        NSError *e = nil;
        double began = now_us();
        id<MTLFunction> function = [lib newFunctionWithName:@"stage_in_fetch_wide"];
        lookup[i] = now_us() - began;
        if (!function) {
          fprintf(stderr, "freshdevice: newFunctionWithName failed\n");
          return 2;
        }
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = function;
        d.stageInputDescriptor = stageInput(base_formats, kOffsets, kStride);
        double at_us = now_us();
        id<MTLComputePipelineState> state =
            [own newComputePipelineStateWithDescriptor:d
                                              options:MTLPipelineOptionNone
                                           reflection:NULL
                                                error:&e];
        us[i] = now_us() - at_us;
        refused[i] = state == nil;
        if (state) {
          [kept addObject:state];
        } else {
          printf("freshdevice  creation %d refused: %s\n", i, e.localizedDescription.UTF8String);
        }
      }
      int distinct_devices = 1;
      for (int i = 1; i < n; i++) {
        if (devices[i] != devices[0]) {
          distinct_devices++;
        }
      }
      printf("freshdevice %d devices in %.0f us (%.0f us each), %d of %d distinct objects; their "
             "%d libraries in %.0f us (%.0f us each); neither is in the creation figures below\n",
             n, device_total, device_total / n, distinct_devices, n, n, library_total,
             library_total / n);
      // One descriptor, so creation 1 onward are REPEATS, not new pipelines: the figure that
      // matters is the first against them.
      report("freshdevice", us, lookup, refused, n, 1,
             "ONE pipeline each on a device that has made none");
      [devices removeAllObjects];
    }

    if (all || strcmp(which, "same") == 0) {
      NSUInteger before = kept.count;
      for (int i = 0; i < n; i++) {
        create(shared, @"stage_in_fetch_wide", nil, stageInput(base_formats, kOffsets, kStride), i);
      }
      int distinct_objects = 1;
      for (NSUInteger i = before + 1; i < kept.count; i++) {
        if (kept[i] != kept[before]) {
          distinct_objects++;
        }
      }
      char note[112];
      snprintf(note, sizeof note, "%d of %lu are distinct objects", distinct_objects,
               (unsigned long)(kept.count - before));
      report("same", us, lookup, refused, n, 1, note);
    }

    if (all || strcmp(which, "released") == 0) {
      // `same` again, except that each state is RELEASED before the next creation -- which is what
      // the suite's timing leg does between cases: it drains an autorelease pool around each case,
      // so the pipeline, function and library a case made are gone before the next case runs. If
      // Metal's memo is a cache of LIVE states this reads like a new creation, and if it outlives
      // the state it reads like `same`.
      for (int i = 0; i < n; i++) {
        @autoreleasepool {
          NSError *e = nil;
          double began = now_us();
          id<MTLFunction> function = [shared newFunctionWithName:@"stage_in_fetch_wide"];
          lookup[i] = now_us() - began;
          if (!function) {
            fprintf(stderr, "released: newFunctionWithName failed\n");
            return 2;
          }
          MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
          d.computeFunction = function;
          d.stageInputDescriptor = stageInput(base_formats, kOffsets, kStride);
          double at_us = now_us();
          id<MTLComputePipelineState> state =
              [device newComputePipelineStateWithDescriptor:d
                                                   options:MTLPipelineOptionNone
                                                reflection:NULL
                                                     error:&e];
          us[i] = now_us() - at_us;
          refused[i] = state == nil;
          if (!state) {
            printf("released    creation %d refused: %s\n", i,
                   e.localizedDescription.UTF8String);
          }
          // `state` and `function` die with this pool, so the next creation finds whatever Metal
          // keeps for a descriptor whose state nobody holds.
        }
      }
      report("released", us, lookup, refused, n, 1,
             "the identical descriptor, each state released before the next");
    }

    if (all || strcmp(which, "formats") == 0) {
      for (int i = 0; i < n; i++) {
        MTLAttributeFormat five[5];
        formats_at(i, five);
        create(shared, @"stage_in_fetch_wide", nil, stageInput(five, kOffsets, kStride), i);
      }
      // The tuple repeats once every attribute's list has come round, which is the least common
      // multiple of the five lengths -- 29, 4, 1, 2, 4 -- so 116.
      report("formats", us, lookup, refused, n, n < 116 ? n : 116,
             "each attribute cycling its own class's list");
    }

    if (all || strcmp(which, "onefmt") == 0) {
      for (int i = 0; i < n; i++) {
        MTLAttributeFormat five[5];
        memcpy(five, base_formats, sizeof five);
        five[0] = kFloat4Formats[i % kListLengths[0]];
        create(shared, @"stage_in_fetch_wide", nil, stageInput(five, kOffsets, kStride), i);
      }
      report("onefmt", us, lookup, refused, n, n < kListLengths[0] ? n : kListLengths[0],
             "attribute 0's format alone");
    }

    if (all || strcmp(which, "narrow") == 0) {
      for (int i = 0; i < n; i++) {
        MTLAttributeFormat one[1] = {kFloat4Formats[i % kListLengths[0]]};
        create(shared, @"stage_in_fetch_one", nil, stageInputOf(one, kOffsets, kStride, 1), i);
      }
      report("narrow", us, lookup, refused, n, n < kListLengths[0] ? n : kListLengths[0],
             "ONE attribute's format, against `formats`' five");
    }

    if (all || strcmp(which, "dynamic") == 0) {
      for (int i = 0; i < n; i++) {
        MTLAttributeFormat five[5];
        formats_at(i, five);
        create(shared, @"stage_in_fetch_wide", nil,
               stageInput(five, kOffsets, MTLBufferLayoutStrideDynamic), i);
      }
      report("dynamic", us, lookup, refused, n, n < 116 ? n : 116,
             "`formats` again under MTLBufferLayoutStrideDynamic");
    }

    if (all || strcmp(which, "offsets") == 0) {
      for (int i = 0; i < n; i++) {
        NSUInteger offsets[5];
        memcpy(offsets, kOffsets, sizeof offsets);
        // Attribute 0's widest format is 16 bytes and attribute 1 sits at 32, so an offset of
        // 16 * (i % 2) keeps every attribute's bytes inside the stride and apart from each other.
        offsets[0] = 16 * (NSUInteger)(i % 2);
        create(shared, @"stage_in_fetch_wide", nil, stageInput(base_formats, offsets, kStride), i);
      }
      report("offsets", us, lookup, refused, n, 2, "attribute 0's offset alone; 2 distinct");
    }

    if (all || strcmp(which, "stride") == 0) {
      for (int i = 0; i < n; i++) {
        create(shared, @"stage_in_fetch_wide", nil,
               stageInput(base_formats, kOffsets, kStride + 16 * (NSUInteger)i), i);
      }
      report("stride", us, lookup, refused, n, n, "the layout's stride alone");
    }

    if (all || strcmp(which, "nostagein") == 0) {
      for (int i = 0; i < n; i++) {
        create(shared, @"no_stage_in", nil, nil, i);
      }
      report("nostagein", us, lookup, refused, n, 1, "no stage input at all; one descriptor");
    }

    if (all || strcmp(which, "fnconst") == 0) {
      for (int i = 0; i < n; i++) {
        MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
        float value = (float)i + 0.5f;
        [values setConstantValue:&value type:MTLDataTypeFloat atIndex:0];
        create(shared, @"stage_in_fetch_specialized", values,
               stageInput(base_formats, kOffsets, kStride), i);
      }
      report("fnconst", us, lookup, refused, n, n, "a distinct specialized MTLFunction each");
    }

    if (all || strcmp(which, "distinctfn") == 0) {
      double library_total = 0;
      for (int i = 0; i < n; i++) {
        NSString *nonce = [NSString stringWithFormat:@"%@-distinct-%d", stamp, i];
        double one = 0;
        id<MTLLibrary> own = libraryOf(device, sourceWithNonce(nonce, statements), &one);
        library_total += one;
        create(own, @"stage_in_fetch_wide", nil, stageInput(base_formats, kOffsets, kStride), i);
      }
      printf("distinctfn  its %d libraries compiled in %.0f us (%.0f us each), which is NOT in the "
             "creation figures below\n",
             n, library_total, library_total / n);
      report("distinctfn", us, lookup, refused, n, n, "no two share a library");
    }

    printf("kept %lu states\n", (unsigned long)kept.count);
    print_load("after");
    free(us);
    free(lookup);
    free(refused);
  }
  return 0;
}
