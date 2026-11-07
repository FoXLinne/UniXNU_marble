// Owned fixture (written for this repository): kernel writes and reads of one slice of a
// `texture2d_array` of Apple's two-plane extended-range format (`RGB10A8_2P_XR10` or its sRGB
// twin) as `float`, one thread per texel of the slice, the grid's rows `w` texels apart. Loop-free.
#include <metal_stdlib>
using namespace metal;

struct Slice {
    uint w;
    uint slice;
};

// Writes `in[g.y * w + g.x]` to texel `g` of slice `s.slice`.
kernel void xr2pa_write_float(texture2d_array<float, access::write> t [[texture(0)]],
                              device const float4 *in [[buffer(0)]], constant Slice &s [[buffer(1)]],
                              uint2 g [[thread_position_in_grid]])
{
    t.write(in[g.y * s.w + g.x], g, s.slice);
}

// Reads texel `g` of slice `s.slice` into `out[g.y * w + g.x]`.
kernel void xr2pa_read_float(texture2d_array<float, access::read> t [[texture(0)]],
                             device float4 *out [[buffer(0)]], constant Slice &s [[buffer(1)]],
                             uint2 g [[thread_position_in_grid]])
{
    out[g.y * s.w + g.x] = t.read(g, s.slice);
}
