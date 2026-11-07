//! The cube face and texel Metal reads along a direction.
//!
//! A cube sample, gather, or level-of-detail query reads the face of its direction's major axis,
//! the component of largest magnitude, at the face coordinates `s_c / |m_a|` and `t_c / |m_a|`.
//! Vulkan leaves two things there to the device that Metal decides its own way (Apple M4, macOS
//! 27.0.1, rgba32_float cubes of 8x8 and 5x5 faces, nearest samples; each run with and without
//! Metal's validation layer, byte-identical):
//!
//! - **The face at a tie.** Where two or three components share the largest magnitude, Vulkan's
//!   "Cube Map Face Selection" only recommends `z` over `y` over `x`; Metal picks `x` over `y`
//!   over `z`. Every one of 834 lanes, each pair and the triple of tied axes in every sign
//!   combination at magnitudes 1, 0.885, 0.5 and 3, read that face, and each control with one
//!   tied component an ulp larger read that component's face; so did ties at `2^-125`, `2^-124`,
//!   `2^-100`, `10^-30`, and `2^100`. Authored case `914ba505`'s lane 239, `(-1, -0.885, -1)`,
//!   read the `-z` face on five Vulkan devices where Metal reads `-x`.
//! - **The ends of the range.** Metal reads a subnormal component as zero when it selects the
//!   face, and a direction left zero, every component zero or subnormal in any signs (magnitudes
//!   `2^-149`, `2^-140`, `2^-127`, the largest subnormal), at face `+x`'s texel `(0, 0)`, where
//!   Vulkan divides by zero. It reads a face coordinate as zero where its component is below
//!   `2^-125` (a minor `2^-126`, `1.5 * 2^-126`, or a subnormal one beside a major `2^-125` or
//!   `2^-120` read the face's centre texel, and a tie at `2^-126` or the next float above it too)
//!   or where the major magnitude is above `2^126` (ties at `2^127` and at the largest float read
//!   the centre; `2^125` beside `2^126` reads exactly).
//!
//! `metal_ties` hands the device a direction that reads Metal's face on every device, and its
//! texel wherever the device rounds the face coordinate as Metal does (below), Apple's texture
//! unit (which MoltenVK samples through) included: each minor component Metal
//! reads as zero is `+0`; a direction left zero is `(1, 1 - 2^-24, 1 - 2^-24)`, which reads face
//! `+x` at texel `(0, 0)` on every face size below `2^24`; and each minor component tying the
//! major magnitude otherwise is one ulp smaller in magnitude (its bits less one), so no tie is
//! left for a device to break. At a tie at `2^-125` itself the shrunk component would fall below
//! `2^-125`, where Apple's texture unit reads it as zero (MoltenVK read the face's centre there),
//! so the major component grows one ulp in magnitude instead (its bits plus
//! one). On Metal's face a tied component is the edge coordinate `±1`, which becomes `±(1 - e)`
//! with `e <= 2^-23` either way: the texel the edge reads through a nearest sample on every face
//! size below `2^23`, and the footprint it reads through a gather or a linear filter on every size
//! below `2^15` (the position quantizes to 1/256 of a texel), above Metal's largest cube face,
//! 16,384. Every other component passes unchanged. A gather or linear sample at a zero direction,
//! like one aimed at any corner, reads a corner texel Vulkan leaves to the device (its answer,
//! `PrecisionNote::DeviceSampling` where its sampler is not Metal's).
//!
//! **A huge major magnitude.** Devices misread a direction whose major magnitude is near the top
//! of the range (the probe lanes, `wrg_cube_ties` at 8x8 and 5x5): RADV (AMD Radeon 780M)
//! read `(2^126, 2^125, 0)` at the face's centre, where Metal reads `t = 1/4`, as a reciprocal of
//! AMD's doubled major axis (`v_cubema`) flushed to zero would; ANV (Intel Iris Xe) and NVIDIA
//! (RTX 5080 laptop) read face `+z` along every direction with a major magnitude of `2^127` or
//! more (`(2^127, 0, 0)` and `(f32::MAX, 0, 0)` among them), and the major face at `2^126`. So a
//! direction whose major magnitude is above `2^125` is scaled by a power of two, `1/2` up to
//! `2^126` and `1/8` past it, where every minor component is already `+0`: the device divides by
//! at most `2^125`, and no quotient moves, since every component scaled by `1/2` that Metal reads
//! (at least `2^-125`) stays normal.
//!
//! A direction with an infinite or NaN component passes unchanged, and the device's answer
//! stands: no infinity becomes finite and no NaN moves. Metal's is measured and not reproduced:
//! it reads a face coordinate of `±inf / inf` at texel 0 and `(inf, 0, 0)` at the centre of `+x`;
//! its face is the first axis whose magnitude equals the largest non-NaN one (`z` where none
//! does), and a NaN face coordinate reads texel 0. A level of detail the device takes from the
//! direction's derivatives sees each broken tie as a face coordinate moved by at most `2^-24`
//! (`PrecisionNote::CubeGradientFootprint`).

use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::BResult;
use crate::spirv::ids::Value as SValue;
use crate::spirv::ir::{BinOp, ConstBits, Constant, ConvOp, GlslOp, Type as SType, UnOp};

/// The estimated words of `metal_ties`: its 89 instructions and 11 constants, at most 6 words each.
pub const TIE_WORDS: u32 = 600;

/// `1 - 2^-24`, the largest float below 1: a zero direction's `y` and `z`.
const BELOW_ONE: u32 = 0x3f7f_ffff;

/// `2^-125`: Metal reads a face coordinate's component below it as zero.
const LEAST_READ: u32 = 0x0100_0000;

/// `2^126`: past a major magnitude above it Metal reads every face coordinate as zero.
const MOST_READ: u32 = 0x7e80_0000;

/// `2^125`: the largest major magnitude the device is handed. AMD's cube instructions take the
/// major axis doubled (`v_cubema`), whose reciprocal is subnormal, and flushed, past it.
const MOST_DIVIDED: u32 = 0x7e00_0000;

/// The cube direction `p` (three floats) as Metal reads it, handed to the device so that every
/// device reads Metal's face and texel. A direction with an infinite or NaN component passes
/// unchanged. Otherwise Metal's major axis is the first of `x`, `y`, `z` whose magnitude, a
/// subnormal one read as zero, is the largest; a direction left zero is handed as
/// `(1, 1 - 2^-24, 1 - 2^-24)` (face `+x`, texel `(0, 0)`); each minor component is `+0` where
/// its magnitude is below `2^-125` or the major magnitude above `2^126`, else one ulp smaller in
/// magnitude where it ties the major magnitude; but at a tie at `2^-125` the major component is
/// one ulp larger instead; every other component passes unchanged. Then a direction whose major
/// magnitude is above `2^125` is scaled by `1/2`, or by `1/8` where it is above `2^126`.
pub(super) fn metal_ties(fb: &mut FunctionBuilder<'_>, p: SValue) -> BResult<SValue> {
    let f32_ = fb.module().ty(SType::Float { width: 32 })?;
    let u32_ = fb.module().ty(SType::Int {
        width: 32,
        signed: false,
    })?;
    let triple = fb.module().ty(SType::Vector { elem: f32_, len: 3 })?;
    let f32_const = |fb: &mut FunctionBuilder<'_>, x: f32| -> BResult<SValue> {
        let c = fb.module().constant(Constant::Scalar {
            ty: f32_,
            bits: ConstBits(u64::from(x.to_bits())),
        })?;
        fb.module().const_value(c)
    };
    let least = f32_const(fb, f32::MIN_POSITIVE)?;
    let most = f32_const(fb, f32::MAX)?;
    let least_read = f32_const(fb, f32::from_bits(LEAST_READ))?;
    let most_read = f32_const(fb, f32::from_bits(MOST_READ))?;
    let most_divided = f32_const(fb, f32::from_bits(MOST_DIVIDED))?;
    let zero = f32_const(fb, 0.0)?;
    let a = fb.glsl(GlslOp::FAbs, &[p])?;
    let cx = fb.composite_extract(p, &[0])?;
    let cy = fb.composite_extract(p, &[1])?;
    let cz = fb.composite_extract(p, &[2])?;
    let ax = fb.composite_extract(a, &[0])?;
    let ay = fb.composite_extract(a, &[1])?;
    let az = fb.composite_extract(a, &[2])?;
    // Every component finite (an infinity or a NaN fails the test): the direction is changed at
    // all only then.
    let finite_x = fb.binary(BinOp::FOrdLessThanEqual, ax, most)?;
    let finite_y = fb.binary(BinOp::FOrdLessThanEqual, ay, most)?;
    let finite_z = fb.binary(BinOp::FOrdLessThanEqual, az, most)?;
    let finite_xy = fb.binary(BinOp::LogicalAnd, finite_x, finite_y)?;
    let regular = fb.binary(BinOp::LogicalAnd, finite_xy, finite_z)?;
    // The magnitude Metal selects the face by: a subnormal one as zero.
    let selecting = |fb: &mut FunctionBuilder<'_>, ak: SValue| -> BResult<SValue> {
        let subnormal = fb.binary(BinOp::FOrdLessThan, ak, least)?;
        fb.select(subnormal, zero, ak)
    };
    let mx = selecting(fb, ax)?;
    let my = selecting(fb, ay)?;
    let mz = selecting(fb, az)?;
    // Metal's major axis: `x` over `y` over `z` at equal magnitudes.
    let x_y = fb.binary(BinOp::FOrdGreaterThanEqual, mx, my)?;
    let x_z = fb.binary(BinOp::FOrdGreaterThanEqual, mx, mz)?;
    let major_x = fb.binary(BinOp::LogicalAnd, x_y, x_z)?;
    let y_z = fb.binary(BinOp::FOrdGreaterThanEqual, my, mz)?;
    let z_y = fb.binary(BinOp::FOrdGreaterThan, mz, my)?;
    let minor_x = fb.unary(UnOp::LogicalNot, major_x)?;
    let minor_y = fb.binary(BinOp::LogicalOr, major_x, z_y)?;
    let minor_z = fb.binary(BinOp::LogicalOr, major_x, y_z)?;
    let yz = fb.select(y_z, my, mz)?;
    let top = fb.select(major_x, mx, yz)?;
    // Past a major magnitude above `2^126` Metal reads every face coordinate as zero.
    let beyond = fb.binary(BinOp::FOrdGreaterThan, top, most_read)?;
    // Whether Metal reads a minor component of magnitude `ak` as zero.
    let zeroed = |fb: &mut FunctionBuilder<'_>, ak: SValue, minor: SValue| -> BResult<SValue> {
        let small = fb.binary(BinOp::FOrdLessThan, ak, least_read)?;
        let drop = fb.binary(BinOp::LogicalOr, small, beyond)?;
        let dropped = fb.binary(BinOp::LogicalAnd, minor, drop)?;
        fb.binary(BinOp::LogicalAnd, regular, dropped)
    };
    let zeroed_x = zeroed(fb, ax, minor_x)?;
    let zeroed_y = zeroed(fb, ay, minor_y)?;
    let zeroed_z = zeroed(fb, az, minor_z)?;
    // A minor component tying the major magnitude: `y` beside a major `x`, `z` beside either.
    let tie = |fb: &mut FunctionBuilder<'_>, ak: SValue, beside: SValue| -> BResult<SValue> {
        let equal = fb.binary(BinOp::FOrdEqual, ak, top)?;
        let tied = fb.binary(BinOp::LogicalAnd, beside, equal)?;
        fb.binary(BinOp::LogicalAnd, regular, tied)
    };
    let tie_y = tie(fb, ay, major_x)?;
    let tie_z = tie(fb, az, minor_z)?;
    // A tied component shrinks where its magnitude stays at least `2^-125`. At a tie at `2^-125`
    // itself it would fall below, where Apple's texture unit reads it as zero (MoltenVK samples
    // there), so the major component grows one ulp in magnitude instead.
    let above = fb.binary(BinOp::FOrdGreaterThan, top, least_read)?;
    let shrink_y = fb.binary(BinOp::LogicalAnd, tie_y, above)?;
    let shrink_z = fb.binary(BinOp::LogicalAnd, tie_z, above)?;
    let at_least = fb.binary(BinOp::FOrdEqual, top, least_read)?;
    let any_tie = fb.binary(BinOp::LogicalOr, tie_y, tie_z)?;
    let grow = fb.binary(BinOp::LogicalAnd, at_least, any_tie)?;
    let grow_x = fb.binary(BinOp::LogicalAnd, grow, major_x)?;
    let major_y = fb.binary(BinOp::LogicalAnd, minor_x, y_z)?;
    let grow_y = fb.binary(BinOp::LogicalAnd, grow, major_y)?;
    let one = fb.u32_const(1)?;
    let one = fb.module().const_value(one)?;
    // A normal float's bits less one are its magnitude one ulp smaller, and its bits plus one
    // (below the largest float) one ulp larger, its sign kept either way.
    let step = |fb: &mut FunctionBuilder<'_>, bits: SValue, op: BinOp| -> BResult<SValue> {
        let stepped = fb.binary(op, bits, one)?;
        fb.convert(ConvOp::Bitcast, stepped, f32_)
    };
    let bits_x = fb.convert(ConvOp::Bitcast, cx, u32_)?;
    let bits_y = fb.convert(ConvOp::Bitcast, cy, u32_)?;
    let bits_z = fb.convert(ConvOp::Bitcast, cz, u32_)?;
    let grown_x = step(fb, bits_x, BinOp::IAdd)?;
    let x = fb.select(grow_x, grown_x, cx)?;
    let x = fb.select(zeroed_x, zero, x)?;
    let grown_y = step(fb, bits_y, BinOp::IAdd)?;
    let shrunk_y = step(fb, bits_y, BinOp::ISub)?;
    let y = fb.select(grow_y, grown_y, cy)?;
    let y = fb.select(shrink_y, shrunk_y, y)?;
    let y = fb.select(zeroed_y, zero, y)?;
    let shrunk_z = step(fb, bits_z, BinOp::ISub)?;
    let z = fb.select(shrink_z, shrunk_z, cz)?;
    let z = fb.select(zeroed_z, zero, z)?;
    // A direction left zero reads face `+x`'s texel `(0, 0)`.
    let empty = fb.binary(BinOp::FOrdEqual, top, zero)?;
    let nothing = fb.binary(BinOp::LogicalAnd, regular, empty)?;
    let unit = f32_const(fb, 1.0)?;
    let below_one = f32_const(fb, f32::from_bits(BELOW_ONE))?;
    let x = fb.select(nothing, unit, x)?;
    let y = fb.select(nothing, below_one, y)?;
    let z = fb.select(nothing, below_one, z)?;
    // A major magnitude past `2^125` is scaled down by a power of two, `1/2` up to `2^126` and
    // `1/8` past it (every minor component is `+0` there), so that the device divides by at most
    // `2^125`; no quotient moves.
    let huge = fb.binary(BinOp::FOrdGreaterThan, top, most_divided)?;
    let huge = fb.binary(BinOp::LogicalAnd, regular, huge)?;
    let half = f32_const(fb, 0.5)?;
    let eighth = f32_const(fb, 0.125)?;
    let scale = fb.select(beyond, eighth, half)?;
    let scaled = |fb: &mut FunctionBuilder<'_>, c: SValue| -> BResult<SValue> {
        let product = fb.binary(BinOp::FMul, c, scale)?;
        fb.select(huge, product, c)
    };
    let x = scaled(fb, x)?;
    let y = scaled(fb, y)?;
    let z = scaled(fb, z)?;
    fb.composite_construct(triple, &[x, y, z])
}

#[cfg(test)]
mod tests;
