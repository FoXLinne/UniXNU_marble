// Owned fixture: fragment functions that read a coverage texture, sample a
// texture at LOD 0, take a branch where the coverage is not 1 (where every covered pixel's
// coverage is 1, only the quad helpers past a primitive's edges take it), take the high words of
// products there (`mulhi`, an `OpUMulExtended`, which SPIRV-Cross writes through its `optnone`
// `spvMulExtended`) beside a tangled instruction, and write a texture after the branch. In the
// `after` functions the tangled instruction reads the products: an implicit-LOD sample at the
// coordinate they make, `dfdx` and `quad_sum` of them, and the LOD a sample there would take, in
// the arm; an implicit-LOD sample after the branch's merge at the coordinate the arm makes
// (`rch_tangle_merge_after`); and one after a loop that only the helpers run more than once,
// making a coordinate each time (`rch_tangle_loop_after`). In the `before` functions it reads
// nothing the arm makes and comes first: `quad_sum` and an implicit-LOD sample of the pixel's own.
// In `rch_tangle_unread` the sample comes after the products but reads none of them.
// `rch_tangle_vertex` draws the triangle whose three corners `buffer(0)` holds, in clip space.
#include <metal_stdlib>
using namespace metal;

constexpr sampler tangle_sampler(filter::nearest, address::repeat);

vertex float4 rch_tangle_vertex(uint vid [[vertex_id]], constant float2 *corners [[buffer(0)]])
{
    return float4(corners[vid], 0.0f, 1.0f);
}

static float4 base(float4 position, texture2d<float> noise)
{
    float4 n = noise.sample(tangle_sampler, position.xy * 0.03125f, level(0.0f));
    return float4(1.0f, 0.5f, 0.25f, 1.0f) + n * 0.0078125f;
}

// The high words of the products of `v`'s bits and `c`'s, scaled into [0, 1).
static float4 products(float4 v, float c)
{
    return float4(mulhi(as_type<uint4>(v), uint4(as_type<uint>(c)))) * 0x1p-32f;
}

fragment void rch_tangle_sample_after(float4 position [[position]],
                                      texture2d<float> noise [[texture(0)]],
                                      texture2d<float> coverage [[texture(1)]],
                                      texture2d<float, access::write> dst [[texture(2)]],
                                      device const float4 *src [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    float c = coverage.read(p).x;
    float4 out = base(position, noise);
    if (c != 1.0f) {
        float4 m = products(src[p.y * 64u + p.x], c);
        out = noise.sample(tangle_sampler, m.xy);
    }
    dst.write(out, p);
}

fragment void rch_tangle_dfdx_after(float4 position [[position]],
                                    texture2d<float> noise [[texture(0)]],
                                    texture2d<float> coverage [[texture(1)]],
                                    texture2d<float, access::write> dst [[texture(2)]],
                                    device const float4 *src [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    float c = coverage.read(p).x;
    float4 out = base(position, noise);
    if (c != 1.0f) {
        float4 m = products(src[p.y * 64u + p.x], c);
        out = dfdx(m);
    }
    dst.write(out, p);
}

fragment void rch_tangle_quad_after(float4 position [[position]],
                                    texture2d<float> noise [[texture(0)]],
                                    texture2d<float> coverage [[texture(1)]],
                                    texture2d<float, access::write> dst [[texture(2)]],
                                    device const float4 *src [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    float c = coverage.read(p).x;
    float4 out = base(position, noise);
    if (c != 1.0f) {
        float4 m = products(src[p.y * 64u + p.x], c);
        out = float4(quad_sum(m.x));
    }
    dst.write(out, p);
}

fragment void rch_tangle_lod_after(float4 position [[position]],
                                   texture2d<float> noise [[texture(0)]],
                                   texture2d<float> coverage [[texture(1)]],
                                   texture2d<float, access::write> dst [[texture(2)]],
                                   device const float4 *src [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    float c = coverage.read(p).x;
    float4 out = base(position, noise);
    if (c != 1.0f) {
        float4 m = products(src[p.y * 64u + p.x], c);
        out = float4(noise.calculate_unclamped_lod(tangle_sampler, m.xy));
    }
    dst.write(out, p);
}

fragment void rch_tangle_quad_before(float4 position [[position]],
                                     texture2d<float> noise [[texture(0)]],
                                     texture2d<float> coverage [[texture(1)]],
                                     texture2d<float, access::write> dst [[texture(2)]],
                                     device const float4 *src [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    float c = coverage.read(p).x;
    float4 out = base(position, noise);
    if (c != 1.0f) {
        float t = quad_sum(position.x);
        float4 m = products(src[p.y * 64u + p.x], c);
        out = m + t * 0.0078125f;
    }
    dst.write(out, p);
}

fragment void rch_tangle_sample_before(float4 position [[position]],
                                       texture2d<float> noise [[texture(0)]],
                                       texture2d<float> coverage [[texture(1)]],
                                       texture2d<float, access::write> dst [[texture(2)]],
                                       device const float4 *src [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    float c = coverage.read(p).x;
    float4 out = base(position, noise);
    if (c != 1.0f) {
        float4 t = noise.sample(tangle_sampler, position.xy * 0.5f);
        float4 m = products(src[p.y * 64u + p.x], c);
        out = m + t * 0.0078125f;
    }
    dst.write(out, p);
}

fragment void rch_tangle_unread(float4 position [[position]],
                                texture2d<float> noise [[texture(0)]],
                                texture2d<float> coverage [[texture(1)]],
                                texture2d<float, access::write> dst [[texture(2)]],
                                device const float4 *src [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    float c = coverage.read(p).x;
    float4 out = base(position, noise);
    if (c != 1.0f) {
        float4 m = products(src[p.y * 64u + p.x], c);
        float4 t = noise.sample(tangle_sampler, position.xy * 0.5f);
        out = m + t * 0.0078125f;
    }
    dst.write(out, p);
}

fragment void rch_tangle_merge_after(float4 position [[position]],
                                     texture2d<float> noise [[texture(0)]],
                                     texture2d<float> coverage [[texture(1)]],
                                     texture2d<float, access::write> dst [[texture(2)]],
                                     device const float4 *src [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    float c = coverage.read(p).x;
    float4 out = base(position, noise);
    float2 at = position.xy * 0.5f;
    if (c != 1.0f) {
        float4 m = products(src[p.y * 64u + p.x], c);
        at = m.xy;
    }
    float4 t = noise.sample(tangle_sampler, at);
    dst.write(out * (t.w + 1.0f), p);
}

fragment void rch_tangle_loop_after(float4 position [[position]],
                                    texture2d<float> noise [[texture(0)]],
                                    texture2d<float> coverage [[texture(1)]],
                                    texture2d<float, access::write> dst [[texture(2)]],
                                    device const float4 *src [[buffer(0)]])
{
    uint2 p = uint2(position.xy);
    float c = coverage.read(p).x;
    float4 out = base(position, noise);
    float2 at = position.xy * 0.5f;
    uint k = 0u;
    do {
        float4 m = products(src[p.y * 64u + p.x + k], c);
        at += m.xy;
        k++;
    } while (k < uint((1.0f - c) * 4.0f));
    float4 t = noise.sample(tangle_sampler, at);
    dst.write(out * (t.w + 1.0f), p);
}
