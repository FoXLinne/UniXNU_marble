//! Metal's level rule: the level a nearest mip filter reads, against the probe's answers, and
//! `word_level` on the interpreter against the host's rule.

use crate::float16::{f16_bits_to_f32, f32_to_f16_bits};
use crate::pipeline::sym::{ResourceKind, ResourceSlot, Stage};
use crate::spirv::builder::{FunctionBuilder, ModuleBuilder};
use crate::spirv::error::BResult;
use crate::spirv::ids::{GlobalId, TypeId, Value};
use crate::spirv::interp::{self, Buffer, Config, Resources};
use crate::spirv::ir::{
    natural_layout, BinOp, ConvOp, ExecMode, ExecModel, IoDir, LayoutRule, MemAccess, StorageClass,
    Type, TypedModule,
};

/// Our probe's `r32Float` 2D texture of five levels, level `L` holding `L`: rows of (level
/// `x`'s bits; the nearest-mip and linear-mip samples; and those of samplers clamping the level
/// into `[0.75, 2.5]`, linear then nearest), Apple M4 on macOS 27.0.1. A linear mip filter's
/// sample is `L + q`, a nearest one's the level it reads.
const MEASURED: [(u32, f32, f32, f32, f32); 23] = [
    (0x3f00_0000, 0.0, 0.5, 0.75, 1.0),
    (0x3f02_0000, 0.0, 0.5, 0.75, 1.0),
    (0x3f04_0000, 1.0, 0.515_625, 0.75, 1.0),
    (0x3fc1_0000, 1.0, 1.5, 1.5, 1.0),
    (0x3fc2_0000, 2.0, 1.515_625, 1.515_625, 2.0),
    (0x3f80_1000, 1.0, 1.0, 1.0, 1.0),
    (0x401f_8000, 2.0, 2.484_375, 2.484_375, 2.0),
    (0x3c80_0000, 0.0, 0.015_625, 0.75, 1.0),
    (0x3c7f_ffff, 0.0, 0.015_625, 0.75, 1.0),
    (0xbf00_0000, 0.0, 0.0, 0.75, 1.0),
    (0x4090_0000, 4.0, 4.0, 2.5, 2.0),
    (0x42c8_0000, 4.0, 4.0, 2.5, 2.0),
    (0x7f80_0000, 4.0, 4.0, 2.5, 2.0),
    (0x7fc0_0000, 0.0, 0.0, 0.75, 1.0),
    (0x3f03_dfff, 0.0, 0.5, 0.75, 1.0),
    (0x3f03_e000, 0.0, 0.5, 0.75, 1.0),
    (0x3f03_e001, 0.0, 0.5, 0.75, 1.0),
    (0x3f03_ffff, 1.0, 0.515_625, 0.75, 1.0),
    (0x3fc1_efff, 1.0, 1.5, 1.5, 1.0),
    (0x3fc1_f000, 2.0, 1.515_625, 1.515_625, 2.0),
    (0x3fc1_f001, 2.0, 1.515_625, 1.515_625, 2.0),
    (0x3fc1_ffff, 2.0, 1.515_625, 1.515_625, 2.0),
    (0x0000_0001, 0.0, 0.0, 0.75, 1.0),
];

/// The level a nearest mip filter reads at level `x` before the sampler's clamp, by Metal's rule:
/// `x` rounded to `half` (a NaN reads 0), clamped into `[0, 64]`, and `L + 1` where its fraction
/// truncated onto sixty-fourths is above one half, else `L`.
fn nearest_rule(x: f32) -> f32 {
    let h = if x.is_nan() {
        0.0
    } else {
        f16_bits_to_f32(f32_to_f16_bits(x))
    };
    let h = h.clamp(0.0, 64.0);
    let l = h.floor();
    let q = ((h - l) * 64.0).floor() / 64.0;
    l + if q > 0.5 { 1.0 } else { 0.0 }
}

/// The rule reads the probe's nearest-mip levels of a texture of five levels.
#[test]
fn the_rule_reads_the_measured_nearest_levels() {
    for &(bits, nearest, ..) in &MEASURED {
        let x = f32::from_bits(bits);
        assert_eq!(nearest_rule(x).min(4.0), nearest, "{x:e} ({bits:#010x})");
    }
}

fn slot(index: u32) -> ResourceSlot {
    ResourceSlot {
        stage: Stage::Kernel,
        kind: ResourceKind::Buffer,
        index,
    }
}

fn storage_buffer(b: &mut ModuleBuilder, index: u32, elem: TypeId) -> BResult<GlobalId> {
    let rta = b.ty(Type::RuntimeArray {
        elem,
        stride: Some(4),
    })?;
    let layout = natural_layout(b.types(), LayoutRule::Std430, true, &[rta])?;
    let block = b.ty(Type::Struct {
        members: [rta].into(),
        layout: Some(layout),
    })?;
    b.resource(slot(index), StorageClass::StorageBuffer, block)
}

/// Word `k` of invocation `i`'s record of `per` words in `buf`.
fn word(f: &mut FunctionBuilder<'_>, buf: GlobalId, i: Value, per: u32, k: u32) -> BResult<Value> {
    let per = f.u32_const(per)?;
    let per = f.module().const_value(per)?;
    let at = f.binary(BinOp::IMul, i, per)?;
    let k = f.u32_const(k)?;
    let k = f.module().const_value(k)?;
    let at = f.binary(BinOp::IAdd, at, k)?;
    let zero = f.u32_const(0)?;
    let zero = f.module().const_value(zero)?;
    let base = f.module().global_value(buf)?;
    f.access_chain(base, &[zero, at])
}

/// A kernel whose invocation `i` reads `(bits(x), word)` from buffer 0 and writes `word_level`'s
/// level of it to buffer 1.
fn word_kernel() -> BResult<TypedModule> {
    let mut b = ModuleBuilder::new();
    let f32_ = b.ty(Type::Float { width: 32 })?;
    let u32_ = b.ty(Type::Int {
        width: 32,
        signed: false,
    })?;
    let uvec3 = b.ty(Type::Vector { elem: u32_, len: 3 })?;
    let input = storage_buffer(&mut b, 0, u32_)?;
    let output = storage_buffer(&mut b, 1, u32_)?;
    let id = b.builtin(crate::spv::BuiltIn::GlobalInvocationId, IoDir::Input, uvec3)?;
    let void = b.ty(Type::Void)?;
    let mut f = b.function(void)?;
    let gid = f.module().global_value(id)?;
    let gid = f.load(gid, MemAccess::default())?;
    let i = f.composite_extract(gid, &[0])?;
    let at = word(&mut f, input, i, 2, 0)?;
    let xb = f.load(at, MemAccess::default())?;
    let x = f.convert(ConvOp::Bitcast, xb, f32_)?;
    let at = word(&mut f, input, i, 2, 1)?;
    let w = f.load(at, MemAccess::default())?;
    let level = super::word_level(&mut f, w, Some(x))?;
    let at = word(&mut f, output, i, 1, 0)?;
    f.store(at, level, MemAccess::default())?;
    f.ret(None)?;
    let main = f.finish()?;
    b.entry(
        ExecModel::GLCompute,
        main,
        vec![ExecMode::LocalSize { x: 1, y: 1, z: 1 }],
    )?;
    b.finalize()
}

/// The levels the rule is checked at: the measured ones, every `1/256` from -2 to 18, each `half`
/// of `[1/4, 6)` with its tie to the next and the tie's `f32` neighbours, and the edges.
fn levels() -> Vec<u32> {
    let mut out: Vec<u32> = MEASURED.iter().map(|m| m.0).collect();
    out.extend((-512..=4608).map(|k| (k as f32 / 256.0).to_bits()));
    for h in 0x3400u16..0x4600 {
        let (v, next) = (f16_bits_to_f32(h), f16_bits_to_f32(h + 1));
        // Exact: two neighbouring halves' midpoint has 12 significant bits.
        let tie = ((v + next) / 2.0).to_bits();
        out.extend([v.to_bits(), tie - 1, tie, tie + 1]);
    }
    out.extend([
        0x7f80_0000,
        0xff80_0000,
        0x7fc0_0000,
        0x8000_0000,
        0x477f_e000,
        0x477f_f000,
        0x4780_0000,
    ]);
    out
}

/// The level a runtime sampler's nearest mip filter reads before the texture's levels clamp it
/// (`snap_level`), as the host computes it.
fn snap_level_of(word: u32, nearest: f32) -> u32 {
    use crate::metal::sampler::{SamplerWord as W, LAST_WORD_LEVEL};
    if word & W::MIP_FILTER == 0 {
        return 0;
    }
    let min = (word >> W::LEVEL_MIN_SHIFT) & 0xf;
    let max = LAST_WORD_LEVEL - ((word >> W::LEVEL_MAX_SHIFT) & 0xf);
    (nearest as u32).max(min).min(max)
}

/// `word_level` in the function is the host's rule, bit for bit, for words of every mip filter,
/// filters alike and not, coordinates, anisotropy and clamp levels, at every level of [`levels`].
#[test]
fn the_word_level_computes_metals_level_in_the_function() {
    use crate::metal::sampler::SamplerWord as W;
    let clamp = |min: u32, max: u32| {
        (min << W::LEVEL_MIN_SHIFT)
            | ((crate::metal::sampler::LAST_WORD_LEVEL - max) << W::LEVEL_MAX_SHIFT)
    };
    let mut words = Vec::new();
    for mip in 0..3u32 {
        for filters in [
            0,
            W::MIN_LINEAR,
            W::MAG_LINEAR,
            W::MIN_LINEAR | W::MAG_LINEAR,
        ] {
            for extra in [0, W::PIXEL_COORDINATES, W::ANISOTROPIC] {
                for (min, max) in [(0, 15), (1, 2), (3, 3)] {
                    words.push((mip << W::MIP_FILTER_SHIFT) | filters | extra | clamp(min, max));
                }
            }
        }
    }
    let xs = levels();
    let step = (xs.len() / 600).max(1);
    let xs: Vec<u32> = xs.into_iter().step_by(step).collect();
    let lanes: Vec<(u32, u32)> = words
        .iter()
        .flat_map(|&w| xs.iter().map(move |&x| (x, w)))
        .collect();
    let m = word_kernel().expect("kernel");
    let bytes: Vec<u8> = lanes
        .iter()
        .flat_map(|(x, w)| [x.to_le_bytes(), w.to_le_bytes()])
        .flatten()
        .collect();
    let res = Resources {
        buffers: vec![
            Buffer {
                slot: Some(slot(0)),
                address: None,
                bytes,
            },
            Buffer {
                slot: Some(slot(1)),
                address: None,
                bytes: vec![0xAB; lanes.len() * 4],
            },
        ],
        ..Resources::default()
    };
    let cfg = Config {
        workgroups: [u32::try_from(lanes.len()).expect("lanes"), 1, 1],
        ..Config::default()
    };
    let (out, _) = interp::run_with_images(&m, &cfg, res, Vec::new()).expect("run");
    let mut wrong = Vec::new();
    for (i, &(b, w)) in lanes.iter().enumerate() {
        let x = f32::from_bits(b);
        let at = 4 * i;
        let got = u32::from_le_bytes(out.buffers[1][at..at + 4].try_into().expect("word"));
        let want = snap_level_of(w, nearest_rule(x));
        if got != want {
            wrong.push(format!(
                "word {w:#x} at {x:e} ({b:#010x}): {got}, want {want}"
            ));
        }
    }
    assert!(
        wrong.is_empty(),
        "{} differ:\n{}",
        wrong.len(),
        wrong[..wrong.len().min(40)].join("\n")
    );
    assert!(lanes.len() > 50_000, "{}", lanes.len());
}
