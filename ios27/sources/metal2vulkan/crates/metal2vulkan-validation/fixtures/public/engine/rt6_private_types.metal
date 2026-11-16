// Owned fixture: a vertex function that places vertex `vid` at the clip-space position buffer 0
// holds at `min(vid, 65535)`, moved by buffer 1's offset of its instance, and writes its vertex ID
// as smooth values and, flat, its vertex ID, base vertex, instance ID and base instance; and
// fragment functions that return them with the barycentric weights, the primitive ID and the
// front-facing bit, with some of them, or with neither, so a lane compares what each pixel of a
// draw of Metal's private primitive types reads with what Metal's list of the same vertices reads.
// Written for this repository; no third-party source. Loop-free.
#include <metal_stdlib>
using namespace metal;

struct PtVertex {
    float4 position [[position]];
    float4 smooth_id;
    uint4 tags [[flat]];
};

vertex PtVertex pt_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                          uint base_vertex [[base_vertex]], uint base_instance [[base_instance]],
                          const device float4 *positions [[buffer(0)]],
                          const device float2 *offsets [[buffer(1)]]) {
    PtVertex out;
    float4 p = positions[min(vid, 65535u)];
    p.xy += offsets[min(iid, 3u)] * p.w;
    out.position = p;
    out.smooth_id = float4(float(vid & 0xffffu), float(vid >> 16), p.x, p.y);
    out.tags = uint4(vid, base_vertex, iid, base_instance);
    return out;
}

struct PtOut {
    float4 weights [[color(0)]];
    float4 smooth_id [[color(1)]];
    uint4 tags [[color(2)]];
    uint4 primitive [[color(3)]];
};

// The weights, the primitive ID and the front-facing bit.
fragment PtOut pt_full(PtVertex in [[stage_in]], float3 weights [[barycentric_coord]],
                       uint primitive [[primitive_id]], bool front [[front_facing]]) {
    PtOut out;
    out.weights = float4(weights, 1.0);
    out.smooth_id = in.smooth_id;
    out.tags = in.tags;
    out.primitive = uint4(primitive, front ? 1u : 0u, 0u, 1u);
    return out;
}

// The weights and the front-facing bit, without the primitive ID.
fragment PtOut pt_weights(PtVertex in [[stage_in]], float3 weights [[barycentric_coord]],
                          bool front [[front_facing]]) {
    PtOut out;
    out.weights = float4(weights, 1.0);
    out.smooth_id = in.smooth_id;
    out.tags = in.tags;
    out.primitive = uint4(0u, front ? 1u : 0u, 0u, 1u);
    return out;
}

// The primitive ID, without the weights.
fragment PtOut pt_primitive(PtVertex in [[stage_in]], uint primitive [[primitive_id]]) {
    PtOut out;
    out.weights = float4(0.0, 0.0, 0.0, 1.0);
    out.smooth_id = in.smooth_id;
    out.tags = in.tags;
    out.primitive = uint4(primitive, 0u, 0u, 1u);
    return out;
}

// Neither the weights nor the primitive ID.
fragment PtOut pt_plain(PtVertex in [[stage_in]], bool front [[front_facing]]) {
    PtOut out;
    out.weights = float4(0.0, 0.0, 0.0, 1.0);
    out.smooth_id = in.smooth_id;
    out.tags = in.tags;
    out.primitive = uint4(0u, front ? 1u : 0u, 0u, 1u);
    return out;
}
