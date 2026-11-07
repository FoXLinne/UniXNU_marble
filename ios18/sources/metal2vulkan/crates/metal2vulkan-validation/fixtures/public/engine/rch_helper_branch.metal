// Owned fixture: fragment functions that read a coverage texture, take a
// branch where the coverage is not 1 (where every covered pixel's coverage is 1, only the quad
// helpers past a primitive's edges take it), blend `half` values there with the high word of a
// product (`mulhi`, an `OpUMulExtended`, which SPIRV-Cross writes through its `optnone`
// `spvMulExtended`), and write a buffer after it. `rch_helper_branch` samples a texture at LOD 0
// before the branch; `rch_helper_branch_late` also samples at an implicit LOD after it, at the
// pixel; `rch_helper_branch_gradient` samples after it at a coordinate the branch computes;
// `rch_helper_branch_unsampled` samples nothing; `rch_helper_branch_loop` blends in a loop that
// runs at least once and as long as the coverage says.
#include <metal_stdlib>
using namespace metal;

constexpr sampler noise_sampler(filter::nearest, address::repeat);

static half4 base(float4 position, texture2d<half> noise)
{
    half4 n = noise.sample(noise_sampler, position.xy * 0.03125f, level(0.0f));
    return half4(1.0h, 0.5h, 0.25h, 1.0h) + n * 0.0078125h;
}

// `mix(old, out, a)` plus the lowest bit of the high word of `i * k`.
static half4 blend(half4 old, half4 out, float a, uint i, uint k)
{
    return mix(old, out, half(a)) + half(mulhi(i, k) & 1u);
}

fragment void rch_helper_branch(float4 position [[position]],
                                texture2d<half> noise [[texture(0)]],
                                texture2d<float> coverage [[texture(1)]],
                                device half4 *target [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    uint i = p.y * 64u + p.x;
    float c = coverage.read(p).x;
    half4 out = base(position, noise);
    if (c != 1.0f) {
        out = blend(target[i], out, c, i, as_type<uint>(c));
    }
    target[i] = out;
}

fragment void rch_helper_branch_late(float4 position [[position]],
                                     texture2d<half> noise [[texture(0)]],
                                     texture2d<float> coverage [[texture(1)]],
                                     device half4 *target [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    uint i = p.y * 64u + p.x;
    float c = coverage.read(p).x;
    half4 out = base(position, noise);
    if (c != 1.0f) {
        out = blend(target[i], out, c, i, as_type<uint>(c));
    }
    target[i] = out + noise.sample(noise_sampler, position.xy * 0.5f) * 0.0078125h;
}

fragment void rch_helper_branch_gradient(float4 position [[position]],
                                         texture2d<half> noise [[texture(0)]],
                                         texture2d<float> coverage [[texture(1)]],
                                         device half4 *target [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    uint i = p.y * 64u + p.x;
    float c = coverage.read(p).x;
    half4 out = base(position, noise);
    if (c != 1.0f) {
        out = blend(target[i], out, c, i, as_type<uint>(c));
    }
    target[i] = out + noise.sample(noise_sampler, float2(out.xy)) * 0.0078125h;
}

fragment void rch_helper_branch_unsampled(float4 position [[position]],
                                          texture2d<float> coverage [[texture(1)]],
                                          device half4 *target [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    uint i = p.y * 64u + p.x;
    float c = coverage.read(p).x;
    half4 out = half4(1.0h, 0.5h, 0.25h, 1.0h);
    if (c != 1.0f) {
        out = blend(target[i], out, c, i, as_type<uint>(c));
    }
    target[i] = out;
}

fragment void rch_helper_branch_loop(float4 position [[position]],
                                     texture2d<half> noise [[texture(0)]],
                                     texture2d<float> coverage [[texture(1)]],
                                     device half4 *target [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    uint i = p.y * 64u + p.x;
    float c = coverage.read(p).x;
    half4 out = base(position, noise);
    uint k = 0u;
    do {
        out = blend(target[i], out, c * float(k + 1u) * 0.25f, i, k + 1u);
        k++;
    } while (k < uint(c * 4.0f));
    target[i] = out;
}
