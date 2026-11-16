// Owned fixture: 32-bit integer vertex formats read into integer-declared attributes.
//
// A 32-bit integer vertex format reaches the shader as the integer it holds rather than converted
// to float, so it needs an integer-declared attribute: `dsx_int_attributes`' `float4` and `half4`
// attributes cannot read one at all. These two are that declaration.
// - `wide_int_attribute_to_int4`, `wide_uint_attribute_to_uint4`: a rasterization-disabled point
//   per vertex; vertex v writes the int4 (uint4) attribute it fetched to out[v], unconverted, so
//   every bit of the fetch is in the answer and a code the widest float could not hold exactly
//   still compares.
// Loop-free, no texture, no rasterization.
#include <metal_stdlib>
using namespace metal;

struct Int4Attribute {
    int4 a [[attribute(0)]];
};

struct UInt4Attribute {
    uint4 a [[attribute(0)]];
};

vertex void wide_int_attribute_to_int4(Int4Attribute in [[stage_in]],
                                       device int4 *out [[buffer(0)]],
                                       uint vid [[vertex_id]]) {
    out[vid] = in.a;
}

vertex void wide_uint_attribute_to_uint4(UInt4Attribute in [[stage_in]],
                                         device uint4 *out [[buffer(0)]],
                                         uint vid [[vertex_id]]) {
    out[vid] = in.a;
}
