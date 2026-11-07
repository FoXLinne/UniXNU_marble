// metal-std: macos-metal2.2
// metal-macos-min: 10.15
// Owned fixture: the lower edge of the residency-status form, the step `air_2_1_front_end` is the
// upper edge of no-status. At 10.15 the compiler writes `air64_v22` and `!air.version = !{i32 2,
// i32 2, i32 0}`, the first version where
//
//  - a sample, gather, or read states a residency status: its result is `{T, i8}`, the texel and
//    Metal's residency byte, where AIR 2.1 returns the texel alone;
//  - a sample carries a min-LOD-clamp operand, the `float` before the access qualifier. AIR 2.2
//    added the status and the clamp in one step, so a declaration states both or neither and a
//    mixture no emitter writes stays a mismatch.
//
// It is still before three later steps, which this fixture also pins the absence of: a buffer
// states no `air.address_space` (AIR 2.5 adds it), a function-constant initializer global sits in
// no section (AIR 2.5 places it in `air.fc_initializer`), and a read states no sampler or offset
// position (AIR 2.8 adds both). The vertex output carries no interpolation qualifier -- AIR 2.2 is
// the first version to drop it, which `air_2_1_front_end` carries.
//
// Every entry reads `tint` only under `is_function_constant_defined`, so each compiles with no
// function constants supplied. Loop-free.
#include <metal_stdlib>
using namespace metal;

constant float tint [[function_constant(0)]];
constant bool tint_given = is_function_constant_defined(tint);

struct a22_out {
    float4 pos [[position]];
    float2 uv [[centroid_perspective]];
};

struct a22_args {
    float4 scale;
};

vertex a22_out a22_vs(uint vid [[vertex_id]], constant a22_args &args [[buffer(0)]]) {
    a22_out o;
    float2 corner = float2(float(vid & 1u), float((vid >> 1) & 1u));
    o.pos = float4(corner * 2.0 - 1.0, 0.0, 1.0) * args.scale;
    o.uv = corner;
    return o;
}

fragment float4 a22_fs(a22_out in [[stage_in]],
                       texture2d<float> src [[texture(0)]],
                       sampler s [[sampler(0)]],
                       depth2d<float> shadow [[texture(1)]],
                       sampler cmp [[sampler(1)]],
                       texture2d<float, access::read> raw [[texture(2)]]) {
    float4 c = src.sample(s, in.uv);
    c += src.sample(s, in.uv, level(1.0));
    c += src.gather(s, in.uv, int2(0), component::y);
    c += shadow.sample_compare(cmp, in.uv, 0.5);
    c += raw.read(uint2(in.pos.xy));
    return tint_given ? c * tint : c;
}

kernel void a22_reduce(texture2d<float, access::read> src [[texture(0)]],
                       device float *out [[buffer(0)]],
                       constant a22_args &args [[buffer(1)]],
                       uint2 gid [[thread_position_in_grid]]) {
    float4 texel = src.read(gid) * args.scale;
    out[gid.x] = tint_given ? texel.x * tint : texel.x;
}
