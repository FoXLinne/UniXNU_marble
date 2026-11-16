// metal-macos-min: 27.0
// Owned fixture: the newest AIR this toolchain emits, and the four shapes its step changed. At
// 27.0 the compiler writes `air64_v29` and `!air.version = !{i32 2, i32 9, i32 0}` (the ceiling:
// `air64_v210` is not a triple this compiler knows), with `!air.language_version = !{!"Metal", i32
// 4, i32 1, i32 0}`. The shapes are:
//
//  - `frexp` as the intrinsic `air.frexp.<T>(T, thread I*)`, where every earlier emitter outlines
//    it as the runtime routine `___metal_frexp_<T>_pthread<I>`. Both read to one `Math`, so one
//    expansion serves them and the two spellings translate to the same module;
//  - a memory atomic with its `mem_flags` immediate between the scope and the volatile flag;
//  - `air.wg.barrier(i32 flags, i32 order, i32 scope)`, the order inserted in the middle, where
//    AIR 2.8 writes `(flags, scope)` and means `seq_cst`;
//  - a texture read with a sampler and an offset position, and `air.get_read_sampler()`, which
//    AIR 2.8 added and this version keeps.
//
// `ldexp` is here too: every version from AIR 1.8 on spells it `air.fast_ldexp.<T>`, and AIR 1.6
// alone outlines it as `___metal_ldexp_<T>_<I>` with a third operand this fixture cannot reach.
// Loop-free.
#include <metal_stdlib>
using namespace metal;

kernel void a29_split_floats(const device float4 *in [[buffer(0)]],
                             device float4 *mantissas [[buffer(1)]],
                             device int4 *exponents [[buffer(2)]],
                             device float4 *rebuilt [[buffer(3)]],
                             uint i [[thread_position_in_grid]]) {
    int4 e;
    float4 m = frexp(in[i], e);
    mantissas[i] = m;
    exponents[i] = e;
    rebuilt[i] = ldexp(m, e);
}

kernel void a29_atomics_and_barrier(device atomic_uint *counter [[buffer(0)]],
                                    device uint *out [[buffer(1)]],
                                    threadgroup uint *scratch [[threadgroup(0)]],
                                    uint i [[thread_position_in_grid]],
                                    uint tid [[thread_index_in_threadgroup]]) {
    scratch[tid] = tid;
    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
    uint previous = atomic_fetch_add_explicit(counter, scratch[tid], memory_order_relaxed);
    atomic_fetch_or_explicit(counter, 1u, memory_order_relaxed);
    out[i] = previous + atomic_load_explicit(counter, memory_order_relaxed);
}

kernel void a29_read_texels(texture2d<float, access::read> src [[texture(0)]],
                            texture2d<float, access::write> dst [[texture(1)]],
                            uint2 gid [[thread_position_in_grid]]) {
    dst.write(src.read(gid), gid);
}
