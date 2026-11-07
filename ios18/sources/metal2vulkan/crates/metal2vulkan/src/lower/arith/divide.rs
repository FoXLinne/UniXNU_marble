//! Metal's precise division, by the device's `OpFDiv`.
//!
//! Metal's `float` division without `arcp` (a flagless `fdiv`, `precise::divide`, and the plain
//! operator under `-fno-fast-math`) answers the quotient rounded once to nearest even (a probe
//! cases 914cc4d2 and 711cc190, every lane), and its `half` division under every flag set the
//! quotient rounded once from the exact one (4c363561, 54f0fe8e). Vulkan bounds `OpFDiv` at 2.5
//! ulp for a divisor in `[2^-126, 2^126]` and not at all beyond, and devices use that latitude:
//! RADV answers `3 / 3` with `0x3f7fffff` and a divisor past `2^126` with a zero. Every such
//! division takes the device's `OpFDiv` ([`native`]), its divisor kept in Vulkan's range, within
//! Vulkan's bound (`PrecisionNote::NativeDivision`).

use crate::air::ir::{BinOp as AirBin, FastMath};
use crate::lower::arith::legal::{FloatFmt, Form};
use crate::lower::arith::narrow;
use crate::lower::arith::ops::fsplat;
use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::{BResult, BuildError, TypeRule};
use crate::spirv::ids::Value;
use crate::spirv::ir::BinOp;

/// Whether the float binary operator `op` with fast-math flags `fmf` on format `fmt` is Metal's
/// precise division: an `fdiv` on `float` without `arcp`, or on `half` under every flag set (the
/// module's comment).
pub fn precise(op: AirBin, fmt: FloatFmt, fmf: FastMath) -> bool {
    op == AirBin::FDiv
        && match fmt {
            FloatFmt::F32 => !fmf.contains(FastMath::ARCP),
            FloatFmt::Half => true,
            FloatFmt::BFloat | FloatFmt::F64 => false,
        }
}

/// The census name of a precise division ([`native`]).
pub const NATIVE: &str = "arith.fdiv.native";

/// An upper estimate of the SPIR-V words of one [`native`] division on form `f`: the divisor's
/// magnitude, its test, the scale's select, two products, the division, and a `half` result's
/// rounding.
pub fn native_words(f: Form) -> u32 {
    96 + 16 * u32::from(f.lanes)
}

/// Metal's precise division `x / y` on `float` or `half` lanes (form `f`): the device's `OpFDiv`,
/// within Vulkan's 2.5 ulp for a divisor in `[2^-126, 2^126]`, the operands of a divisor past
/// `2^126` first scaled by `2^-2` into that range (exactly, but where the dividend becomes a
/// denormal, whose quotient by such a divisor Metal flushes to zero anyway); a `half` quotient
/// rounded to `half` (`PrecisionNote::NativeDivision`).
pub fn native(fb: &mut FunctionBuilder<'_>, f: Form, x: Value, y: Value) -> BResult<Value> {
    let ay = fb.glsl(crate::spirv::ir::GlslOp::FAbs, &[y])?;
    let limit = fsplat(fb, f, 2f64.powi(126))?;
    let past = fb.binary(BinOp::FOrdGreaterThan, ay, limit)?;
    let (quarter, one) = (fsplat(fb, f, 0.25)?, fsplat(fb, f, 1.0)?);
    let scale = fb.select(past, quarter, one)?;
    let x = fb.binary(BinOp::FMul, x, scale)?;
    let y = fb.binary(BinOp::FMul, y, scale)?;
    let q = fb.binary(BinOp::FDiv, x, y)?;
    match f.float_fmt() {
        Some(FloatFmt::F32) => Ok(q),
        Some(FloatFmt::Half) => narrow::round(fb, f, q),
        _ => Err(BuildError::Type(TypeRule::FunctionSignature)),
    }
}
