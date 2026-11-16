// Owned fixture: [[sample_mask, post_depth_coverage]] under early fragment tests. A pass at 4
// samples draws a depth-writing triangle, then a triangle covering the whole target behind part
// of it: `pdc_after` reads the coverage left after the depth test and writes its popcount / 4,
// `pdc_plain` writes 1.0. Each runs once per pixel and writes its samples that pass, so the
// resolved red of `pdc_after` is the square of `pdc_plain`'s (an M3 Max, measured). Loop-free.
#include <metal_stdlib>
using namespace metal;

struct PdcPoint {
    float4 position [[position]];
};

// Corner `id` of the triangle in buffer 0, at the depth in buffer 1.
vertex PdcPoint pdc_vertex(uint id [[vertex_id]], constant float2 *corners [[buffer(0)]],
                           constant float &depth [[buffer(1)]]) {
    return PdcPoint { float4(corners[id], depth, 1.0) };
}

// The share of the pixel's samples that passed the early depth test.
[[early_fragment_tests]] fragment float4 pdc_after(uint mask [[sample_mask, post_depth_coverage]]) {
    return float4(float(popcount(mask)) / 4.0, 0.0, 0.0, 1.0);
}

// 1.0 on every sample that passes.
[[early_fragment_tests]] fragment float4 pdc_plain() {
    return float4(1.0, 0.0, 0.0, 1.0);
}
