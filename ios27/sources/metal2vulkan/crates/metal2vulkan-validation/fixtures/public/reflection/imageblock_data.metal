// Owned fixture: a fragment function that takes and returns explicit imageblock data, whose
// render pipeline reflection Metal answers with an MTLBindingTypeImageblockData binding, beside a
// vertex function and a buffer each. Written for this repository; no third-party source.
// Loop-free.
//
// `[[imageblock_data]]` on a fragment function's argument and on a member of its return struct is
// the only shape that reaches MTLBindingTypeImageblockData: a tile function's imageblock, implicit
// or explicit, and a compute kernel's are both MTLBindingTypeImageblock instead. Every member of
// an explicit-layout imageblock needs a `raster_order_group`, which the compiler requires.
//
// The members are plain scalars and vectors. MSL also allows an imageblock member to be a packed
// pixel type (`r8unorm<half>` and the rest, which MTLDataType names), and none is stated here
// because none is observable: Metal's imageblock binding conforms to no public MTLBinding
// subprotocol, so no public getter reaches a struct type for it (measured on an M3 Max under
// Metal 27.0.1; `cases/tile_pipeline_reflection.rs` records the measurement). A member nothing can
// read would be a claim the reflection cannot answer.
#include <metal_stdlib>
using namespace metal;

struct ReflectionImageblock {
    half weight [[raster_order_group(0)]];
    half4 tint [[raster_order_group(0)]];
};

struct ReflectionImageblockOut {
    half4 colour [[color(0)]];
    ReflectionImageblock block [[imageblock_data]];
};

struct ReflectionImageblockVertex {
    float4 position [[position]];
};

vertex ReflectionImageblockVertex reflection_imageblock_vertex(
    uint vid [[vertex_id]], constant float2 *corners [[buffer(0)]]) {
    ReflectionImageblockVertex out;
    out.position = float4(corners[vid], 0.0, 1.0);
    return out;
}

fragment ReflectionImageblockOut reflection_imageblock_fragment(
    ReflectionImageblock in [[imageblock_data]], constant half &scale [[buffer(1)]]) {
    ReflectionImageblockOut out;
    out.colour = in.tint * in.weight * scale;
    out.block = in;
    out.block.weight = in.weight * scale;
    return out;
}
