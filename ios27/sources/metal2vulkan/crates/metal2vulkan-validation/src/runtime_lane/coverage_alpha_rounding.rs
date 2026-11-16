//! Alpha to coverage under Apple's `textureWriteRoundingMode` 2 (`NearestEven`), on the device the
//! cfg selects, under the validation layer.
//!
//! Metal's rule, measured with our own probe `a2c` on an Apple M3 Max under macOS 27.0.1 (a 128²
//! `RGBA16Float` target of 4 samples, each pixel drawn `(1, 1, 1, a)` with its own alpha `a` just
//! past the midpoint between two halves, resolved): the coverage of all 16,384 alphas is the same
//! under mode 0 and mode 2, so the coverage reads the alpha before rounding, and the stored alpha
//! is the nearest-even half under mode 2. A draw of mode 2 at `a` matched a draw of mode 0 at that
//! half in 16,382 of the 16,384 pixels; the 2 others covered another sample.
//!
//! The function rounds the alpha to that half, which the coverage then reads, announced as
//! `coverage-reads-rounded-alpha`. Each pixel here is checked against the same draw of mode 0 at
//! the rounded half, which stores that half and covers as the device covers it; where all four
//! samples are covered the resolved alpha is the rounded half itself. An attachment whose write
//! mask stores no alpha keeps the alpha unrounded, so its coverage is Metal's: each 2x2 quad of
//! such a draw is checked against the same draw of mode 0.

use super::degrade_notices::counts;
use super::extended_range_targets::half_value;
use super::{layer_reported_nothing_at_all, validated_device_with, Messages, ScratchCache, WAIT};
use crate::air_library::AirLibrary;
use crate::gpu::TEST_DEVICE;
use metal2vulkan::metal::{
    ColorWriteMask, PixelFormat, PrimitiveType, TextureType, TextureUsage, TextureWriteRoundingMode,
};
use metal2vulkan::pipeline::{ColorAttachmentDescriptor, RenderPipelineDescriptor, StageSource};
use metal2vulkan_devtools::{engine_dir, llvm_assemble};
use metal2vulkan_runtime::{
    ClearColor, CommandQueue, Degradation, Device, Library, LoadAction, PipelineOptions, Region,
    RenderPassDescriptor, RenderPipelineState, ResourceOptions, ScissorRect, StoreAction,
    TextureDescriptor,
};

/// The target's side: one pixel per alpha.
const SIDE: u64 = 64;

/// The first half of the alphas' range, `1/16`; pixel `i` draws an alpha just past the midpoint
/// between half `FIRST + i` and the next, to `1`.
const FIRST: u16 = 0x2c00;

/// The nearest `f32` above the midpoint between half `bits` and the next: the default mode stores
/// half `bits`, nearest-even rounding the next one.
fn past_midpoint(bits: u16) -> f32 {
    let mid = (f64::from(half_value(bits)) + f64::from(half_value(bits + 1))) / 2.0;
    let mid = mid as f32;
    f32::from_bits(mid.to_bits() + 1)
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

    /// The fill into a 4-sample `RGBA16Float` attachment of write mask `mask`, alpha to coverage
    /// on, under `rounding`.
    fn pipeline(
        &self,
        rounding: TextureWriteRoundingMode,
        mask: ColorWriteMask,
    ) -> RenderPipelineState {
        let mut d = RenderPipelineDescriptor::new(self.function("a8r_vertex"));
        d.fragment = Some(self.function("a8r_fill"));
        let mut color = ColorAttachmentDescriptor::new(PixelFormat::RGBA16Float);
        color.write_mask = mask;
        d.color_attachments[0] = Some(color);
        d.raster_sample_count = 4;
        d.alpha_to_coverage = true;
        d.texture_write_rounding_mode = rounding;
        self.device
            .new_render_pipeline_state(&d, &PipelineOptions::default())
            .unwrap_or_else(|e| panic!("{rounding:?}: {e}"))
    }

    /// One pass cleared to zero drawing `(1, 1, 1, alphas[i])` through `pipeline` into pixel `i`
    /// alone (into the `side`-pixel square `i` of a row of `SIDE / side`), resolved: the resolved
    /// texels' half bits.
    fn draw(&self, pipeline: &RenderPipelineState, alphas: &[f32], side: u64) -> Vec<[u16; 4]> {
        let mut d = TextureDescriptor::texture_2d(PixelFormat::RGBA16Float, SIDE, SIDE, false);
        d.texture_type = TextureType::Type2DMultisample;
        d.sample_count = 4;
        d.usage = TextureUsage::RENDER_TARGET;
        d.resource_options = ResourceOptions::private();
        let samples = self.device.new_texture(&d).unwrap();
        let mut d = TextureDescriptor::texture_2d(PixelFormat::RGBA16Float, SIDE, SIDE, false);
        d.usage = TextureUsage::RENDER_TARGET;
        d.resource_options = ResourceOptions::shared();
        let resolved = self.device.new_texture(&d).unwrap();
        let mut pass = RenderPassDescriptor::new();
        let a = &mut pass.color_attachments[0];
        a.texture = Some(samples);
        a.resolve_texture = Some(resolved.clone());
        a.load_action = LoadAction::Clear;
        a.clear_color = ClearColor::new(0.0, 0.0, 0.0, 0.0);
        a.store_action = StoreAction::MultisampleResolve;
        let command_buffer = self.queue.command_buffer().unwrap();
        let mut e = command_buffer.render_command_encoder(&pass).unwrap();
        e.set_render_pipeline_state(pipeline).unwrap();
        let row = SIDE / side;
        for (i, &alpha) in (0u64..).zip(alphas) {
            let (x, y) = (i % row * side, i / row * side);
            e.set_scissor_rect(ScissorRect::new(x, y, side, side))
                .unwrap();
            let color: Vec<u8> = [1.0, 1.0, 1.0, alpha]
                .iter()
                .flat_map(|v: &f32| v.to_le_bytes())
                .collect();
            e.set_fragment_bytes(&color, 0).unwrap();
            e.draw_primitives(PrimitiveType::Triangle, 0, 3, 1, 0)
                .unwrap();
        }
        e.end_encoding().unwrap();
        command_buffer.commit().unwrap();
        command_buffer.wait_until_completed_for(WAIT).unwrap();
        assert_eq!(command_buffer.error(), None);
        let mut out = vec![0u8; (SIDE * SIDE * 8) as usize];
        resolved
            .get_bytes(
                &mut out,
                SIDE * 8,
                0,
                Region::new_2d(0, 0, SIDE, SIDE),
                0,
                0,
            )
            .unwrap();
        out.as_chunks::<8>()
            .0
            .iter()
            .map(|t| std::array::from_fn(|c| u16::from_le_bytes([t[2 * c], t[2 * c + 1]])))
            .collect()
    }
}

/// A 4-sample `RGBA16Float` draw covering by alpha under `NearestEven`, at 4,096 alphas from
/// `1/16` to `1` each just past the midpoint between two halves, stores in every pixel what the
/// default mode's draw of the nearest-even half stores (the module docs), the rounded half itself
/// where all four samples are covered; the mode-2 pipeline announces
/// `coverage-reads-rounded-alpha`, and not `device-color-conversion`.
#[test]
fn coverage_alpha_stores_the_nearest_even_half_under_mode_two() {
    let messages = Messages::default();
    let cache = ScratchCache::new().unwrap();
    let lane = Lane::new(&messages, &cache);
    let name = lane.device.name().to_owned();
    let count = (SIDE * SIDE) as u16;
    let alphas: Vec<f32> = (FIRST..FIRST + count).map(past_midpoint).collect();
    let rounded: Vec<f32> = (FIRST..FIRST + count).map(|h| half_value(h + 1)).collect();
    let nearest = lane.pipeline(TextureWriteRoundingMode::NearestEven, ColorWriteMask::ALL);
    let got = lane.draw(&nearest, &alphas, 1);
    // The mode-2 pipeline's notices alone: the default mode's may note the device's conversion.
    let noted = counts(&messages.take_notices());
    let default = lane.pipeline(TextureWriteRoundingMode::TowardZero, ColorWriteMask::ALL);
    let want = lane.draw(&default, &rounded, 1);
    let one = 0x3c00;
    let mut wrong = Vec::new();
    let mut covered = 0usize;
    for (i, (g, w)) in got.iter().zip(&want).enumerate() {
        let half = FIRST + i as u16 + 1;
        if g != w {
            wrong.push(format!(
                "alpha {:e}: {g:04x?}, the rounded half's draw {w:04x?}",
                alphas[i]
            ));
        }
        if g[0] == one {
            covered += 1;
            if g[3] != half {
                wrong.push(format!(
                    "alpha {:e} fully covered: alpha {:04x}, Metal's {half:04x}",
                    alphas[i], g[3]
                ));
            }
        }
    }
    assert!(
        wrong.is_empty(),
        "{name}: {} of {count} pixels differ from Metal's:\n{}",
        wrong.len(),
        wrong.join("\n")
    );
    assert!(covered > 0, "{name}: no pixel fully covered");
    eprintln!("{name}: {count} covered alphas store Metal's half ({covered} fully covered)");
    assert!(
        noted.contains_key(&Degradation::CoverageReadsRoundedAlpha)
            && !noted.contains_key(&Degradation::DeviceColorConversion),
        "{name}: {noted:?}"
    );
    drop(lane);
    layer_reported_nothing_at_all(&messages);
}

/// Alphas whose rounding to the nearest-even half crosses a coverage level's edge: for each edge
/// `(c - 0.5) / 255` of an 8-bit code `c` a multiple of 4 (the levels of 2 and 4 samples change
/// at multiples of 8), the `f32`s either side of the midpoints of the halves next to it.
fn edge_alphas() -> Vec<f32> {
    let mut out = Vec::new();
    for c in (4..=252u32).step_by(4) {
        let edge = (f64::from(c) - 0.5) / 255.0;
        // The half just below the edge.
        let mut h: u16 = 0x2c00;
        while f64::from(half_value(h + 1)) < edge {
            h += 1;
        }
        for h in h - 1..=h + 1 {
            let mid = ((f64::from(half_value(h)) + f64::from(half_value(h + 1))) / 2.0) as f32;
            out.push(f32::from_bits(mid.to_bits() - 1));
            out.push(f32::from_bits(mid.to_bits() + 1));
        }
    }
    out.retain(|&a| a >= half_value(0x2c00));
    out
}

/// A 4-sample `RGBA16Float` draw covering by alpha under `NearestEven` into an attachment whose
/// write mask stores no alpha covers exactly as the default mode's draw of the same alpha: Metal
/// reads the coverage from the alpha before rounding and stores none of it, so the function
/// keeps the alpha unrounded (and announces nothing about it). Each alpha fills a 2x2 quad, so a
/// coverage level that moves shows at whichever pixel of the quad takes it; the alphas sit
/// either side of half midpoints next to every 8-bit code's edge.
#[test]
fn an_unstored_coverage_alpha_keeps_metal_s_coverage_under_mode_two() {
    let messages = Messages::default();
    let cache = ScratchCache::new().unwrap();
    let lane = Lane::new(&messages, &cache);
    let name = lane.device.name().to_owned();
    let rgb = ColorWriteMask::RED
        .union(ColorWriteMask::GREEN)
        .union(ColorWriteMask::BLUE);
    let alphas = edge_alphas();
    let quads = (SIDE / 2 * SIDE / 2) as usize;
    assert!(alphas.len() <= quads, "{} alphas", alphas.len());
    let nearest = lane.pipeline(TextureWriteRoundingMode::NearestEven, rgb);
    let got = lane.draw(&nearest, &alphas, 2);
    let noted = counts(&messages.take_notices());
    let default = lane.pipeline(TextureWriteRoundingMode::TowardZero, rgb);
    let want = lane.draw(&default, &alphas, 2);
    let row = (SIDE / 2) as usize;
    let mut wrong = Vec::new();
    for (i, &alpha) in alphas.iter().enumerate() {
        let (x, y) = (i % row * 2, i / row * 2);
        for (dx, dy) in [(0, 0), (1, 0), (0, 1), (1, 1)] {
            let at = (y + dy) * SIDE as usize + x + dx;
            if got[at] != want[at] {
                wrong.push(format!(
                    "alpha {alpha:e} pixel ({}, {}): {:04x?}, the default mode's {:04x?}",
                    x + dx,
                    y + dy,
                    got[at],
                    want[at]
                ));
            }
        }
    }
    assert!(
        wrong.is_empty(),
        "{name}: {} pixels differ from Metal's coverage:\n{}",
        wrong.len(),
        wrong.join("\n")
    );
    let covered = want.iter().filter(|t| t[0] != 0).count();
    assert!(covered > 0, "{name}: nothing covered");
    eprintln!(
        "{name}: {} alphas next to code edges cover as the default mode ({covered} pixels)",
        alphas.len()
    );
    assert!(
        !noted.contains_key(&Degradation::CoverageReadsRoundedAlpha),
        "{name}: {noted:?}"
    );
    drop(lane);
    layer_reported_nothing_at_all(&messages);
}
