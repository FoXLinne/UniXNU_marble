//! Metal's level of a sample with an explicit level (`level(x)`), or a kernel's bias (`bias(x)`,
//! the explicit level `x` outside a fragment function), computed in the function where the module
//! reads the texels itself (an extended-range texture's sample, `extended`).
//!
//! **The rule** (Apple M4, macOS 27.0.1; for comparisons of a mipmapped `depth32Float` texture, for
//! colour samples). Metal rounds the level `x` to `half`, to nearest even (a NaN reads 0), clamps
//! it into the sampler's level clamp (whose bounds a constexpr sampler holds as `half`s), and into
//! the texture's levels. With `L = floor(x)` and the fraction truncated onto sixty-fourths, `q =
//! floor(64 (x - L)) / 64`:
//!
//! - a nearest mip filter reads level `L + 1` where `q > 1/2`, else level `L`;
//! - a linear mip filter blends levels `L` and `L + 1` with weights `1 - q` and `q`;
//! - a sampler without a mip filter reads level 0;
//! - every sampler magnifies (its magnification filter) where `L + q` is 0, and minifies
//!   elsewhere.
//!
//! Every lane of the probes reads so: 2,230 levels each (every sixty-fourth from -1 to 5, the
//! neighbourhoods of `L + 1/2` in steps of `1/4096`, every `1/1024` from 1 to 2, the `half` ties
//! around `L + k/64` with their `f32` neighbours, past the last level, the infinities and a NaN)
//! through nearest and linear filters under nearest and linear mip filters, of `r32Float`,
//! `rgba16Float` and `rgba8Unorm` 2D textures of five levels (the level and weight; an 8-bit
//! format's blend itself rounds onto sixteenths of a code, which the device's filter answers under
//! the sampler's note), and of `r32Float` 3D, cube and 2D-array textures; as a bias in a kernel
//! alike; through samplers clamping the level into `[0.75, 2.5]` and `[0.3, 2.7]`; and where a
//! sampler minifies and magnifies with different filters (a 16x16 texture of distinct texels).
//! Metal's compiler builds no kernel that samples with `min_lod_clamp` (its device compiler
//! stops), so a clamp beside a bias is not measured; nor is a texture of one level, where the
//! clamp into its levels would decide the filter of a sampler whose filters differ.
//!
//! **A depth comparison** reads its level by the same rule through a runtime, an argument
//! buffer's, and a constexpr sampler alike (2,048,400 comparisons at 1,707 levels through nearest
//! filters under nearest, linear and no mip filters and eleven clamps, a runtime or argument
//! buffer's sampler's as `f32`s); a linear mip filter blends the two levels' comparisons as an
//! `r8Unorm` texture's codes 0 and 255, rounded once onto sixteenths of a code, ties up
//! (`Depth32Float` and `Depth16Unorm`, one level, both, or neither passing). A comparison the
//! device makes takes the device's level.
//!
//! **The device** chooses its level from `λ` (Vulkan's explicit `Lod`), clamped into the
//! sampler's `[minLod, maxLod]` (the same level clamp), at its own precision: a nearest mip filter
//! reads `ceil(λ + 1/2) - 1` (rounding `L + 1/2` either way is the implementation's), a linear one
//! blends `floor(λ)` and the next level by `λ`'s fraction to `mipmapPrecisionBits` (at least 6 on
//! the measured venues), and `λ <= 0` magnifies. A sample the device makes takes the device's
//! level (`PrecisionNote::DeviceSampling` on a device whose sampler is not Metal's).
//!
//! **A runtime or argument buffer's sampler** holds its clamp as `f32`s, not `half`s (Apple M4 on
//! macOS 27.0.1: all 37,080 lanes of `level(x)` and a kernel's `bias(x)` through eighteen clamps,
//! nearest and linear filters and mip filters). The function reads a nearest mip filter's level
//! within the levels the word's clamp leaves ([`word_level`]), and a linear one's from the
//! clamp's minimum on Metal's grid (`SpecKey::SamplerMinLevel`). A sample the device makes is
//! clamped by the device into the `VkSampler`'s clamp, the descriptor's
//! (`metal::sampler::vk_sampler`).

use crate::lower::arith::legal::{FloatFmt, Form, Held};
use crate::lower::arith::texel_round::{self, HalfRounding};
use crate::metal::sampler::SamplerWord;
use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::BResult;
use crate::spirv::ids::Value as SValue;
use crate::spirv::ir::{BinOp, ConstBits, Constant, GlslOp, Type as SType, UnOp};

#[cfg(test)]
mod tests;

/// The estimated words of a level on Metal's grid ([`clamped_on_grid`] and the mip filter's
/// level): the rounding to `half` (about 40 instructions of 4 to 6 words), the NaN's select, the
/// clamps, the floor, the fraction's grid, the mip filter's level, and their constants.
pub const LEVEL_WORDS: u32 = 400;

/// The cap on a level before its floor: past every texture's levels, and an `f32` whose fraction
/// is 0.
const CAP: f32 = 64.0;

/// Level `x` (an `f32`) rounded to `half` to nearest even, a NaN read as 0 (module docs).
fn half_level(fb: &mut FunctionBuilder<'_>, x: SValue) -> BResult<SValue> {
    let form = Form {
        held: Held::Float(FloatFmt::F32),
        lanes: 1,
    };
    let h = texel_round::f32_to_half(fb, form, x, HalfRounding::NearestEven)?;
    let zero = float(fb, 0.0)?;
    let nan = fb.unary(UnOp::IsNan, x)?;
    fb.select(nan, zero, h)
}

/// Level `x` as Metal reads it before the sampler's clamp, on its grid, in whole levels and
/// sixty-fourths `(L, q)` (module docs): rounded to `half`, a NaN 0, kept within `[0, 64]`.
fn on_grid(fb: &mut FunctionBuilder<'_>, x: SValue) -> BResult<(SValue, SValue)> {
    let h = half_level(fb, x)?;
    let (zero, cap) = (float(fb, 0.0)?, float(fb, CAP)?);
    let h = fb.glsl(GlslOp::FMax, &[h, zero])?;
    let h = fb.glsl(GlslOp::FMin, &[h, cap])?;
    let l = fb.glsl(GlslOp::Floor, &[h])?;
    let f = fb.binary_no_contraction(BinOp::FSub, h, l)?;
    let (sixty_four, step) = (float(fb, 64.0)?, float(fb, 1.0 / 64.0)?);
    let f = fb.binary_no_contraction(BinOp::FMul, f, sixty_four)?;
    let f = fb.glsl(GlslOp::Floor, &[f])?;
    let q = fb.binary_no_contraction(BinOp::FMul, f, step)?;
    Ok((l, q))
}

/// Level `x` (an `f32`) as Metal reads it, on its grid, in whole levels and sixty-fourths `(L,
/// q)` (module docs): rounded to `half` (a NaN 0), clamped into the sampler's clamp `[min, max]`
/// (`f32`s; `None`: no maximum) and into the texture's levels `[0, last]` (`last` an `f32`).
pub(super) fn clamped_on_grid(
    fb: &mut FunctionBuilder<'_>,
    x: SValue,
    (min, max): (SValue, Option<SValue>),
    last: SValue,
) -> BResult<(SValue, SValue)> {
    let h = half_level(fb, x)?;
    let h = fb.glsl(GlslOp::FMax, &[h, min])?;
    let h = match max {
        Some(max) => fb.glsl(GlslOp::FMin, &[h, max])?,
        None => h,
    };
    let zero = float(fb, 0.0)?;
    let h = fb.glsl(GlslOp::FMax, &[h, zero])?;
    let h = fb.glsl(GlslOp::FMin, &[h, last])?;
    let l = fb.glsl(GlslOp::Floor, &[h])?;
    let f = fb.binary_no_contraction(BinOp::FSub, h, l)?;
    let (sixty_four, step) = (float(fb, 64.0)?, float(fb, 1.0 / 64.0)?);
    let f = fb.binary_no_contraction(BinOp::FMul, f, sixty_four)?;
    let f = fb.glsl(GlslOp::Floor, &[f])?;
    let q = fb.binary_no_contraction(BinOp::FMul, f, step)?;
    Ok((l, q))
}

/// The level a nearest mip filter reads at level `x` before the sampler's clamp (module docs):
/// `L + 1` where `q > 1/2`, else `L`, on [`on_grid`]'s `(L, q)`.
pub(super) fn nearest_on_grid(
    fb: &mut FunctionBuilder<'_>,
    (l, q): (SValue, SValue),
) -> BResult<SValue> {
    let (zero, half, one) = (float(fb, 0.0)?, float(fb, 0.5)?, float(fb, 1.0)?);
    let next = fb.binary(BinOp::FOrdGreaterThan, q, half)?;
    let up = fb.select(next, one, zero)?;
    fb.binary_no_contraction(BinOp::FAdd, l, up)
}

/// The level a runtime sampler's nearest sample reads (a `u32`) before the texture's levels clamp
/// it, by its word `word`, where the sample's level before the sampler's clamp reads `nearest`
/// (an `f32`, [`nearest_on_grid`]'s, or 0 at level 0) under a nearest mip filter: that level clamped into
/// the levels the word's clamp leaves ([`SamplerWord::LEVEL_MIN`], [`SamplerWord::LEVEL_MAX`])
/// under a nearest mip filter, else 0, the one level a sampler without a mip filter reads.
pub(super) fn snap_level(
    fb: &mut FunctionBuilder<'_>,
    word: SValue,
    nearest: SValue,
) -> BResult<SValue> {
    let u32_ = fb.module().ty(SType::Int {
        width: 32,
        signed: false,
    })?;
    let field = |fb: &mut FunctionBuilder<'_>, shift: u32| -> BResult<SValue> {
        let s = word_const(fb, shift)?;
        let v = fb.binary(BinOp::ShiftRightLogical, word, s)?;
        let m = word_const(fb, 0xf)?;
        fb.binary(BinOp::BitwiseAnd, v, m)
    };
    let min = field(fb, SamplerWord::LEVEL_MIN_SHIFT)?;
    let below = field(fb, SamplerWord::LEVEL_MAX_SHIFT)?;
    let last = word_const(fb, crate::metal::sampler::LAST_WORD_LEVEL)?;
    let max = fb.binary(BinOp::ISub, last, below)?;
    let n = fb.convert(crate::spirv::ir::ConvOp::ConvertFToU, nearest, u32_)?;
    let n = fb.glsl(GlslOp::UMax, &[n, min])?;
    let n = fb.glsl(GlslOp::UMin, &[n, max])?;
    let mip = word_const(fb, SamplerWord::MIP_FILTER)?;
    let mip = fb.binary(BinOp::BitwiseAnd, word, mip)?;
    let zero = word_const(fb, 0)?;
    let none = fb.binary(BinOp::IEqual, mip, zero)?;
    fb.select(none, zero, n)
}

/// The level a runtime or argument buffer's sampler's sample reads at level `x` (an `f32`; `None`:
/// level 0, a kernel's sample without a level or bias), by its word `word` (a `u32`), before the
/// texture's levels clamp it (a `u32`, [`snap_level`] of the level a nearest mip filter reads at
/// `x`): under a nearest mip filter the level Metal's rule reads, clamped into the levels the
/// word's clamp leaves, and level 0 without a mip filter. Metal reads a comparison's level so too.
pub(super) fn word_level(
    fb: &mut FunctionBuilder<'_>,
    word: SValue,
    x: Option<SValue>,
) -> BResult<SValue> {
    let nearest = match x {
        Some(x) => {
            let grid = on_grid(fb, x)?;
            nearest_on_grid(fb, grid)?
        }
        None => float(fb, 0.0)?,
    };
    snap_level(fb, word, nearest)
}

/// The `u32` constant `v`.
fn word_const(fb: &mut FunctionBuilder<'_>, v: u32) -> BResult<SValue> {
    let c = fb.u32_const(v)?;
    fb.module().const_value(c)
}

/// The `f32` constant `v`.
fn float(fb: &mut FunctionBuilder<'_>, v: f32) -> BResult<SValue> {
    let ty = fb.module().ty(SType::Float { width: 32 })?;
    let c = fb.module().constant(Constant::Scalar {
        ty,
        bits: ConstBits(u64::from(v.to_bits())),
    })?;
    fb.module().const_value(c)
}
