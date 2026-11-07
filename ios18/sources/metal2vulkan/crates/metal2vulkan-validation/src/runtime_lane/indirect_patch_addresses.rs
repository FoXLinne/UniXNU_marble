//! The addresses Apple's private patch draw getters answer of an indirect render command, on the
//! device the cfg selects, as Metal answers them (the M3 Max under macOS 27.0.1): a nil patch
//! index or control-point index buffer answers `0x1eeee1111` whatever its offset, a buffer its
//! address plus the offset's low 32 bits.

use super::host_memory::on_device;
use metal2vulkan::metal::IndirectCommandType;
use metal2vulkan_runtime::{
    IndirectCommand, IndirectCommandBuffer, IndirectCommandBufferDescriptor, IndirectDraw,
    ResourceOptions, NIL_PATCH_BUFFER_ADDRESS,
};

/// The draw command `index` of `icb` reads back as.
fn draw(icb: &IndirectCommandBuffer, index: u64) -> IndirectDraw {
    match icb.commands(index..index + 1).unwrap().remove(0) {
        IndirectCommand::Render(command) => command.draw.expect("a draw"),
        other => panic!("not a render command: {other:?}"),
    }
}

/// Each offset of the probe, nil and over a buffer, of both patch draws.
#[test]
fn patch_draws_answer_metal_s_buffer_addresses() {
    on_device(|device| {
        let mut descriptor = IndirectCommandBufferDescriptor::new();
        descriptor.command_types =
            IndirectCommandType::DRAW_PATCHES.union(IndirectCommandType::DRAW_INDEXED_PATCHES);
        descriptor.inherit_pipeline_state = true;
        descriptor.inherit_buffers = true;
        let icb = device
            .new_indirect_command_buffer(&descriptor, 1, ResourceOptions::shared())
            .unwrap();
        let factors = device.new_buffer(4096, ResourceOptions::shared()).unwrap();
        let indices = device.new_buffer(4096, ResourceOptions::shared()).unwrap();
        let base = indices.gpu_address().0;
        let command = icb.indirect_render_command_at(0).unwrap();
        for offset in [0, 0x10, 0xffff_fff0, 0x1_0000_0010, 0x1_eeee_1111] {
            let over = base + (offset & 0xffff_ffff);
            let patches = |buffer| {
                command
                    .draw_patches(3, 0, 1, buffer, offset, 1, 0, &factors, 0, 0)
                    .unwrap();
                let read = draw(&icb, 0);
                (
                    read.patch_index_buffer_address(),
                    read.control_point_index_buffer_address(),
                )
            };
            let nil = Some(NIL_PATCH_BUFFER_ADDRESS);
            assert_eq!(patches(None), (nil, nil), "{offset:#x}");
            // The offsets past the buffer's length stand for their low 32 bits alone, inside
            // it: Metal's getter answers those; the draw's patch index range is not drawn.
            if offset & 0xffff_ffff < 4096 {
                assert_eq!(patches(Some(&indices)), (Some(over), nil), "{offset:#x}");
            }
            let indexed = |buffer, control_points| {
                command
                    .draw_indexed_patches(
                        3,
                        0,
                        1,
                        buffer,
                        offset,
                        control_points,
                        offset,
                        1,
                        0,
                        &factors,
                        0,
                        0,
                    )
                    .unwrap();
                let read = draw(&icb, 0);
                (
                    read.patch_index_buffer_address(),
                    read.control_point_index_buffer_address(),
                )
            };
            assert_eq!(indexed(None, None), (nil, nil), "{offset:#x}");
            if offset & 0xffff_ffff < 4096 {
                assert_eq!(
                    indexed(None, Some(&indices)),
                    (nil, Some(over)),
                    "{offset:#x}"
                );
            }
            command.reset().unwrap();
        }
    });
}
