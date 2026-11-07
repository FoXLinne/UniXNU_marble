// metal-std: macos-metal2.4
// metal-macos-min: 12.0
// Owned fixture: the upper edge of two absences that AIR 2.5 ends. At 12.0 the compiler writes
// `air64_v24` and `!air.version = !{i32 2, i32 4, i32 0}`, the last version where
//
//  - a buffer states no `air.address_space`. A top-level buffer's parameter type carries the space
//    instead, but an argument buffer's member has no parameter, and the member's own pointer field
//    is reachable only through the holding struct's type name, which is no structural fact: such a
//    member decodes to device space, which Metal cannot tell from `constant` in an argument buffer
//    (both are `setBuffer:offset:atIndex:`) and which claims no uniform-block containment;
//  - a function-constant initializer global sits in no section, where AIR 2.5 and later place it
//    in `air.fc_initializer`.
//
// The argument buffer holds a `device` member and a `constant` member, so both arms of the member
// decode are exercised, plus a texture and a sampler, whose member nodes are unchanged by the
// step. `mode` is read only under `is_function_constant_defined`, so every entry compiles with no
// function constants supplied. Loop-free.
#include <metal_stdlib>
using namespace metal;

constant int mode [[function_constant(0)]];
constant bool mode_given = is_function_constant_defined(mode);

struct a24_bundle {
    texture2d<float> tex [[id(0)]];
    sampler smp [[id(1)]];
    device float4 *writable [[id(2)]];
    constant float4 *readable [[id(3)]];
    uint count [[id(4)]];
};

kernel void a24_bundle_read(constant a24_bundle &bundle [[buffer(0)]],
                            device float4 *out [[buffer(1)]],
                            uint i [[thread_position_in_grid]]) {
    float4 c = bundle.tex.sample(bundle.smp, float2(0.5));
    c += bundle.readable[0] + bundle.writable[i];
    bundle.writable[i] = c;
    out[i] = mode_given ? c * float(mode) : c * float(bundle.count);
}

struct a24_out {
    float4 pos [[position]];
    float2 uv;
};

vertex a24_out a24_vs(uint vid [[vertex_id]], constant a24_bundle &bundle [[buffer(0)]]) {
    a24_out o;
    float2 corner = float2(float(vid & 1u), float((vid >> 1) & 1u));
    o.pos = float4(corner * 2.0 - 1.0, 0.0, 1.0) * bundle.readable[0];
    o.uv = corner;
    return o;
}

fragment float4 a24_fs(a24_out in [[stage_in]], constant a24_bundle &bundle [[buffer(0)]]) {
    float4 c = bundle.tex.sample(bundle.smp, in.uv);
    return mode_given ? c * float(mode) : c;
}
