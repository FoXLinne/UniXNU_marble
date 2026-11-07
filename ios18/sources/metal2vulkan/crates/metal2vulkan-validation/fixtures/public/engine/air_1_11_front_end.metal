// metal-std: macos-metal1.2
// metal-macos-min: 10.12
// Owned fixture: one emitter step after `air_1_8_front_end`. At 10.12 the compiler writes
// `air64_v111` and `!air.version = !{i32 1, i32 11, i32 0}`. It keeps AIR 1.x's texture and atomic
// operand forms -- no trailing access qualifier, no trailing volatile flag -- and changes two
// other shapes:
//
//  - `air.render_target` gains its second operand, the dual-source index, which every later
//    emitter also writes: `!{!"air.render_target", i32 0, i32 0, ...}`;
//  - a function constant is an `internal` global with no `MTL_FC_INIT_<n>_<mangled>` suffix and no
//    `air.fc_initializer` section (the section arrives at AIR 2.5), and a constant derived from
//    another -- an `is_function_constant_defined` result, or a ternary over it -- is computed by a
//    module constructor in `@llvm.global_ctors` rather than folded into the entries.
//
// Both derived shapes are here (`given`, `scaled`) so the constructor is emitted, and every entry
// reads the constant only under `is_function_constant_defined`, so each compiles with no function
// constants supplied. Loop-free.
#include <metal_stdlib>
using namespace metal;

constant float gain [[function_constant(0)]];
constant bool given = is_function_constant_defined(gain);
constant float scaled = given ? gain : 2.0f;

struct a111_out {
    float4 pos [[position]];
    float2 uv;
};

vertex a111_out a111_vs(uint vid [[vertex_id]]) {
    a111_out o;
    float2 corner = float2(float(vid & 1u), float((vid >> 1) & 1u));
    o.pos = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    o.uv = corner * scaled;
    return o;
}

fragment float4 a111_fs(a111_out in [[stage_in]]) {
    return float4(in.uv, scaled, 1.0);
}

kernel void a111_blit(texture2d<float, access::read> src [[texture(0)]],
                      texture2d<float, access::write> dst [[texture(1)]],
                      texture2d_ms<float> ms [[texture(2)]],
                      device atomic_uint *counter [[buffer(0)]],
                      device uint *sizes [[buffer(1)]],
                      uint2 gid [[thread_position_in_grid]]) {
    sizes[0] = src.get_num_mip_levels();
    sizes[1] = ms.get_num_samples();
    float4 texel = src.read(gid) * scaled + ms.read(gid, 0);
    dst.write(texel, gid);
    uint expected = 1u;
    atomic_compare_exchange_weak_explicit(counter, &expected, 2u, memory_order_relaxed,
                                          memory_order_relaxed);
    atomic_fetch_and_explicit(counter, expected, memory_order_relaxed);
}
