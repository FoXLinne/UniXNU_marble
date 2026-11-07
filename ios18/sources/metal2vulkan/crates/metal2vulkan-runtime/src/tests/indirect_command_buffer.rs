//! Indirect command buffers (`indirect_command_buffer.rs`): what needs no device.

use crate::indirect_command_buffer::metal_allocated_size;
use crate::IndirectCommandBufferDescriptor;
use metal2vulkan::metal::IndirectCommandType;

/// A fresh descriptor answers as Metal's (an M3 Max and an M4): no command types, neither the
/// pipeline state nor the buffers inherited, no buffers, 31 threadgroup lengths, no dynamic
/// strides; and as the macOS 26 SDK states: every render state inherited, no object threadgroup
/// lengths, no color attachment mapping.
#[test]
fn a_descriptor_has_metal_s_defaults() {
    let descriptor = IndirectCommandBufferDescriptor::new();
    assert!(descriptor.command_types.is_empty());
    assert!(!descriptor.inherit_pipeline_state && !descriptor.inherit_buffers);
    assert!(
        descriptor.inherit_depth_stencil_state
            && descriptor.inherit_depth_bias
            && descriptor.inherit_depth_clip_mode
            && descriptor.inherit_cull_mode
            && descriptor.inherit_front_facing_winding
            && descriptor.inherit_triangle_fill_mode
    );
    assert_eq!(descriptor.max_object_threadgroup_memory_bind_count, 0);
    assert!(!descriptor.support_color_attachment_mapping);
    assert_eq!(
        [
            descriptor.max_vertex_buffer_bind_count,
            descriptor.max_fragment_buffer_bind_count,
            descriptor.max_kernel_buffer_bind_count,
            descriptor.max_kernel_threadgroup_memory_bind_count,
        ],
        [0, 0, 0, 31]
    );
    assert!(!descriptor.support_dynamic_attribute_stride);
    // Apple's private properties, as a fresh descriptor answers on an M4 (plain and validated
    // alike).
    assert!(
        descriptor.inherit_stencil_reference_values
            && descriptor.inherit_depth_test_bounds
            && descriptor.inherit_scissor_rects
            && descriptor.inherit_viewports
            && descriptor.inherit_blend_color
    );
    assert_eq!(
        (
            descriptor.max_scissor_rect_count,
            descriptor.max_viewport_count,
            descriptor.allow_override_render_states
        ),
        (1, 1, 0)
    );
}

/// `allocatedSize` is Metal's: rows measured on an M4, at a page's last count and the next where it
/// turns, and rows measured on an M3 Max for the buffer counts past 31. Each row: the command types, whether the pipeline and the buffers are inherited, dynamic
/// strides, the vertex, fragment, kernel, object and mesh buffer counts, the count of commands, and
/// Metal's answer. Every kind answers one page for no commands.
#[test]
fn a_buffer_answers_metal_s_allocated_size() {
    type Row = (u32, bool, bool, bool, [u32; 5], u32, u64);
    let rows: [Row; 68] = [
        (32, false, false, true, [0, 0, 5, 0, 0], 1000, 802816),
        (64, true, false, false, [0, 0, 31, 0, 0], 1000, 966656),
        (96, false, true, true, [0, 0, 5, 0, 0], 511, 376832),
        (96, false, true, true, [0, 0, 5, 0, 0], 512, 393216),
        (96, false, true, true, [0, 0, 5, 0, 0], 1000, 737280),
        (1, false, false, true, [3, 5, 0, 0, 0], 15, 16384),
        (1, false, false, true, [3, 5, 0, 0, 0], 16, 32768),
        (1, false, false, true, [3, 5, 0, 0, 0], 1000, 1032192),
        (2, true, false, false, [31, 31, 0, 0, 0], 1000, 1196032),
        (4, false, false, true, [1, 0, 0, 0, 0], 15, 16384),
        (4, false, false, true, [1, 0, 0, 0, 0], 16, 32768),
        (4, false, false, true, [1, 0, 0, 0, 0], 1000, 1015808),
        (8, true, false, false, [0, 2, 0, 0, 0], 1000, 737280),
        (128, false, false, false, [0, 0, 0, 0, 0], 9, 16384),
        (128, false, false, false, [0, 0, 0, 0, 0], 10, 32768),
        (128, false, false, false, [0, 0, 0, 0, 0], 1000, 1671168),
        (256, true, false, false, [2, 3, 0, 4, 4], 11, 16384),
        (256, true, false, false, [2, 3, 0, 4, 4], 12, 32768),
        (256, true, false, false, [2, 3, 0, 4, 4], 1000, 1376256),
        (129, false, false, false, [2, 3, 0, 4, 4], 14, 32768),
        (129, false, false, false, [2, 3, 0, 4, 4], 15, 49152),
        (129, false, false, false, [2, 3, 0, 4, 4], 1000, 2179072),
        (132, true, false, false, [0, 0, 0, 7, 0], 9, 16384),
        (132, true, false, false, [0, 0, 0, 7, 0], 10, 32768),
        (132, true, false, false, [0, 0, 0, 7, 0], 1000, 1654784),
        (128, false, false, true, [5, 0, 0, 0, 0], 9, 16384),
        (128, false, false, true, [5, 0, 0, 0, 0], 10, 32768),
        (128, false, false, true, [5, 0, 0, 0, 0], 1000, 1687552),
        // A mesh type's odd count with dynamic strides and its own buffers: 4 bytes more,
        // past a page where the even count ends on one.
        (128, false, false, true, [0, 0, 0, 0, 0], 354, 589824),
        (128, false, false, true, [0, 0, 0, 0, 0], 355, 606208),
        (129, false, false, true, [2, 3, 0, 0, 0], 434, 917504),
        (129, false, false, true, [2, 3, 0, 0, 0], 435, 933888),
        (0, false, false, false, [0, 0, 0, 0, 0], 31, 16384),
        (0, false, false, false, [0, 0, 0, 0, 0], 32, 32768),
        (0, false, false, false, [0, 0, 0, 0, 0], 1000, 524288),
        (0, true, false, true, [5, 0, 0, 0, 0], 43, 16384),
        (0, true, false, true, [5, 0, 0, 0, 0], 44, 32768),
        (0, true, false, true, [5, 0, 0, 0, 0], 1000, 376832),
        (0, false, false, false, [0, 6, 0, 0, 0], 28, 16384),
        (0, false, false, false, [0, 6, 0, 0, 0], 29, 32768),
        (0, false, false, false, [0, 6, 0, 0, 0], 1000, 557056),
        (33, false, false, false, [0, 0, 5, 0, 0], 20, 16384),
        (33, false, false, false, [0, 0, 5, 0, 0], 21, 32768),
        (33, false, false, false, [0, 0, 5, 0, 0], 1000, 786432),
        (160, true, false, true, [3, 4, 5, 6, 7], 21, 16384),
        (160, true, false, true, [3, 4, 5, 6, 7], 22, 32768),
        (160, true, false, true, [3, 4, 5, 6, 7], 1000, 770048),
        // The buffer counts past the 31 Metal's validation allows, measured on an Apple M3 Max
        // under macOS 27.0.1 at 8 commands (`probes/icb_refusals_that_metal_takes.m`): Metal lays
        // out the slots the count asks for, so `allocatedSize` grows with it, on past the 65536
        // a refusal once stopped at. A kernel count moves a compute buffer's layout alone and a
        // vertex or fragment count a render buffer's; the other kind's rows below read one page
        // whatever the count, which is what makes these live readings.
        (32, false, false, false, [0, 0, 4, 0, 0], 8, 16384),
        (32, false, false, false, [0, 0, 31, 0, 0], 8, 16384),
        (32, false, false, false, [0, 0, 32, 0, 0], 8, 16384),
        (32, false, false, false, [0, 0, 33, 0, 0], 8, 16384),
        (32, false, false, false, [0, 0, 1024, 0, 0], 8, 81920),
        (32, false, false, false, [0, 0, 65535, 0, 0], 8, 4210688),
        (32, false, false, false, [0, 0, 65536, 0, 0], 8, 4210688),
        (32, false, false, false, [0, 0, 65537, 0, 0], 8, 4210688),
        (32, false, false, false, [0, 0, 70000, 0, 0], 8, 4489216),
        (32, false, false, false, [0, 0, 1 << 20, 0, 0], 8, 67125248),
        (
            32,
            false,
            false,
            false,
            [0, 0, 1 << 24, 0, 0],
            8,
            1073758208,
        ),
        (
            32,
            false,
            false,
            false,
            [0, 0, 1 << 25, 0, 0],
            8,
            2147500032,
        ),
        (32, false, false, false, [0, 0, 70000, 0, 0], 2, 1130496),
        (1, false, false, false, [4, 0, 0, 0, 0], 8, 16384),
        (1, false, false, false, [1024, 0, 0, 0, 0], 8, 81920),
        (1, false, false, false, [65536, 0, 0, 0, 0], 8, 4210688),
        (1, false, false, false, [65537, 0, 0, 0, 0], 8, 4210688),
        (1, false, false, false, [0, 65536, 0, 0, 0], 8, 4210688),
        // A count the buffer's own layout ignores: one page, whatever it asks for.
        (1, false, false, false, [0, 0, 65536, 0, 0], 8, 16384),
        (1, false, false, false, [0, 0, 0, 65536, 0], 8, 16384),
        (1, false, false, false, [0, 0, 0, 0, 65536], 8, 16384),
    ];
    for (
        types,
        pipeline,
        buffers,
        strides,
        [vertex, fragment, kernel, object, mesh],
        count,
        size,
    ) in rows
    {
        let mut descriptor = IndirectCommandBufferDescriptor::new();
        descriptor.command_types = IndirectCommandType::try_from(types).unwrap();
        descriptor.inherit_pipeline_state = pipeline;
        descriptor.inherit_buffers = buffers;
        descriptor.support_dynamic_attribute_stride = strides;
        descriptor.max_vertex_buffer_bind_count = vertex;
        descriptor.max_fragment_buffer_bind_count = fragment;
        descriptor.max_kernel_buffer_bind_count = kernel;
        descriptor.max_object_buffer_bind_count = object;
        descriptor.max_mesh_buffer_bind_count = mesh;
        assert_eq!(
            metal_allocated_size(&descriptor, count),
            size,
            "types {types}, {count} commands"
        );
        assert_eq!(metal_allocated_size(&descriptor, 0), 16384, "types {types}");
    }
}

/// An execution range read from a buffer, as measured on an Apple M3 Max under macOS 27.0.1 with
/// the API validation layer off (`rangebuf.m`, `rangebuf2.m`, `rangebuf3.m`, one case per process,
/// on a compute encoder's dispatches and a render encoder's draws alike): plain Metal refuses
/// neither of the two rules below, so neither is misuse. Both are the runtime's own gaps.
#[test]
fn an_execution_range_metal_takes_is_the_runtime_s_gap() {
    use crate::error::{Error, RuntimeGap};
    use crate::indirect_execute::check_range_buffer;
    // Aligned and inside the buffer: taken, as Metal takes it.
    for offset in [0, 4, 8, 1000, 4088] {
        check_range_buffer(4096, offset).unwrap();
    }
    // An offset that is not a multiple of 4: Metal's GPU faults the command buffer where the
    // range read there runs a command, and completes where it runs none.
    for offset in [1, 2, 3, 6, 1002] {
        assert_eq!(
            check_range_buffer(4096, offset).unwrap_err(),
            Error::NotImplemented(RuntimeGap::ExecutionRangeOffFour { offset }),
            "offset {offset}"
        );
    }
    // 8 bytes that run past the buffer: Metal reads whatever bytes follow it.
    for (length, offset) in [(12, 8), (12, 12), (12, 16), (16384, 16400), (4096, 4092)] {
        assert_eq!(
            check_range_buffer(length, offset).unwrap_err(),
            Error::NotImplemented(RuntimeGap::ExecutionRangePastBuffer { offset, length }),
            "{offset} of {length}"
        );
    }
    // An offset whose 8 bytes overflow is past the buffer, not off four.
    assert_eq!(
        check_range_buffer(16, u64::MAX - 3).unwrap_err(),
        Error::NotImplemented(RuntimeGap::ExecutionRangePastBuffer {
            offset: u64::MAX - 3,
            length: 16,
        })
    );
}

/// An executed indirect command's buffer bound without the attribute stride its pipeline state's
/// layout of `MTLBufferLayoutStrideDynamic` reads is input plain Metal takes: the setters, the
/// execution and the commit return and the command buffer completes, nothing faulting (an M3 Max
/// under macOS 27.0.1, the API validation layer off, our probe `unstrided.m`; and an M3 Max and an
/// M4 before it, with and without the layer). On a buffer whose descriptor supports dynamic
/// attribute strides the fetch reads at stride 2^32 - 1 there, which the 32-bit fetch cannot
/// place, so the runtime has no exact answer, which makes this its own gap and never misuse
/// ([`unstrided_buffer`], the decision both encoders' executions make): the lowest index that is
/// both bound without a stride and read by such a layout is refused, and a command whose layouts
/// read none of them is taken.
#[test]
fn an_unstrided_executed_buffer_is_the_runtime_s_gap_not_misuse() {
    use crate::error::{Error, RuntimeGap};
    use crate::indirect_execute::unstrided_buffer;
    let gap = |index| {
        Some(Error::NotImplemented(
            RuntimeGap::IndirectCommandUnstridedBuffer { index },
        ))
    };
    // Nothing bound without a stride: nothing refused, whatever the layouts read.
    assert_eq!(unstrided_buffer(true, [], |_| true), None);
    // Bound without a stride only at indices no dynamic layout reads: taken, as Metal takes it.
    assert_eq!(unstrided_buffer(true, [1, 3], |index| index == 2), None);
    assert_eq!(unstrided_buffer(true, [1, 3], |_| false), None);
    // The lowest index that is both, whatever the order the indices come in.
    assert_eq!(
        unstrided_buffer(true, [3, 1, 7], |index| index != 7),
        gap(1)
    );
    assert_eq!(
        unstrided_buffer(true, [3, 1, 7], |index| index == 7),
        gap(7)
    );
    assert_eq!(unstrided_buffer(true, [0], |_| true), gap(0));
    // The refusal is the runtime's own gap, never `Error::Invalid`.
    for refusal in [
        unstrided_buffer(true, [0], |_| true),
        unstrided_buffer(true, [30], |_| true),
    ] {
        assert!(
            matches!(refusal, Some(Error::NotImplemented(_))),
            "{refusal:?}"
        );
    }
}

/// A command of an indirect command buffer whose descriptor does *not* support dynamic attribute
/// strides holds no stride word in its records at all, so Metal's fetch reads a buffer bound there
/// at stride 0, which the plan path gives exactly (`bind::dispatch`'s `PushField::StageInStride`
/// and `bind::graphics`'s per-bind stride read a binding without a stride at 0; the fetch itself
/// is `engine_kernel_stage_in`'s `a_dynamic_layout_reads_the_bound_stride`, whose unbound stride
/// reads 0). Measured on an Apple M3 Max under macOS 27.0.1, the API validation layer silent
/// there: the plain setter's command read element 1 at stride 0 in five runs of five. So the gap
/// is refused only with that support, where the fetch reads at stride 2^32 - 1
/// ([`unstrided_buffer`]) -- the two halves of one input, decided by the descriptor's bit and by
/// nothing else.
#[test]
fn an_unstrided_buffer_without_dynamic_stride_support_reads_stride_zero() {
    use crate::error::{Error, RuntimeGap};
    use crate::indirect_execute::unstrided_buffer;
    // Every shape the supporting buffer refuses is taken without the support, at every index a
    // dynamic layout reads: the execution goes on to the plan path's stride 0.
    for unstrided in [vec![0], vec![30], vec![3, 1, 7]] {
        let refusal = unstrided_buffer(false, unstrided.iter().copied(), |_| true);
        assert_eq!(refusal, None, "without support, {unstrided:?}");
        let index = unstrided.iter().copied().min().expect("an index");
        assert_eq!(
            unstrided_buffer(true, unstrided.iter().copied(), |_| true),
            Some(Error::NotImplemented(
                RuntimeGap::IndirectCommandUnstridedBuffer { index }
            )),
            "with support, {unstrided:?}"
        );
    }
    // Nothing a layout reads: taken either way.
    assert_eq!(unstrided_buffer(false, [1, 3], |_| false), None);
    assert_eq!(unstrided_buffer(true, [1, 3], |_| false), None);
}
