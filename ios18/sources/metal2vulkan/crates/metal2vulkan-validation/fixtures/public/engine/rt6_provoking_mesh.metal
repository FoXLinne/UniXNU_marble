// Owned fixture: mesh functions that draw four triangles, and six lines, of twelve vertices whose
// indices name them in shuffled orders, each vertex `i` at a fixed clip-space position of its own
// (`w` from 1 to 1.9, so the weights are perspective-correct) with its index as a smooth value and
// a flat one; and fragment functions that return what reads a primitive's provoking vertex and
// vertex order (the flat index, the barycentric weights), some or all of them, with the smooth
// value, in the attachments of `rt6_provoking`'s fragment functions, so a lane checks each pixel
// of a mesh draw under setProvokingVertexMode: against the index Metal reads its flat inputs from
// (the primitive's last at mode 1, a Metal probe on the M3 Max) and the order of its weights.
// Each of the mesh threadgroup's twelve threads writes its own vertex and index. Written for this
// repository; no third-party source. Loop-free.
#include <metal_stdlib>
using namespace metal;

struct PmVertex {
    float4 position [[position]];
    float smooth_id;
    uint flat_id [[flat]];
};

using PmTriangles = metal::mesh<PmVertex, void, 12, 4, topology::triangle>;
using PmLines = metal::mesh<PmVertex, void, 12, 6, topology::line>;

constant float2 PM_POSITIONS[12] = {
    {-0.9, -0.9}, {0.9, -0.8}, {-0.7, 0.9}, {0.95, 0.95}, {-0.95, 0.6}, {0.6, -0.95},
    {-0.2, -0.95}, {0.95, 0.1}, {-0.95, -0.1}, {0.1, 0.95}, {0.9, -0.5}, {-0.6, -0.7},
};

// The triangles' indices: (0, 1, 2), (5, 3, 4), (8, 6, 7), (10, 11, 9).
constant uchar PM_TRIANGLES[12] = {0, 1, 2, 5, 3, 4, 8, 6, 7, 10, 11, 9};

// The lines' indices: (0, 1), (3, 2), (4, 7), (9, 5), (6, 10), (11, 8).
constant uchar PM_LINES[12] = {0, 1, 3, 2, 4, 7, 9, 5, 6, 10, 11, 8};

static PmVertex pm_vertex(uint i) {
    PmVertex v;
    float w = 1.0 + 0.3 * float(i % 4);
    v.position = float4(PM_POSITIONS[i] * w, 0.5, w);
    v.smooth_id = float(i);
    v.flat_id = i;
    return v;
}

[[mesh]] void pm_triangles(PmTriangles out, uint tid [[thread_index_in_threadgroup]]) {
    if (tid < 12) {
        out.set_vertex(tid, pm_vertex(tid));
        out.set_index(tid, PM_TRIANGLES[tid]);
    }
    if (tid == 0) {
        out.set_primitive_count(4);
    }
}

[[mesh]] void pm_lines(PmLines out, uint tid [[thread_index_in_threadgroup]]) {
    if (tid < 12) {
        out.set_vertex(tid, pm_vertex(tid));
        out.set_index(tid, PM_LINES[tid]);
    }
    if (tid == 0) {
        out.set_primitive_count(6);
    }
}

struct PmSmooth {
    float4 position [[position]];
    float smooth_id;
};

struct PmOut {
    float4 weights [[color(0)]];
    float4 smooth_id [[color(1)]];
    uint4 ids [[color(2)]];
};

// The flat index and the weights.
fragment PmOut pm_both(PmVertex in [[stage_in]], float3 weights [[barycentric_coord]]) {
    PmOut out;
    out.weights = float4(weights, 1.0);
    out.smooth_id = float4(in.smooth_id, 0.0, 0.0, 1.0);
    out.ids = uint4(in.flat_id, 0u, 0u, 1u);
    return out;
}

// The flat index alone.
fragment PmOut pm_flat(PmVertex in [[stage_in]]) {
    PmOut out;
    out.weights = float4(0.0, 0.0, 0.0, 1.0);
    out.smooth_id = float4(in.smooth_id, 0.0, 0.0, 1.0);
    out.ids = uint4(in.flat_id, 0u, 0u, 1u);
    return out;
}

// The weights alone, no flat input.
fragment PmOut pm_weights(PmSmooth in [[stage_in]], float3 weights [[barycentric_coord]]) {
    PmOut out;
    out.weights = float4(weights, 1.0);
    out.smooth_id = float4(in.smooth_id, 0.0, 0.0, 1.0);
    out.ids = uint4(0u, 0u, 0u, 1u);
    return out;
}
