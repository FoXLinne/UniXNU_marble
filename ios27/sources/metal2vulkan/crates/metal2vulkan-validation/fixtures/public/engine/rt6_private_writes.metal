// Owned fixture: a vertex function that writes memory, for draws of Metal's private primitive
// types: each invocation counts itself at its position's slot of buffer 0 (its vertex ID less its
// base vertex, modulo 64, a second 64 slots for instances past the first) and stores the vertex ID
// and base vertex it read at the same slot of buffer 1, and places its vertex at a corner of the
// clip-space square from the vertex ID's low bits; and a fragment function that returns white. So
// a lane compares how many times each list vertex runs, and what it reads, with Metal's.
// Written for this repository; no third-party source. Loop-free.
#include <metal_stdlib>
using namespace metal;

struct PwVertex {
    float4 position [[position]];
};

vertex PwVertex pw_vertex(uint vid [[vertex_id]], uint base_vertex [[base_vertex]],
                          uint iid [[instance_id]], device atomic_uint *counts [[buffer(0)]],
                          device uint2 *seen [[buffer(1)]]) {
    uint slot = ((vid - base_vertex) & 63u) + 64u * min(iid, 1u);
    atomic_fetch_add_explicit(&counts[slot], 1u, memory_order_relaxed);
    seen[slot] = uint2(vid, base_vertex);
    PwVertex out;
    out.position = float4(float(vid & 1u) - 0.5, float((vid >> 1) & 1u) - 0.5, 0.0, 1.0);
    return out;
}

fragment float4 pw_fragment() {
    return float4(1.0);
}
