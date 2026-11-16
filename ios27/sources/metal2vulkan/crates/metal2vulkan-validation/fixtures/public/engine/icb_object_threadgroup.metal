// Owned fixture: a mesh render pipeline whose object function has threadgroup memory arguments 0
// and 1, for the lengths an executed indirect command's mesh draw runs it with. Its thread 0
// writes 0xb into b[0], then 0xa into a[0] to a[63], then b[0] into word 0 of object buffer 0
// and a[63] into word 1: where argument 0 is shorter than 256 bytes, a's writes reach b, which
// follows it. It launches no mesh threadgroup.
#include <metal_stdlib>
using namespace metal;

struct Payload {
    uint unused;
};

struct ObjectTgVertex {
    float4 position [[position]];
};

using ObjectTgMesh = metal::mesh<ObjectTgVertex, void, 3, 1, topology::triangle>;

[[object]] void icb_object_threadgroup_object(object_data Payload &payload [[payload]],
                                              mesh_grid_properties grid,
                                              device uint *out [[buffer(0)]],
                                              threadgroup uint *a [[threadgroup(0)]],
                                              threadgroup uint *b [[threadgroup(1)]],
                                              uint index [[thread_index_in_threadgroup]]) {
    if (index == 0) {
        b[0] = 0xb;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (uint i = 0; i < 64; i++) {
            a[i] = 0xa;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        out[0] = b[0];
        out[1] = a[63];
    }
    grid.set_threadgroups_per_grid(uint3(0, 0, 0));
}

[[mesh]] void icb_object_threadgroup_mesh(ObjectTgMesh output) {
}

fragment uint4 icb_object_threadgroup_fragment() {
    return uint4(1);
}
