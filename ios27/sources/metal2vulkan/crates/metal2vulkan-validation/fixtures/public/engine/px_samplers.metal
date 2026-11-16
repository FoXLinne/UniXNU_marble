// Owned fixture (written for this repository): 2D samples of a `texture2d<float>` through
// pixel-coordinate samplers, our own MSL. One thread per coordinate, loop-free.
//
// Three ways of reaching a sampler, which is what the pixel-coordinate question is about: the
// sampler an argument buffer holds beside its texture, and a constexpr `coord::pixel` sampler of
// each filter and of each address mode MSL can state. A texture slot's runtime sampler needs no
// kernel of its own here: `xr_samples`' `xr_sample_runtime` is that shape already.
//
// `address::clamp_to_zero` and `address::clamp_to_border` are one addressing mode of Metal's under
// two border colours, so the two are spelled apart by the colour.
#include <metal_stdlib>
using namespace metal;

// The texture and the sampler an argument buffer holds, bound at `[[buffer(2)]]`.
struct PixelArguments {
    texture2d<float> t [[id(0)]];
    sampler s [[id(1)]];
};

// The sample of the argument buffer's texture, through the sampler beside it, at `uv[g]`.
kernel void px_sample_argument(constant PixelArguments &a [[buffer(2)]],
                               device const float2 *uv [[buffer(0)]],
                               device float4 *out [[buffer(1)]],
                               uint g [[thread_position_in_grid]])
{
    out[g] = a.t.sample(a.s, uv[g]);
}

constexpr sampler px_pixel_nearest_edge(filter::nearest, address::clamp_to_edge, coord::pixel);
constexpr sampler px_pixel_nearest_zero(filter::nearest, address::clamp_to_zero, coord::pixel);
constexpr sampler px_pixel_nearest_border(filter::nearest, address::clamp_to_border,
                                          border_color::opaque_white, coord::pixel);
constexpr sampler px_pixel_linear_edge(filter::linear, address::clamp_to_edge, coord::pixel);
constexpr sampler px_pixel_linear_zero(filter::linear, address::clamp_to_zero, coord::pixel);
constexpr sampler px_pixel_linear_border(filter::linear, address::clamp_to_border,
                                         border_color::opaque_white, coord::pixel);

// The sample of texture slot 0 through the constexpr pixel sampler the name says, at `uv[g]`.
kernel void px_const_nearest_edge(texture2d<float> t [[texture(0)]],
                                  device const float2 *uv [[buffer(0)]],
                                  device float4 *out [[buffer(1)]],
                                  uint g [[thread_position_in_grid]])
{
    out[g] = t.sample(px_pixel_nearest_edge, uv[g]);
}

kernel void px_const_nearest_zero(texture2d<float> t [[texture(0)]],
                                  device const float2 *uv [[buffer(0)]],
                                  device float4 *out [[buffer(1)]],
                                  uint g [[thread_position_in_grid]])
{
    out[g] = t.sample(px_pixel_nearest_zero, uv[g]);
}

kernel void px_const_nearest_border(texture2d<float> t [[texture(0)]],
                                    device const float2 *uv [[buffer(0)]],
                                    device float4 *out [[buffer(1)]],
                                    uint g [[thread_position_in_grid]])
{
    out[g] = t.sample(px_pixel_nearest_border, uv[g]);
}

kernel void px_const_linear_edge(texture2d<float> t [[texture(0)]],
                                 device const float2 *uv [[buffer(0)]],
                                 device float4 *out [[buffer(1)]],
                                 uint g [[thread_position_in_grid]])
{
    out[g] = t.sample(px_pixel_linear_edge, uv[g]);
}

kernel void px_const_linear_zero(texture2d<float> t [[texture(0)]],
                                 device const float2 *uv [[buffer(0)]],
                                 device float4 *out [[buffer(1)]],
                                 uint g [[thread_position_in_grid]])
{
    out[g] = t.sample(px_pixel_linear_zero, uv[g]);
}

kernel void px_const_linear_border(texture2d<float> t [[texture(0)]],
                                   device const float2 *uv [[buffer(0)]],
                                   device float4 *out [[buffer(1)]],
                                   uint g [[thread_position_in_grid]])
{
    out[g] = t.sample(px_pixel_linear_border, uv[g]);
}
