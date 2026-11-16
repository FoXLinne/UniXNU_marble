//! The samples alpha to coverage covers (`PrecisionNote::DeviceAlphaCoverage`,
//! `DriverDeviation::MetalAlphaCoverage`), on the device the cfg selects, under the validation
//! layer, over a sweep of alphas at every sample count the device offers, pixel by pixel against
//! Metal's own reading of the same draws (`alpha_coverage_m3.txt`).
//!
//! Metal's reading: the probe `metal2vulkan-conformance/probes/alpha_coverage_sweep.m` draws this
//! lane's sweep this lane's way through plain Metal on an Apple M3 Max under macOS 27.0.1, at the
//! counts it offers (1, 2 and 4). At one sample a pixel is covered where its alpha is above 1/2.
//! At two and four samples a pixel covers `floor(alpha * n)` or `ceil(alpha * n)` samples, by its
//! place in a 2x2 ordered dither of window coordinates: the count is `ceil(alpha * n - t)`, `t`
//! being 1/8 at even `x` and `y`, 5/8 at odd `x` and even `y`, 7/8 at even `x` and odd `y` and 3/8
//! at odd `x` and `y`, but within an `f32` step of a threshold (an alpha on it, or one ulp off it),
//! where Metal's count lies on either side; so `alpha * n` itself where that is an integer. Vulkan
//! specifies no algorithm for converting the alpha to a coverage mask, and lets it differ by
//! framebuffer coordinates, so the device's coverage is its own answer: a pipeline that covers by
//! alpha records the note on a profile without the deviation, and on one with it records none and
//! requires `VkProperty::MetalAlphaCoverage`.
//!
//! Each alpha of the sweep ([`alphas`]: every multiple of 1/512 in `[0, 1]`, and the `f32` either
//! side of every multiple of 1/64 inside it, where every such threshold of 1, 2, 4 and 8 samples
//! lies) fills its own 4x4 block of a 128x128 `RGBA16Float` target of `n` samples, cleared to
//! zero, through the owned fixture `a8r_targets` (a fullscreen triangle and a fragment writing
//! buffer 0's color), drawn `(1, 1, 1, alpha)` with alpha to coverage on; a target of more than one
//! sample is resolved. A pixel's red is then `k / n` for the `k` samples it covered, read back
//! exactly in `half`. Every sample count the device makes such a target of
//! (`supports_texture_sample_count`, of 1, 2, 4, 8 and 16) is measured.
//!
//! Where the profile takes the deviation, every pixel at every count Metal's reading holds covers
//! Metal's samples, and no draw announces `device-alpha-coverage`. Elsewhere every pipeline records
//! the note and its draws announce it, and the test prints how many pixels cover another count than
//! Metal's, by how many samples at most, and whether each pixel's count lies between
//! `floor(alpha * n)` and `ceil(alpha * n)`, holding none.

use super::degrade_notices::counts;
use super::extended_range_targets::half_value;
use super::{layer_reported_nothing_at_all, validated_device_with, Messages, ScratchCache, WAIT};
use crate::air_library::AirLibrary;
use crate::gpu::TEST_DEVICE;
use metal2vulkan::device::VkProperty;
use metal2vulkan::error::DriverDeviation;
use metal2vulkan::metal::{PixelFormat, PrimitiveType, TextureType, TextureUsage};
use metal2vulkan::pipeline::{
    ColorAttachmentDescriptor, PrecisionNote, RenderPipelineDescriptor, StageSource,
};
use metal2vulkan::Budget;
use metal2vulkan_devtools::{engine_dir, llvm_assemble};
use metal2vulkan_runtime::{
    ClearColor, CommandQueue, Degradation, Device, DeviceOptions, Library, LoadAction,
    PipelineOptions, Region, RenderPassDescriptor, ResourceOptions, ScissorRect, StoreAction,
    TextureDescriptor,
};
use std::collections::BTreeMap;

/// Metal's reading of the sweep: a line a sample count and alpha, `<n> <the alpha's bits, hex>
/// <its block's 16 counts, row by row, base 36>`, after `#` comment lines.
const METAL: &str = include_str!("alpha_coverage_m3.txt");

/// The target's side.
const SIDE: u64 = 128;

/// The side of the block of pixels one alpha fills.
const BLOCK: usize = 4;

/// The sample counts a device may offer, each measured where it does.
const COUNTS: [u32; 5] = [1, 2, 4, 8, 16];

/// The alphas of the sweep, ascending, each once: every multiple of 1/512 in `[0, 1]`, and the
/// `f32` either side of every multiple of 1/64 inside it.
fn alphas() -> Vec<f32> {
    let mut out: Vec<f32> = (0..=512u16).map(|i| f32::from(i) / 512.0).collect();
    for j in 1..64u16 {
        let edge = f32::from(j) / 64.0;
        out.push(f32::from_bits(edge.to_bits() - 1));
        out.push(f32::from_bits(edge.to_bits() + 1));
    }
    out.sort_by(f32::total_cmp);
    out.dedup();
    out
}

/// One block's counts, row by row.
type Block = [u32; BLOCK * BLOCK];

/// Metal's counts by sample count and alpha bits ([`METAL`]).
fn metal_reading() -> BTreeMap<(u32, u32), Block> {
    METAL
        .lines()
        .filter(|line| !line.starts_with('#'))
        .map(|line| {
            let mut fields = line.split(' ');
            let mut field = || fields.next().unwrap_or_else(|| panic!("{line}"));
            let n: u32 = field().parse().unwrap();
            let bits = u32::from_str_radix(field(), 16).unwrap();
            let digits: Vec<u32> = field().chars().map(|c| c.to_digit(36).unwrap()).collect();
            let block: Block = digits.try_into().unwrap_or_else(|_| panic!("{line}"));
            ((n, bits), block)
        })
        .collect()
}

struct Lane {
    device: Device,
    queue: CommandQueue,
    /// The owned fixture `a8r_targets`.
    library: Library,
}

impl Lane {
    /// A lane device that announces `device-alpha-coverage` at each draw that takes it, so each
    /// pipeline's draws are read apart.
    fn new(messages: &Messages, cache: &ScratchCache) -> Lane {
        let each = |options: &mut DeviceOptions| {
            options.announce_each = [Degradation::DeviceAlphaCoverage].into_iter().collect();
        };
        let device = validated_device_with(TEST_DEVICE, messages, cache.path(), each).unwrap();
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

    /// The fill into an `RGBA16Float` attachment of `n` samples, alpha to coverage on where
    /// `coverage`.
    fn descriptor(&self, n: u32, coverage: bool) -> RenderPipelineDescriptor {
        let mut d = RenderPipelineDescriptor::new(self.function("a8r_vertex"));
        d.fragment = Some(self.function("a8r_fill"));
        d.color_attachments[0] = Some(ColorAttachmentDescriptor::new(PixelFormat::RGBA16Float));
        d.raster_sample_count = n;
        d.alpha_to_coverage = coverage;
        d
    }

    /// One pass of `n` samples cleared to zero drawing `(1, 1, 1, alphas[i])` through `d`'s state
    /// into block `i` alone, resolved where `n` is above 1: each block's counts of covered samples.
    fn draw(&self, d: &RenderPipelineDescriptor, n: u32, alphas: &[f32]) -> Vec<Block> {
        let pipeline = self
            .device
            .new_render_pipeline_state(d, &PipelineOptions::default())
            .unwrap_or_else(|e| panic!("{n} samples: {e}"));
        let mut t = TextureDescriptor::texture_2d(PixelFormat::RGBA16Float, SIDE, SIDE, false);
        t.usage = TextureUsage::RENDER_TARGET;
        t.resource_options = ResourceOptions::shared();
        let read = self.device.new_texture(&t).unwrap();
        let mut pass = RenderPassDescriptor::new();
        let a = &mut pass.color_attachments[0];
        if n > 1 {
            let mut t = TextureDescriptor::texture_2d(PixelFormat::RGBA16Float, SIDE, SIDE, false);
            t.texture_type = TextureType::Type2DMultisample;
            t.sample_count = u64::from(n);
            t.usage = TextureUsage::RENDER_TARGET;
            t.resource_options = ResourceOptions::private();
            a.texture = Some(self.device.new_texture(&t).unwrap());
            a.resolve_texture = Some(read.clone());
            a.store_action = StoreAction::MultisampleResolve;
        } else {
            a.texture = Some(read.clone());
            a.store_action = StoreAction::Store;
        }
        a.load_action = LoadAction::Clear;
        a.clear_color = ClearColor::new(0.0, 0.0, 0.0, 0.0);
        let command_buffer = self.queue.command_buffer().unwrap();
        let mut e = command_buffer.render_command_encoder(&pass).unwrap();
        e.set_render_pipeline_state(&pipeline).unwrap();
        for (i, &alpha) in alphas.iter().enumerate() {
            let (x, y) = origin(i);
            let side = BLOCK as u64;
            e.set_scissor_rect(ScissorRect::new(x as u64, y as u64, side, side))
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
        read.get_bytes(
            &mut out,
            SIDE * 8,
            0,
            Region::new_2d(0, 0, SIDE, SIDE),
            0,
            0,
        )
        .unwrap();
        let count = |x: usize, y: usize| {
            let at = (y * SIDE as usize + x) * 8;
            let red = f64::from(half_value(u16::from_le_bytes([out[at], out[at + 1]])));
            let k = red * f64::from(n);
            assert!(
                k.fract() == 0.0 && (0.0..=f64::from(n)).contains(&k),
                "{n} samples: a red of {red} is no count of covered samples"
            );
            k as u32
        };
        (0..alphas.len())
            .map(|i| {
                let (x, y) = origin(i);
                std::array::from_fn(|p| count(x + p % BLOCK, y + p / BLOCK))
            })
            .collect()
    }
}

/// The top-left pixel of block `i`.
fn origin(i: usize) -> (usize, usize) {
    let row = SIDE as usize / BLOCK;
    (i % row * BLOCK, i / row * BLOCK)
}

/// At every sample count the device offers, a pipeline covering by alpha covers Metal's samples
/// at every pixel of the sweep where the profile takes `MetalAlphaCoverage`, records no
/// `DeviceAlphaCoverage` and requires the property; elsewhere it records the note, its draws
/// announce it, and its difference from Metal's reading is printed (the module docs). A pipeline
/// without alpha to coverage records neither and covers every sample.
#[test]
fn alpha_to_coverage_covers_metal_s_samples_where_measured() {
    let messages = Messages::default();
    let cache = ScratchCache::new().unwrap();
    {
        let lane = Lane::new(&messages, &cache);
        let name = lane.device.name().to_owned();
        let metal = lane
            .device
            .compile_device()
            .profile()
            .takes_driver_deviation(DriverDeviation::MetalAlphaCoverage);
        println!("A2C {name}: the profile takes MetalAlphaCoverage: {metal}");
        let alphas = alphas();
        assert!(alphas.len() <= (SIDE as usize / BLOCK).pow(2));
        let reading = metal_reading();
        let mut read: Vec<u32> = reading.keys().map(|&(n, _)| n).collect();
        read.dedup();
        let swept: Vec<u32> = alphas.iter().map(|a| a.to_bits()).collect();
        for &n in &read {
            let bits: Vec<u32> = reading
                .keys()
                .filter(|&&(m, _)| m == n)
                .map(|&(_, bits)| bits)
                .collect();
            assert_eq!(
                bits, swept,
                "Metal's reading at {n} samples is not of the lane's sweep"
            );
        }
        let offered: Vec<u32> = COUNTS
            .into_iter()
            .filter(|&n| {
                lane.device
                    .properties()
                    .supports_texture_sample_count(n.into())
                    == Ok(true)
            })
            .collect();
        assert!(offered.contains(&1), "{name}: {offered:?}");
        let mut wrong = Vec::new();
        for &n in &offered {
            for coverage in [false, true] {
                let d = lane.descriptor(n, coverage);
                let compiled = lane
                    .device
                    .compile_device()
                    .compile_render_pipeline(&d, &Budget::standard())
                    .unwrap_or_else(|e| panic!("{n} samples: {e}"));
                let noted = compiled.stages.iter().any(|s| {
                    s.precision_notes
                        .contains(&PrecisionNote::DeviceAlphaCoverage)
                });
                let requires = compiled
                    .requirements
                    .properties
                    .contains(&VkProperty::MetalAlphaCoverage);
                assert_eq!(
                    (noted, requires),
                    (coverage && !metal, coverage && metal),
                    "{name} {n} samples, alpha to coverage {coverage}: the pipeline records the \
                     note exactly where it covers by alpha off the measured profile, and \
                     requires the property in its place on it"
                );
                messages.take_notices();
                let got = lane.draw(&d, n, &alphas);
                let announced = counts(&messages.take_notices());
                assert_eq!(
                    announced.contains_key(&Degradation::DeviceAlphaCoverage),
                    noted,
                    "{name} {n} samples, alpha to coverage {coverage}: {announced:?}"
                );
                if !coverage {
                    let partial = got.iter().flatten().filter(|&&k| k != n).count();
                    assert_eq!(partial, 0, "{name} {n} samples without alpha to coverage");
                    continue;
                }
                let (mut pixels, mut between, mut off, mut most) = (0usize, 0usize, 0usize, 0u32);
                let (mut alphas_off, mut first) = (0usize, None);
                for (&alpha, ks) in alphas.iter().zip(&got) {
                    let x = f64::from(alpha) * f64::from(n);
                    let theirs = reading.get(&(n, alpha.to_bits()));
                    let mut apart = false;
                    for (p, &k) in ks.iter().enumerate() {
                        pixels += 1;
                        between += usize::from((x.floor()..=x.ceil()).contains(&f64::from(k)));
                        let Some(&want) = theirs.map(|block| &block[p]) else {
                            continue;
                        };
                        if k != want {
                            off += 1;
                            apart = true;
                            most = most.max(k.abs_diff(want));
                            first.get_or_insert((alpha, p, k, want));
                            if metal {
                                wrong.push(format!(
                                    "{n} samples, alpha {alpha:e}, pixel {p} of its block: {k} \
                                     covered, Metal's {want}"
                                ));
                            }
                        }
                    }
                    alphas_off += usize::from(apart);
                }
                let against = match read.contains(&n) {
                    false => "no reading of Metal's at this count".to_owned(),
                    true => format!(
                        "{off} cover another count than Metal's, at {alphas_off} alphas{}",
                        match first {
                            Some((alpha, p, k, want)) => format!(
                                " (at most {most} samples apart; first alpha {alpha:e}, pixel \
                                 {p}: {k}, Metal's {want})"
                            ),
                            None => String::new(),
                        }
                    ),
                };
                println!(
                    "A2C {name}: {n} samples, {} alphas, {pixels} pixels: {against}; {between} \
                     cover floor(alpha * n) or ceil(alpha * n)",
                    alphas.len()
                );
            }
        }
        for line in wrong.iter().take(32) {
            println!("A2C-DIFFERS {name}: {line}");
        }
        assert!(
            wrong.is_empty(),
            "{name}: {} pixels cover another count than Metal's on a profile measured to cover \
             as Metal does",
            wrong.len()
        );
    }
    layer_reported_nothing_at_all(&messages);
}
