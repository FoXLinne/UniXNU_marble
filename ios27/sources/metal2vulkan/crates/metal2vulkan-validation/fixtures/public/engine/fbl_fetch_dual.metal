// Owned fixture: framebuffer fetch of an attachment blended with the second source. Each fragment
// function reads color attachment 0 ([[color(0)]]) and writes it through its first output and its
// second (index 1), under the compiler's default fast math. Metal makes and draws such a pipeline:
// the second output is the blend's second source. On a device that orders fetch through the pixel
// interlock alone the write goes to a storage image, which no Vulkan blend applies to, so the
// function computes the blend itself, reading the second source from its second output.
//
// - fbd_dual_half: `half4` outputs, each loaded from its own buffer at the fragment's texel (the
//   texel after the target's last where the fetched alpha is zero).
// - fbd_dual_float: the same as `float4` outputs, reading the attachment as `float4`.
//
// fbd_vertex draws one fullscreen triangle. Loop-free.
// metal-macos-min: 27.0
#include <metal_stdlib>
using namespace metal;

struct FbdVertex {
    float4 position [[position]];
};

vertex FbdVertex fbd_vertex(uint vid [[vertex_id]])
{
    float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
    FbdVertex o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    return o;
}

struct FbdHalf {
    half4 color [[color(0), index(0)]];
    half4 second [[color(0), index(1)]];
};

struct FbdFloat {
    float4 color [[color(0), index(0)]];
    float4 second [[color(0), index(1)]];
};

fragment FbdHalf fbd_dual_half(FbdVertex in [[stage_in]], half4 dst [[color(0)]],
                               const device half4 *src [[buffer(0)]],
                               const device half4 *second [[buffer(1)]])
{
    uint i = uint(in.position.y) * 64u + uint(in.position.x);
    uint j = dst.a == 0.0h ? i + 4096u : i;
    FbdHalf o;
    o.color = src[j];
    o.second = second[j];
    return o;
}

fragment FbdFloat fbd_dual_float(FbdVertex in [[stage_in]], float4 dst [[color(0)]],
                                 const device float4 *src [[buffer(0)]],
                                 const device float4 *second [[buffer(1)]])
{
    uint i = uint(in.position.y) * 64u + uint(in.position.x);
    uint j = dst.a == 0.0f ? i + 4096u : i;
    FbdFloat o;
    o.color = src[j];
    o.second = second[j];
    return o;
}
