// Owned fixture: a fragment imageblock of one float in passes of one and four samples, and beside a
// depth attachment. `ibs_write` writes the cell, `ibs_add` adds to it, `ibs_add_discard` adds but
// discards column 7, `ibs_write_masked` writes it and returns the sample mask 0b0101,
// `ibs_write_early` writes it under `[[early_fragment_tests]]`, and `ibs_read` returns the cell as
// red. `ibs_plain` returns 1.0, with no imageblock: its resolve is a draw's coverage. On the M3 Max
// (macOS 27.0.1) Metal keeps a cell per sample at four samples, runs each function that reaches it
// once per covered sample, keeps a write only where the returned mask has the run's sample, and
// drops the write of a fragment its depth test fails. Loop-free.
#include <metal_stdlib>
using namespace metal;

struct IbsCorner {
    float4 position [[position]];
};

// Corner `id` of the triangle in buffer 0, at the depth in buffer 1.
vertex IbsCorner ibs_vertex(uint id [[vertex_id]], constant float2 *corners [[buffer(0)]],
                            constant float &depth [[buffer(1)]]) {
    return IbsCorner { float4(corners[id], depth, 1.0) };
}

struct IbsCell {
    float v [[raster_order_group(0)]];
};

struct IbsCellOut {
    IbsCell cell [[imageblock_data]];
};

struct IbsMaskOut {
    IbsCell cell [[imageblock_data]];
    uint mask [[sample_mask]];
};

fragment IbsCellOut ibs_write(constant float &value [[buffer(0)]]) {
    IbsCellOut out;
    out.cell.v = value;
    return out;
}

[[early_fragment_tests]] fragment IbsCellOut ibs_write_early(constant float &value [[buffer(0)]]) {
    IbsCellOut out;
    out.cell.v = value;
    return out;
}

fragment IbsCellOut ibs_add(IbsCell in [[imageblock_data]], constant float &value [[buffer(0)]]) {
    IbsCellOut out;
    out.cell.v = in.v + value;
    return out;
}

fragment IbsCellOut ibs_add_discard(IbsCell in [[imageblock_data]],
                                    constant float &value [[buffer(0)]],
                                    float4 position [[position]]) {
    if (uint(position.x) == 7u) {
        discard_fragment();
    }
    IbsCellOut out;
    out.cell.v = in.v + value;
    return out;
}

fragment IbsMaskOut ibs_write_masked(constant float &value [[buffer(0)]]) {
    IbsMaskOut out;
    out.cell.v = value;
    out.mask = 0x5u;
    return out;
}

fragment float4 ibs_read(IbsCell in [[imageblock_data]]) {
    return float4(in.v, 0.0, 0.0, 1.0);
}

fragment float4 ibs_plain() {
    return float4(1.0, 0.0, 0.0, 1.0);
}
