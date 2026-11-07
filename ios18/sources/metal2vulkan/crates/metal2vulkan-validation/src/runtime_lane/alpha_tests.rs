//! The private alpha test of OpenGL on Metal (`PrivateRenderState::alpha_test_enabled` and
//! `alpha_test_function`, with the encoder's `setAlphaTestReferenceValue:`), on the device the cfg
//! selects, under the validation layer.
//!
//! Metal's rule, measured with our own probes `spi` to `spi4` on an Apple M3 Max under macOS
//! 27.0.1 (4 x 4 targets, `RGBA8Unorm`, `RGBA16Float` and `RGBA32Float`, a fullscreen triangle of
//! one color): color 0's alpha and the reference are each clamped to `[0, 1]`, a NaN taken as 0,
//! and converted to an 8-bit unorm code from the exact product with 255 rounded to nearest; the
//! fragment passes where the function holds between the alpha's code and the reference's, and one
//! that fails writes nothing and counts no visibility. An encoder that never set the reference
//! compares against 1, a reference set in an earlier encoder not kept. The rule matched all 413
//! probed rows (every function at alphas and references in and past `[0, 1]`, NaNs, both sides of
//! an 8-bit code, ties) and 17 near-tie rows. Each draw here is checked against it: a passing
//! draw's texels and count as the same draw's without the test, a failing draw's as the clear's
//! and 0.

use super::{
    buffer_with, layer_reported_nothing_at_all, read_buffer, validated_device_with, Messages,
    ScratchCache, WAIT,
};
use crate::air_library::AirLibrary;
use crate::gpu::TEST_DEVICE;
use metal2vulkan::metal::{PixelFormat, PrimitiveType, TextureUsage};
use metal2vulkan::pipeline::{ColorAttachmentDescriptor, RenderPipelineDescriptor, StageSource};
use metal2vulkan_devtools::{engine_dir, llvm_assemble};
use metal2vulkan_runtime::{
    ClearColor, CommandQueue, Device, Library, LoadAction, PipelineOptions, Region,
    RenderPassDescriptor, RenderPipelineState, ResourceOptions, StoreAction, TextureDescriptor,
    VisibilityResultMode,
};

/// The target's side.
const SIDE: u64 = 4;

/// The alphas drawn: inside `[0, 1]`, past it, NaN, and either side of an 8-bit code's edge.
const ALPHAS: [f32; 8] = [0.25, 0.5, 0.75, 1.5, f32::NAN, 0.501_953_1, 0.998, 0.999_9];

/// The references set (`None`: never set in the encoder).
const REFERENCES: [Option<f32>; 8] = [
    None,
    Some(0.0),
    Some(0.5),
    Some(0.75),
    Some(1.0),
    Some(f32::NAN),
    Some(-0.5),
    Some(0.501_953_1),
];

/// Metal's 8-bit unorm code of `x` as its alpha test compares it (the module docs): the exact
/// product with 255 of `x` clamped, rounded to nearest (its one tie, at 0.5, to 128).
fn code(x: f32) -> u32 {
    let x = if x.is_nan() { 0.0 } else { x.clamp(0.0, 1.0) };
    (f64::from(x) * 255.0).round_ties_even() as u32
}

/// Whether Metal's alpha test of `function` (`MTLCompareFunction`'s raw value) passes `alpha`
/// against `reference`.
fn passes(function: u64, alpha: f32, reference: Option<f32>) -> bool {
    let (a, r) = (code(alpha), code(reference.unwrap_or(1.0)));
    match function {
        0 => false,
        1 => a < r,
        2 => a == r,
        3 => a <= r,
        4 => a > r,
        5 => a != r,
        6 => a >= r,
        _ => true,
    }
}

struct Lane {
    device: Device,
    queue: CommandQueue,
    /// The owned fixture `a8r_targets`: a fullscreen triangle and a fragment writing buffer 0's
    /// color.
    library: Library,
}

impl Lane {
    fn new(messages: &Messages, cache: &ScratchCache) -> Lane {
        let device = validated_device_with(TEST_DEVICE, messages, cache.path(), |_| {}).unwrap();
        let bitcode = llvm_assemble(&engine_dir().join("a8r_targets.ll")).unwrap();
        Lane {
            queue: device.new_command_queue().unwrap(),
            library: device.new_library_with_air(&bitcode).unwrap(),
            device,
        }
    }

    fn function(&self, name: &str) -> StageSource {
        StageSource::Function(
            self.library
                .new_function_with_name(name)
                .unwrap()
                .function_descriptor(),
        )
    }

    /// The fullscreen fill into `pf`, the alpha test of `function` on where one is given.
    fn pipeline(&self, pf: PixelFormat, function: Option<u64>) -> RenderPipelineState {
        let mut d = RenderPipelineDescriptor::new(self.function("a8r_vertex"));
        d.fragment = Some(self.function("a8r_fill"));
        d.color_attachments[0] = Some(ColorAttachmentDescriptor::new(pf));
        if let Some(function) = function {
            d.private.alpha_test_enabled = true;
            d.private.alpha_test_function = function;
        }
        self.device
            .new_render_pipeline_state(&d, &PipelineOptions::default())
            .unwrap_or_else(|e| panic!("{pf:?} alpha test {function:?}: {e}"))
    }

    /// One pass into a `pf` target cleared to zero, drawing `color` through `pipeline` with the
    /// reference set where one is given (in an earlier encoder of the command buffer too, where
    /// `earlier` is given, which the draw's encoder must not keep): the target's bytes and the
    /// visibility count.
    fn draw(
        &self,
        pf: PixelFormat,
        pipeline: &RenderPipelineState,
        color: [f32; 4],
        (reference, earlier): (Option<f32>, Option<f32>),
    ) -> (Vec<u8>, u64) {
        let texel = match pf {
            PixelFormat::RGBA16Float => 8,
            _ => 4,
        };
        let mut d = TextureDescriptor::texture_2d(pf, SIDE, SIDE, false);
        d.usage = TextureUsage::RENDER_TARGET;
        d.resource_options = ResourceOptions::shared();
        let target = self.device.new_texture(&d).unwrap();
        let vis = buffer_with(&self.device, &[0xaa; 8]).unwrap();
        let bytes: Vec<u8> = color.iter().flat_map(|v| v.to_le_bytes()).collect();
        let color = buffer_with(&self.device, &bytes).unwrap();
        let mut pass = RenderPassDescriptor::new();
        let a = &mut pass.color_attachments[0];
        a.texture = Some(target.clone());
        a.load_action = LoadAction::Clear;
        a.clear_color = ClearColor::new(0.0, 0.0, 0.0, 0.0);
        a.store_action = StoreAction::Store;
        pass.visibility_result_buffer = Some(vis.clone());
        let command_buffer = self.queue.command_buffer().unwrap();
        if let Some(value) = earlier {
            let mut e = command_buffer.render_command_encoder(&pass).unwrap();
            e.set_alpha_test_reference_value(value).unwrap();
            e.end_encoding().unwrap();
        }
        let mut e = command_buffer.render_command_encoder(&pass).unwrap();
        e.set_render_pipeline_state(pipeline).unwrap();
        e.set_fragment_buffer(Some(&color), 0, 0).unwrap();
        e.set_visibility_result_mode(VisibilityResultMode::Counting, 0)
            .unwrap();
        if let Some(value) = reference {
            e.set_alpha_test_reference_value(value).unwrap();
        }
        e.draw_primitives(PrimitiveType::Triangle, 0, 3, 1, 0)
            .unwrap();
        e.end_encoding().unwrap();
        command_buffer.commit().unwrap();
        command_buffer.wait_until_completed_for(WAIT).unwrap();
        assert_eq!(command_buffer.error(), None);
        let mut out = vec![0u8; (SIDE * SIDE) as usize * texel];
        target
            .get_bytes(
                &mut out,
                SIDE * texel as u64,
                0,
                Region::new_2d(0, 0, SIDE, SIDE),
                0,
                0,
            )
            .unwrap();
        let count = u64::from_le_bytes(read_buffer(&vis, 0, 8).unwrap().try_into().unwrap());
        (out, count)
    }
}

/// Every function's test at every alpha of [`ALPHAS`] against every reference of
/// [`REFERENCES`], into `RGBA8Unorm` and `RGBA16Float` (which keeps an alpha past 1): each
/// draw passes or fails as Metal's rule says (the module docs).
#[test]
fn the_alpha_test_keeps_the_fragments_metal_keeps() {
    let messages = Messages::default();
    let cache = ScratchCache::new().unwrap();
    let lane = Lane::new(&messages, &cache);
    let name = lane.device.name().to_owned();
    let mut wrong = Vec::new();
    let mut checked = 0usize;
    for pf in [PixelFormat::RGBA8Unorm, PixelFormat::RGBA16Float] {
        let untested = lane.pipeline(pf, None);
        let cleared = lane.draw(pf, &untested, [0.0; 4], (None, None)).0;
        for function in 0..=7u64 {
            let tested = lane.pipeline(pf, Some(function));
            for alpha in ALPHAS {
                let color = [0.25, 0.5, 0.75, alpha];
                let (kept, all) = lane.draw(pf, &untested, color, (None, None));
                for reference in REFERENCES {
                    let got = lane.draw(pf, &tested, color, (reference, None));
                    let want = match passes(function, alpha, reference) {
                        true => (kept.clone(), all),
                        false => (cleared.clone(), 0),
                    };
                    checked += 1;
                    if got != want {
                        wrong.push(format!(
                            "{pf:?} function {function} alpha {alpha} reference {reference:?}: \
                             count {} (want {})",
                            got.1, want.1
                        ));
                    }
                }
            }
        }
    }
    // A reference set in an earlier encoder is not kept: the draw compares against 1.
    let pf = PixelFormat::RGBA8Unorm;
    let less = lane.pipeline(pf, Some(1));
    let (_, count) = lane.draw(pf, &less, [0.25, 0.5, 0.75, 0.5], (None, Some(0.25)));
    checked += 1;
    if count != SIDE * SIDE {
        wrong.push(format!(
            "an earlier encoder's reference kept: count {count}"
        ));
    }
    assert!(
        wrong.is_empty(),
        "{name}: {} of {checked} draws differ from Metal's:\n{}",
        wrong.len(),
        wrong.join("\n")
    );
    eprintln!("{name}: {checked} alpha-tested draws keep Metal's fragments");
    drop(lane);
    layer_reported_nothing_at_all(&messages);
}
