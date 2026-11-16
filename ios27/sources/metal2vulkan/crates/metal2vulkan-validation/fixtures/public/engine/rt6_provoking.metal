// Owned fixture: vertex functions that place vertex `vid` at the clip-space position buffer 0
// holds at `min(vid, 65535)` and write its vertex ID as a smooth value and (`pv_vertex`) as a
// flat one; and fragment functions that return what reads a primitive's provoking vertex and
// vertex order (the flat vertex ID, the barycentric weights), some or all of them, with the
// smooth value and the primitive ID, so a lane checks each pixel of a draw under
// setProvokingVertexMode: against the vertex Metal reads its flat inputs from and the order of
// its weights. Written for this repository; no third-party source. Loop-free.
#include <metal_stdlib>
using namespace metal;

struct PvVertex {
    float4 position [[position]];
    float smooth_id;
    uint flat_id [[flat]];
};

struct PvSmooth {
    float4 position [[position]];
    float smooth_id;
};

vertex PvVertex pv_vertex(uint vid [[vertex_id]], const device float4 *positions [[buffer(0)]]) {
    PvVertex out;
    out.position = positions[min(vid, 65535u)];
    out.smooth_id = float(vid);
    out.flat_id = vid;
    return out;
}

vertex PvSmooth pv_vertex_smooth(uint vid [[vertex_id]],
                                 const device float4 *positions [[buffer(0)]]) {
    PvSmooth out;
    out.position = positions[min(vid, 65535u)];
    out.smooth_id = float(vid);
    return out;
}

struct PvOut {
    float4 weights [[color(0)]];
    float4 smooth_id [[color(1)]];
    uint4 ids [[color(2)]];
};

// The flat vertex ID and the primitive ID.
fragment PvOut pv_flat(PvVertex in [[stage_in]], uint primitive [[primitive_id]]) {
    PvOut out;
    out.weights = float4(0.0, 0.0, 0.0, 1.0);
    out.smooth_id = float4(in.smooth_id, 0.0, 0.0, 1.0);
    out.ids = uint4(in.flat_id, primitive, 0u, 1u);
    return out;
}

// The flat vertex ID, the weights and the primitive ID.
fragment PvOut pv_both(PvVertex in [[stage_in]], float3 weights [[barycentric_coord]],
                       uint primitive [[primitive_id]]) {
    PvOut out;
    out.weights = float4(weights, 1.0);
    out.smooth_id = float4(in.smooth_id, 0.0, 0.0, 1.0);
    out.ids = uint4(in.flat_id, primitive, 0u, 1u);
    return out;
}

// The weights and the primitive ID, no flat input.
fragment PvOut pv_weights(PvSmooth in [[stage_in]], float3 weights [[barycentric_coord]],
                          uint primitive [[primitive_id]]) {
    PvOut out;
    out.weights = float4(weights, 1.0);
    out.smooth_id = float4(in.smooth_id, 0.0, 0.0, 1.0);
    out.ids = uint4(0u, primitive, 0u, 1u);
    return out;
}

// The smooth value and the primitive ID alone: nothing that reads the provoking vertex.
fragment PvOut pv_smooth(PvSmooth in [[stage_in]], uint primitive [[primitive_id]]) {
    PvOut out;
    out.weights = float4(0.0, 0.0, 0.0, 1.0);
    out.smooth_id = float4(in.smooth_id, 0.0, 0.0, 1.0);
    out.ids = uint4(0u, primitive, 0u, 1u);
    return out;
}

// The flat vertex ID and the weights, without the primitive ID (Metal counts one a quad).
fragment PvOut pv_bare(PvVertex in [[stage_in]], float3 weights [[barycentric_coord]]) {
    PvOut out;
    out.weights = float4(weights, 1.0);
    out.smooth_id = float4(in.smooth_id, 0.0, 0.0, 1.0);
    out.ids = uint4(in.flat_id, 0u, 0u, 1u);
    return out;
}
