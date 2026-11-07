// metal-std: macos-metal2.1
// metal-macos-min: 10.14
// Owned fixture: the three constructs Apple's AIR 2.1 emitter writes in a shape its AIR 2.2 and
// later emitters do not. The deployment target above, not the language standard, is what selects
// the AIR version: at 10.14 the compiler writes `air64_v21` and `!air.version = !{i32 2, i32 1,
// i32 0}`, at 10.15 `air64_v22`, and so on to `air64_v28` at 26.0. The three shapes are:
//
//  - the function-constant initializer global in no section, where AIR 2.2 and later place it in
//    `air.fc_initializer` (`flip`, read by every entry here);
//  - the interpolation qualifier of a vertex output restated on `air.vertex_output`, and restated
//    partly: `uv`'s `[[centroid_perspective]]` writes `air.centroid` with no `air.perspective`,
//    where the fragment input receiving it writes both. AIR 2.2 and later write neither key on the
//    output;
//  - a sample, gather, or read whose declaration states no residency status: the result is the
//    texel alone rather than `{T, i8}`, and a sample carries no min-LOD-clamp operand. AIR 2.2
//    added both in one step. A read is also in the operand form without the sampler and offset
//    positions, which AIR 2.8 added.
//
// `a21_mip` is the shape the reporter's mipmap generation has: level queries and four reads of a
// level-0 quad written to a storage texture. `a21_blit_fs` samples and gathers, and reads a texel
// whose alpha nothing observes (lane 0 alone, taken by an `extractelement`); `a21_alpha_fs` is
// the same fragment function whose read reaches an operation on all four lanes. Where the declaration states no residency status the read's own result
// is the texel, so `lower::texture::alpha` follows the lanes from the call itself rather than
// through an `extractvalue 0`: on a profile whose reads the shader checks, the first needs no
// `OutOfBoundsAlpha` specialization and the second does.
//
// Each entry reads `flip` only under `is_function_constant_defined`, so every entry compiles with
// no function constants supplied, as the support inventory compiles them. The declaration must
// still decode for any of this to be reached: a module whose `air.function_constants` record or
// whose vertex output's interpolation keys are refused has no ABI, so the inventory would hold one
// row with entry `-` instead of these three, and a texture declaration that is not matched makes
// them `InvalidInput{Intrinsic(SignatureMismatch)}` instead of `ok`. Loop-free.
#include <metal_stdlib>
using namespace metal;

constant bool flip [[function_constant(0)]];
constant bool flip_given = is_function_constant_defined(flip);
// `flip` where a value was supplied, and false where none was: the constant's own `undef` value
// never reaches an operation, so every entry compiles with no function constants.
static inline bool flipped() { return flip_given && flip; }

struct a21_out {
    float4 pos [[position]];
    float2 uv [[centroid_perspective]];
    float fade [[flat]];
};

vertex a21_out a21_blit_vs(uint vid [[vertex_id]]) {
    a21_out o;
    float2 corner = float2(float(vid & 1u), float((vid >> 1) & 1u));
    o.pos = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    o.uv = flipped() ? float2(corner.x, 1.0 - corner.y) : corner;
    o.fade = float(vid);
    return o;
}

fragment float4 a21_blit_fs(a21_out in [[stage_in]],
                            texture2d<float> src [[texture(0)]],
                            sampler s [[sampler(0)]],
                            texture2d<float, access::read> raw [[texture(1)]]) {
    float4 c = src.sample(s, in.uv);
    c += src.sample(s, in.uv, level(in.fade));
    c += src.gather(s, in.uv, int2(0), component::y);
    // The alpha of this read is never observed: lane 0 alone reaches the result, and a shuffle
    // or an `fadd` of the whole texel would make the optimizer feed lane 3 to an operation.
    c += raw.read(uint2(in.pos.xy)).x;
    return flipped() ? c : c * in.fade;
}

// The same reads as `a21_blit_fs` with the whole texel, so its alpha is observed.
fragment float4 a21_alpha_fs(a21_out in [[stage_in]],
                             texture2d<float> src [[texture(0)]],
                             sampler s [[sampler(0)]],
                             texture2d<float, access::read> raw [[texture(1)]]) {
    float4 c = src.sample(s, in.uv);
    c += src.gather(s, in.uv, int2(0), component::y);
    c += raw.read(uint2(in.pos.xy));
    return flipped() ? c : c * in.fade;
}

kernel void a21_mip(texture2d<float, access::read> src [[texture(0)]],
                    texture2d<float, access::write> dst [[texture(1)]],
                    device uint *sizes [[buffer(0)]],
                    uint2 gid [[thread_position_in_grid]]) {
    sizes[0] = src.get_width();
    sizes[1] = src.get_height();
    sizes[2] = src.get_num_mip_levels();
    uint2 q = gid * 2;
    float4 sum = src.read(q) + src.read(q + uint2(1, 0)) + src.read(q + uint2(0, 1))
               + src.read(q + uint2(1, 1));
    dst.write(flipped() ? sum * 0.25 : sum, gid);
}
