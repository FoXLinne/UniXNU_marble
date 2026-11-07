// Owned fixture: a vertex and a fragment function whose texture
// slots Metal's render-pipeline reflection states one access each: a read texture in the vertex
// stage, and in the fragment stage a read texture, a write texture, a read-write texture, an array
// of two write textures, a write texture the function never references, and a sampled texture.
// Written for this repository; no third-party source. Loop-free.
#include <metal_stdlib>
using namespace metal;

vertex float4 texture_accesses_vertex(uint vid [[vertex_id]],
                                      texture2d<float, access::read> vt [[texture(0)]]) {
    float4 read = vt.read(uint2(0, 0));
    return float4(float(vid & 1), float(vid >> 1), read.z, 1.0);
}

fragment float4 texture_accesses_fragment(float4 p [[position]],
                                          texture2d<float, access::read> r [[texture(0)]],
                                          texture2d<float, access::write> w [[texture(1)]],
                                          texture2d<float, access::read_write> rw [[texture(2)]],
                                          array<texture2d<float, access::write>, 2> wa [[texture(3)]],
                                          texture2d<float, access::write> unused [[texture(5)]],
                                          texture2d<float> s [[texture(6)]],
                                          sampler smp [[sampler(0)]]) {
    uint2 c = uint2(p.xy);
    float4 v = r.read(c) + rw.read(c) + s.sample(smp, p.xy);
    w.write(v, c);
    rw.write(v, c);
    wa[0].write(v, c);
    wa[1].write(v, c);
    return v;
}
