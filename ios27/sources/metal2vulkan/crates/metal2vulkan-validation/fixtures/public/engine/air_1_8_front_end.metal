// metal-std: macos-metal1.1
// metal-macos-min: 10.11
// Owned fixture: the shapes Apple's oldest AIR this toolchain can emit writes differently from
// AIR 2.0 and later. At 10.11 the compiler writes `air64_v18` and `!air.version = !{i32 1, i32 8,
// i32 0}`, the floor for a macOS target (10.9 and 10.10 are clamped to it). The shapes are:
//
//  - `air.render_target` with one operand: the attachment alone. AIR 1.11 and later follow it with
//    the dual-source index, there being an `[[index(n)]]` from then on to state; the operand after
//    the attachment tells the forms apart, an index being an integer and every attribute key a
//    string;
//  - a texture operation with no trailing access qualifier: `air.read_texture_2d.v4f32(ptr
//    addrspace(1), <2 x i32>, i32)` and `air.write_texture_2d.v4f32(ptr addrspace(1), <2 x i32>,
//    <4 x float>, i32)`, where AIR 2.0 adds a trailing `i32` to each. The qualifier is then
//    `None` and the texture's own ABI binding states the access;
//  - a memory atomic with no trailing volatile flag: `air.atomic.global.add.u.i32(ptr
//    addrspace(1), i32, i32, i32)`, where AIR 2.0 adds the `i1`. The flag orders nothing (an
//    atomic is never elided or merged), so the operation is the same.
//
// No sampling here: before AIR 2.0 a `constexpr sampler` is no `@__air_sampler_state` global but
// the state word cast inline (`inttoptr (i64 ... to ptr addrspace(2))`), in a packing that is not
// AIR 2.0's, and that word's layout is not pinned, so such a call is refused rather than guessed.
// `air_1_11_front_end` is the same surface one emitter step later. Loop-free.
#include <metal_stdlib>
using namespace metal;

struct a18_out {
    float4 pos [[position]];
    float2 uv;
};

vertex a18_out a18_vs(uint vid [[vertex_id]]) {
    a18_out o;
    float2 corner = float2(float(vid & 1u), float((vid >> 1) & 1u));
    o.pos = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    o.uv = corner;
    return o;
}

// One render target, so `air.render_target` states the attachment and nothing else.
fragment float4 a18_fs(a18_out in [[stage_in]]) {
    return float4(in.uv, in.pos.z, 1.0);
}

kernel void a18_blit(texture2d<float, access::read> src [[texture(0)]],
                     texture2d<float, access::write> dst [[texture(1)]],
                     depth2d<float, access::read> depth [[texture(2)]],
                     device atomic_uint *counter [[buffer(0)]],
                     device uint *sizes [[buffer(1)]],
                     threadgroup float *scratch [[threadgroup(0)]],
                     uint2 gid [[thread_position_in_grid]],
                     uint tid [[thread_index_in_threadgroup]]) {
    sizes[0] = src.get_width();
    sizes[1] = src.get_height();
    scratch[tid] = depth.read(gid);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float4 texel = src.read(gid) + scratch[tid];
    dst.write(texel, gid);
    atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
    atomic_fetch_max_explicit(counter, uint(texel.x), memory_order_relaxed);
}
