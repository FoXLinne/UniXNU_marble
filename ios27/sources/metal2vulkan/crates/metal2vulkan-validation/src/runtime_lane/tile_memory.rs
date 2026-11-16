//! A render pass's tile threadgroup memory (`setThreadgroupMemoryLength:offset:atIndex:`,
//! `runtime/render/tile_memory.rs`) on the device the cfg selects, under the validation layer. The
//! owned fixture `cfit_tile_memory`'s tile functions run in passes of one `R32Uint` attachment of
//! 40x24 pixels in 16x16 tiles, 3x2 of them, each of 4x4 threads, whose `threadgroupMemoryLength`
//! is 128 bytes: `add` adds `1000 * tile + t + 1` to word `t` of its argument 0, `dump` writes word
//! `t` of its argument 0 to `out[tile * 16 + t]`, and `shift` writes word `t` of its argument 0
//! plus `0x10000` to word `t` of its argument 1.
//!
//! As Metal does on an M4 and an M3 Max (identical answers):
//!
//! - A tile's memory persists across the pass's tile dispatches: two `add`s leave twice their
//!   words in each tile's own memory.
//! - It starts at zero with each pass, and nothing carries from one pass to the next.
//! - Each argument sits at the byte offset the encoder set: `shift` with argument 1 at byte 16
//!   writes words 4 to 19, which argument 0 reads back, and `dump` with argument 0 at byte 64
//!   reads words 16 on, past `shift`'s still zero.
//! - The setter takes every length and every offset, as plain Metal does (an M3 Max under macOS
//!   27.0.1: every length and offset tried took, the pass completing). An argument begins at its
//!   offset floored to a multiple of 4, and one at or past the tile's memory reads zero throughout
//!   and has its stores dropped, as an argument that ends past it does from that end on —
//!   measurements `RenderCommandEncoder::set_threadgroup_memory_length` names. A
//!   dispatch of a function whose argument the encoder never set is the binder's
//!   `MissingThreadgroupMemory`: plain Metal runs it with the argument at a driver-chosen offset
//!   (byte 120 of 256 on an M3 Max, our probe), which no rule gives.

use super::tile_states::on_device;
use super::{buffer_with, read_buffer, WAIT};
use crate::air_library::AirLibrary;
use metal2vulkan::error::BindError;
use metal2vulkan::metal::{PixelFormat, TextureUsage};
use metal2vulkan::pipeline::{StageSource, TileRenderPipelineDescriptor};
use metal2vulkan_devtools::{engine_dir, llvm_assemble};
use metal2vulkan_runtime::{
    Buffer, CommandBufferStatus, CommandQueue, Device, Error, Invalid, LoadAction, PipelineOptions,
    Problem, RenderCommandEncoder, RenderPassDescriptor, RenderPipelineState, Size, StoreAction,
    Texture, TextureDescriptor,
};

/// The pass's tiles.
const TILES: u32 = 6;

/// The threads of a tile.
const THREADS: u32 = 16;

/// The pass's `threadgroupMemoryLength`.
const LENGTH: u64 = 128;

/// Where a tile threadgroup argument's offset wraps: plain Metal truncates it to 32 bits, so byte
/// `WRAP` begins where byte 0 does (`RenderCommandEncoder::set_threadgroup_memory_length`).
const WRAP: u64 = u32::MAX as u64 + 1;

/// The fixture's tile functions' states, each in a tile pipeline of one `R32Uint` attachment.
struct States {
    add: RenderPipelineState,
    dump: RenderPipelineState,
    shift: RenderPipelineState,
}

impl States {
    fn new(device: &Device) -> States {
        let bitcode = llvm_assemble(&engine_dir().join("cfit_tile_memory.ll")).unwrap();
        let library = device.new_library_with_air(&bitcode).unwrap();
        let state = |name: &str| {
            let function = library.new_function_with_name(name).unwrap();
            let mut descriptor = TileRenderPipelineDescriptor::new(StageSource::Function(
                function.function_descriptor(),
            ));
            descriptor.color_attachment_formats[0] = Some(PixelFormat::R32Uint);
            device
                .new_tile_render_pipeline_state(&descriptor, &PipelineOptions::default())
                .unwrap()
        };
        States {
            add: state("cfit_tile_memory_add"),
            dump: state("cfit_tile_memory_dump"),
            shift: state("cfit_tile_memory_shift"),
        }
    }
}

/// The passes' attachment.
fn target(device: &Device) -> Texture {
    let mut descriptor = TextureDescriptor::texture_2d(PixelFormat::R32Uint, 40, 24, false);
    descriptor.usage = TextureUsage::RENDER_TARGET;
    device.new_texture(&descriptor).unwrap()
}

/// A pass over `target` of 16x16 tiles, each of [`LENGTH`] bytes of threadgroup memory.
fn pass(target: &Texture) -> RenderPassDescriptor {
    let mut pass = RenderPassDescriptor::new();
    pass.color_attachments[0].texture = Some(target.clone());
    pass.color_attachments[0].load_action = LoadAction::Clear;
    pass.color_attachments[0].store_action = StoreAction::DontCare;
    pass.tile_width = 16;
    pass.tile_height = 16;
    pass.threadgroup_memory_length = LENGTH;
    pass
}

/// A buffer of `dump`'s words, each `0xABABABAB` until written.
fn out(device: &Device) -> Buffer {
    buffer_with(device, &[0xAB; (TILES * THREADS * 4) as usize]).unwrap()
}

/// The words of `buffer`.
fn words(buffer: &Buffer) -> Vec<u32> {
    read_buffer(buffer, 0, buffer.length())
        .unwrap()
        .as_chunks::<4>()
        .0
        .iter()
        .map(|word| u32::from_le_bytes(*word))
        .collect()
}

/// A dispatch of `state`, 4x4 threads a tile.
fn dispatch(encoder: &mut RenderCommandEncoder, state: &RenderPipelineState) {
    encoder.set_render_pipeline_state(state).unwrap();
    encoder
        .dispatch_threads_per_tile(Size::new(4, 4, 1))
        .unwrap();
}

/// Encode a pass over `target` for each of `passes` on a new command buffer of `queue`, and run
/// it.
fn run(queue: &CommandQueue, target: &Texture, passes: &[&dyn Fn(&mut RenderCommandEncoder)]) {
    let command_buffer = queue.command_buffer().unwrap();
    for encode in passes {
        let mut encoder = command_buffer
            .render_command_encoder(&pass(target))
            .unwrap();
        encode(&mut encoder);
        encoder.end_encoding().unwrap();
    }
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    assert_eq!(command_buffer.status(), CommandBufferStatus::Completed);
}

/// Word `t` of `tile`'s memory after `times` dispatches of `add`.
fn added(tile: u32, t: u32, times: u32) -> u32 {
    times * (1000 * tile + t + 1)
}

/// Each tile's words `word(tile, t)`, in `dump`'s order.
fn by_tile(word: impl Fn(u32, u32) -> u32) -> Vec<u32> {
    (0..TILES)
        .flat_map(|tile| (0..THREADS).map(move |t| (tile, t)))
        .map(|(tile, t)| word(tile, t))
        .collect()
}

#[test]
fn a_tiles_threadgroup_memory_persists_across_the_passs_dispatches() {
    on_device(|device| {
        let states = States::new(device);
        let target = target(device);
        let queue = device.new_command_queue().unwrap();
        let outs = [out(device), out(device), out(device)];
        run(
            &queue,
            &target,
            &[&|encoder| {
                encoder.set_threadgroup_memory_length(64, 0, 0).unwrap();
                dispatch(encoder, &states.add);
                dispatch(encoder, &states.add);
                encoder.set_tile_buffer(Some(&outs[0]), 0, 0).unwrap();
                dispatch(encoder, &states.dump);
                encoder.set_threadgroup_memory_length(64, 16, 1).unwrap();
                dispatch(encoder, &states.shift);
                encoder.set_tile_buffer(Some(&outs[1]), 0, 0).unwrap();
                dispatch(encoder, &states.dump);
                encoder.set_threadgroup_memory_length(64, 64, 0).unwrap();
                encoder.set_tile_buffer(Some(&outs[2]), 0, 0).unwrap();
                dispatch(encoder, &states.dump);
            }],
        );
        assert_eq!(
            words(&outs[0]),
            by_tile(|tile, t| added(tile, t, 2)),
            "two adds"
        );
        assert_eq!(
            words(&outs[1]),
            by_tile(|tile, t| match t {
                0..4 => added(tile, t, 2),
                _ => added(tile, t - 4, 2) + 0x10000,
            }),
            "the shift through argument 1 at byte 16"
        );
        assert_eq!(
            words(&outs[2]),
            by_tile(|tile, t| match t {
                0..4 => added(tile, t + 12, 2) + 0x10000,
                _ => 0,
            }),
            "argument 0 at byte 64"
        );
    });
}

#[test]
fn each_pass_starts_its_tiles_threadgroup_memory_at_zero() {
    on_device(|device| {
        let states = States::new(device);
        let target = target(device);
        let queue = device.new_command_queue().unwrap();
        let outs = [out(device), out(device), out(device)];
        let add = |encoder: &mut RenderCommandEncoder, out: &Buffer| {
            encoder.set_threadgroup_memory_length(64, 0, 0).unwrap();
            dispatch(encoder, &states.add);
            encoder.set_tile_buffer(Some(out), 0, 0).unwrap();
            dispatch(encoder, &states.dump);
        };
        run(
            &queue,
            &target,
            &[&|encoder| add(encoder, &outs[0]), &|encoder| {
                add(encoder, &outs[1])
            }],
        );
        run(&queue, &target, &[&|encoder| add(encoder, &outs[2])]);
        for (out, when) in outs
            .iter()
            .zip(["first", "second", "next command buffer's"])
        {
            assert_eq!(
                words(out),
                by_tile(|tile, t| added(tile, t, 1)),
                "the {when} pass"
            );
        }
    });
}

/// An argument begins at its offset truncated to 32 bits and floored to a multiple of 4, and one
/// at or past the tile's memory reads zero throughout and has its stores dropped, as plain Metal
/// answers (the module docs). `add` fills words 0 to 15 of each tile's memory with
/// `1000 * tile + t + 1`, a value no unwritten word and no other word holds, and `dump` through an
/// argument at `offset` then reads the memory from the word that offset begins at. Offsets 0 and 4
/// are the differentiated control: they read words 0 and 1 on, so a reading that did not move
/// between them would mean the offset never reached the shader. 4, 8 and 20 are also what
/// separates a floor to 4 from a floor to 16, which 17 alone cannot (both place it at byte 16),
/// and 2^32 and 2^32 + 4 are what separates truncating the offset from saturating it, which would
/// read zero instead of the memory's first words.
#[test]
fn a_tile_threadgroup_argument_begins_at_its_offset_floored_to_four() {
    on_device(|device| {
        let states = States::new(device);
        let target = target(device);
        let queue = device.new_command_queue().unwrap();
        // One pass an offset: a tile's memory starts at zero with each pass, so each reading is
        // the pass's own `add` alone.
        for (offset, first) in [
            (0, 0),
            (2, 0),
            (4, 1),
            (8, 2),
            (17, 4),
            (20, 5),
            (WRAP, 0),
            (WRAP + 4, 1),
            (WRAP + 20, 5),
            (2 * WRAP + 8, 2),
        ] {
            let out = out(device);
            run(
                &queue,
                &target,
                &[&|encoder| {
                    encoder.set_threadgroup_memory_length(64, 0, 0).unwrap();
                    dispatch(encoder, &states.add);
                    encoder
                        .set_threadgroup_memory_length(64, offset, 0)
                        .unwrap();
                    encoder.set_tile_buffer(Some(&out), 0, 0).unwrap();
                    dispatch(encoder, &states.dump);
                }],
            );
            assert_eq!(
                words(&out),
                by_tile(|tile, t| match first + t {
                    word if word < THREADS => added(tile, word, 1),
                    _ => 0,
                }),
                "argument 0 at byte {offset}, which begins at word {first}"
            );
        }
        // At or past the tile's memory: every read is zero and every store is dropped, so neither
        // the `add` through such an argument nor the `dump` through it sees a word.
        for offset in [LENGTH, LENGTH + 16, 1 << 20, WRAP + LENGTH] {
            let outs = [out(device), out(device)];
            run(
                &queue,
                &target,
                &[&|encoder| {
                    encoder
                        .set_threadgroup_memory_length(64, offset, 0)
                        .unwrap();
                    dispatch(encoder, &states.add);
                    encoder.set_tile_buffer(Some(&outs[0]), 0, 0).unwrap();
                    dispatch(encoder, &states.dump);
                    encoder.set_threadgroup_memory_length(64, 0, 0).unwrap();
                    encoder.set_tile_buffer(Some(&outs[1]), 0, 0).unwrap();
                    dispatch(encoder, &states.dump);
                }],
            );
            assert_eq!(
                words(&outs[0]),
                by_tile(|_, _| 0),
                "a dump through an argument at byte {offset}"
            );
            assert_eq!(
                words(&outs[1]),
                by_tile(|_, _| 0),
                "the memory after an add through an argument at byte {offset}"
            );
        }
    });
}

#[test]
fn tile_threadgroup_arguments_answer_as_plain_metal_does() {
    on_device(|device| {
        let states = States::new(device);
        let target = target(device);
        let queue = device.new_command_queue().unwrap();
        run(
            &queue,
            &target,
            &[
                &|encoder| {
                    let invalid = |problem| {
                        Err(Error::Invalid(Invalid::new(
                            "set_threadgroup_memory_length",
                            problem,
                        )))
                    };
                    assert_eq!(
                        encoder.set_threadgroup_memory_length(64, 0, 31),
                        invalid(Problem::IndexOutOfRange {
                            index: 31,
                            limit: 31
                        })
                    );
                    // Every length and every offset is taken: a multiple of 16 or not, an argument
                    // that ends past the tile's memory, an offset at or past that memory, and an
                    // offset past a `u32`. Plain Metal takes each.
                    for (length, offset) in [
                        (8, 0),
                        (0, 0),
                        (1, 16),
                        (64, 112),
                        (u64::MAX, 32),
                        (64, 8),
                        (64, 17),
                        (64, 144),
                        (64, u64::from(u32::MAX) + 1),
                    ] {
                        assert_eq!(
                            encoder.set_threadgroup_memory_length(length, offset, 0),
                            Ok(()),
                            "{length} at {offset}"
                        );
                    }
                    // The whole tile's memory, the setter's last byte.
                    assert_eq!(encoder.set_threadgroup_memory_length(LENGTH, 0, 0), Ok(()));
                },
                // A dispatch of a function whose argument the encoder never set, which needs a pass of
                // its own: once any call above has set argument 0 the encoder has it, so the claim
                // only holds on an encoder that has set nothing.
                &|encoder| {
                    encoder.set_render_pipeline_state(&states.add).unwrap();
                    assert_eq!(
                        encoder.dispatch_threads_per_tile(Size::new(4, 4, 1)),
                        Err(Error::Bind(BindError::MissingThreadgroupMemory {
                            index: 0
                        }))
                    );
                },
            ],
        );
    });
}
