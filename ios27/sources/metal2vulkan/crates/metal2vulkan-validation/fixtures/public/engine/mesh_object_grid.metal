// Owned fixture: object grids of any count, for the runtime lane `mesh_draws` and Metal probe 5.
// Thread 0 of each object threadgroup counts it in `n` (its buffer 0): one more threadgroup, its
// linear position and that position's square added (each wrapping), the largest position, and the
// grid and threads per grid it read, each or-ed into one word and and-ed into another (equal words
// say every threadgroup read the same); it launches no mesh threadgroup. Loop-free.
#include <metal_stdlib>
using namespace metal;

struct GridVertex {
    float4 position [[position]];
};

using GridMesh = metal::mesh<GridVertex, void, 3, 1, topology::triangle>;

// `n[i]` or-ed with `v`, and `n[i + 4]` and-ed with it.
static void both(device atomic_uint* n, uint i, uint v) {
    atomic_fetch_or_explicit(&n[i], v, memory_order_relaxed);
    atomic_fetch_and_explicit(&n[i + 4], v, memory_order_relaxed);
}

[[object]] void mesh_object_grid_object(mesh_grid_properties grid,
                                        uint tid [[thread_index_in_threadgroup]],
                                        uint3 group [[threadgroup_position_in_grid]],
                                        uint3 groups [[threadgroups_per_grid]],
                                        uint3 tpg [[threads_per_grid]],
                                        device atomic_uint* n [[buffer(0)]]) {
    if (tid == 0) {
        uint linear = group.x + groups.x * (group.y + groups.y * group.z);
        atomic_fetch_add_explicit(&n[0], 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&n[1], linear, memory_order_relaxed);
        atomic_fetch_add_explicit(&n[2], linear * linear, memory_order_relaxed);
        atomic_fetch_max_explicit(&n[3], linear, memory_order_relaxed);
        both(n, 4, groups.x);
        both(n, 5, groups.y);
        both(n, 6, groups.z);
        both(n, 7, tpg.x);
        grid.set_threadgroups_per_grid(uint3(0, 1, 1));
    }
}

[[mesh]] void mesh_object_grid_mesh(GridMesh out, uint tid [[thread_index_in_threadgroup]]) {
    if (tid == 0) {
        out.set_primitive_count(0);
    }
}

fragment uint4 mesh_object_grid_fragment() {
    return uint4(1);
}
