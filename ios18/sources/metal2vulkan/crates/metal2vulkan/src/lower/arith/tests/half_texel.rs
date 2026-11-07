//! Interpreter differential tests of the conversion of a texel to a `half` result on a device that
//! samples through Metal's own texture unit (`lower::texture::half`): every texel to nearest even
//! by the module's rounding (the device's native conversion), but a YCbCr field's one texel on the
//! grid `8192`, with ties away from zero. References are plain Rust. Each runs in every
//! [`QuantizeForm`]; the device's quantize ([`QuantizeForm::Device`]) may answer a `half` denormal
//! as its signed zero ([`quantize_flush_of`]). The exact tests below bound where the device's
//! nearest-even `half` of Metal's `float` texel is Metal's `half` (the conditions of
//! `PrecisionNote::HalfTexelConversion`, `texture::half_texels`).

use super::texel_round::half_of;
use super::*;
use crate::lower::arith::texel_round::HalfRounding;
use crate::lower::texture::half::{self, TexelLanes};
use crate::metal::texel_format::{texel_format, PackedTexels};
use crate::metal::PixelFormat as P;
use crate::spirv::builder::QuantizeForm;

/// The module roundings the routine is built under.
const FORMS: [QuantizeForm; 3] = [
    QuantizeForm::Device,
    QuantizeForm::Converted,
    QuantizeForm::Controlled,
];

fn v4() -> Form {
    vec(float(32), 4)
}

/// The routine at class `class` (an input) and `lanes`, in a module rounding to `half` by `q`.
fn body_lanes(
    lanes: TexelLanes,
    q: QuantizeForm,
) -> impl Fn(&mut FunctionBuilder<'_>, &[Value]) -> BResult<Value> {
    move |fb, x| {
        fb.module().set_quantize_form(q);
        let r = half::define(fb.module())?;
        let lanes = lanes.value(fb)?;
        half::convert(fb, r, x[0], x[1], lanes)
    }
}

/// Every lanes argument: one texel's, each gathered channel's, and a value that may be filtered.
fn every_lanes() -> Vec<TexelLanes> {
    let mut out = vec![TexelLanes::Texel, TexelLanes::Filtered];
    out.extend((0..4).map(TexelLanes::Gathered));
    out
}

/// Run the routine over `texels` at `class` and `lanes`, against `reference` of each lane's value,
/// in every [`QuantizeForm`] (a `half` denormal the device's quantize's zero or itself in
/// [`QuantizeForm::Device`]).
fn run_lanes(
    what: &str,
    class: u32,
    lanes: TexelLanes,
    texels: &[[u64; 4]],
    reference: &dyn Fn(u64) -> u64,
) {
    let cases: Vec<Case> = texels
        .iter()
        .map(|t| vec![t.to_vec(), vec![u64::from(class)]])
        .collect();
    for q in FORMS {
        check_accept(
            &format!("{what} {lanes:?} {q:?}"),
            &[v4(), int(32)],
            v4(),
            &body_lanes(lanes, q),
            &cases,
            &|c| Some(c[0].iter().map(|x| reference(*x)).collect()),
            &|g, w| texel_answers(q, g, w),
        );
    }
}

/// Whether `f32` bits `got` answer the `half` value `want` in a module rounding by `q`: exactly, a
/// NaN for a NaN, or, by the device's quantize, a denormal's signed zero.
fn texel_answers(q: QuantizeForm, got: u64, want: u64) -> bool {
    got == want
        || (is_nan(v4(), got) && is_nan(v4(), want))
        || (q == QuantizeForm::Device && quantize_flush_of(got, want))
}

/// `x` in groups of four lanes, the last padded with zeros.
fn quads(x: &[u64]) -> Vec<[u64; 4]> {
    x.chunks(4)
        .map(|c| {
            let mut q = [0u64; 4];
            q[..c.len()].copy_from_slice(c);
            q
        })
        .collect()
}

/// The treatment bits of `pf`.
fn bits(pf: P) -> u32 {
    texel_format(pf).0
}

/// `f32` bits of `x`'s `half` by `mode`, a NaN kept.
fn rounded(mode: HalfRounding) -> impl Fn(u64) -> u64 {
    move |x| {
        let f = f32::from_bits(x as u32);
        if f.is_nan() {
            x
        } else {
            u64::from(half_of(f, mode))
        }
    }
}

/// Every half below `2^-13` (the denormals and the least normal binade), random `f32` patterns of
/// every magnitude, and the rounding edges: the tie below the least denormal, the overflow, the
/// infinities, an `f32` denormal, and `-0`.
fn float_texels() -> Vec<u64> {
    let mut rng = Rng(0x00b1_f0b1_f5ee_d001);
    let mut out: Vec<u64> = (0u32..0x800)
        .map(|h| u64::from((f16_to_f64(h as u16) as f32).to_bits()))
        .collect();
    out.extend((0..4000).map(|_| rng.next() & 0xFFFF_FFFF));
    out.extend([
        0x3300_0000u64, // 2^-25: the tie below the smallest half denormal
        0x477F_E000,    // 65504
        0x477F_F000,    // 65520: the tie past the largest half
        0x4780_0000,
        0x7F80_0000,
        0xFF80_0000,
        0x0000_0001,
        0x8000_0000,
    ]);
    out
}

/// A 32-bit and a 16-bit float texel convert to nearest even in every access, a read's, a
/// gather's and a sample's alike: past `65504` to the infinity, half denormals kept (but by the
/// device's quantize). Metal rounds a 32-bit float texel's sample toward zero, saturating at
/// `±65504`, and a 16-bit float's linear sum with ties away from zero; the device's nearest even is
/// the answer, noted (`PrecisionNote::HalfTexelConversion`). With every tie of `texel_round`'s
/// probes (each half denormal's, every 13th normal half's, the top binade's).
#[test]
fn a_float_texel_rounds_to_nearest_even_in_every_access() {
    let mut texels = float_texels();
    texels.extend(super::texel_round::half_probe_bits());
    let texels = quads(&texels);
    for pf in [P::RGBA32Float, P::R32Float, P::RGBA16Float] {
        for lanes in every_lanes() {
            run_lanes(
                &format!("{pf:?}"),
                bits(pf),
                lanes,
                &texels,
                &rounded(HalfRounding::NearestEven),
            );
        }
    }
}

/// A two-plane YCbCr format's one texel and a 2D gather's lanes: its step of `1/8192` (or of
/// `1/2048`, a `half` already) rounds to `half` with ties away from zero, as Metal's read and
/// nearest sample of `YCBCR8_420_2P_sRGB`, `YCBCR10_420_2P_sRGB` and three 12-bit and
/// extended-range formats did (every channel), and its gathers, and every step of each grid: the
/// extended-range steps signed within `[-2, 2]`. A value that may be filtered keeps nearest even (a
/// filtered value is the device's).
#[test]
fn a_ycbcr_texel_rounds_its_step_with_ties_away() {
    for (pf, d, lo, hi) in [
        (P::YCBCR84202P, 2048, 0, 2048),
        (P::YCBCR84202PSRGB, 8192, 0, 8192),
        (P::YCBCR124202P, 8192, 0, 8192),
        (P::YCBCR104202PXR, 8192, -16384, 16384),
    ] {
        assert!(
            PackedTexels::of(pf).is_some_and(PackedTexels::ycbcr),
            "{pf:?}"
        );
        let values: Vec<u64> = (lo..=hi)
            .map(|k: i32| u64::from((k as f32 / d as f32).to_bits()))
            .collect();
        let texels = quads(&values);
        for lanes in every_lanes() {
            let mode = if lanes == TexelLanes::Filtered {
                HalfRounding::NearestEven
            } else {
                HalfRounding::NearestAway
            };
            run_lanes(&format!("{pf:?}"), bits(pf), lanes, &texels, &rounded(mode));
        }
        // A step of `1/8192` halfway between two halves rounds away from zero: `4098 / 8192`
        // (between the halves `4096 / 8192` and `4100 / 8192`) to `4100 / 8192`.
        if d == 8192 {
            let away = half_of(4098.0 / 8192.0, HalfRounding::NearestAway);
            assert_eq!(f32::from_bits(away) * 8192.0, 4100.0);
        }
    }
}

/// [`half::ROUTINE_WORDS`] bounds the routine's emission, and [`half::CALL_WORDS`] a call's.
#[test]
fn the_half_texel_routine_words_bound_its_emission() {
    use crate::spirv::serialize::serialize;
    let ins = [v4(), int(32)];
    let size = |body: Body<'_>| -> usize {
        let m = kernel(&ins, v4(), body).expect("build");
        serialize(m).expect("serialize").words.len()
    };
    for q in FORMS {
        let without = size(&|fb, x| {
            fb.module().set_quantize_form(q);
            Ok(x[0])
        });
        let defined = size(&|fb, x| {
            fb.module().set_quantize_form(q);
            half::define(fb.module())?;
            Ok(x[0])
        }) - without;
        let called = size(&body_lanes(TexelLanes::Filtered, q)) - without - defined;
        assert!(
            defined as u64 <= half::ROUTINE_WORDS,
            "{q:?}: the routine is {defined} words, bound {}",
            half::ROUTINE_WORDS
        );
        assert!(
            called <= half::CALL_WORDS as usize,
            "{q:?}: a call is {called} words, bound {}",
            half::CALL_WORDS
        );
        eprintln!("half texel routine {q:?}: {defined} words, a call {called}");
    }
}

/// On a device that samples through Metal's own texture unit, an 8-bit sRGB format's `f32` texel is
/// Metal's `float` value, a step `n / D` rounded to nearest even on the grids `1/4095` (a color
/// channel's table step, or a linear sum rounded onto it), `1/4080` (alpha's linear sum), and
/// `1/255` (alpha's code), and its rounding to `half` to nearest even is the step's own rounding to
/// `half`, Metal's `half` value: that class's conversion is Metal's there, unnoted
/// (`texture::half_texels`). Exact: integer arithmetic on the step and on the `f32` value.
#[test]
fn metals_srgb_float_steps_round_to_their_half_steps() {
    for d in [4095u32, 4080, 255] {
        let misses = steps_rounding_twice(d);
        assert!(misses.is_empty(), "the steps n / {d} at n in {misses:?}");
    }
}

/// `p / q` (in `[0, 1]`) rounded to `half` to nearest even: the value `k · 2^x` as `(k, x)` with
/// `k` odd (or zero), so that equal values are equal pairs.
fn half_of_ratio(p: u128, q: u128) -> (u128, i32) {
    if p == 0 {
        return (0, 0);
    }
    // The exponent `e` of `p / q` (`2^e <= p / q < 2^(e + 1)`), at least half's least normal
    // one, -14.
    let mut e = 0i32;
    while e > -14 && (p << e.unsigned_abs()) < q {
        e -= 1;
    }
    // `p / q` in units of `2^(e - 10)`, rounded to nearest even.
    let scaled = p << (10 - e).unsigned_abs();
    let (mut k, r) = (scaled / q, scaled % q);
    if 2 * r > q || (2 * r == q && k % 2 == 1) {
        k += 1;
    }
    let mut x = e - 10;
    while k % 2 == 0 {
        k /= 2;
        x += 1;
    }
    (k, x)
}

/// The `f32` value `n / d` rounded to nearest even (IEEE division of two exact operands), as the
/// ratio of its significand to a power of two.
fn f32_step(n: u32, d: u32) -> (u128, u128) {
    let bits = (n as f32 / d as f32).to_bits();
    if bits == 0 {
        return (0, 1);
    }
    let significand = u128::from(bits & 0x7f_ffff | 0x80_0000);
    let exponent = ((bits >> 23) & 0xff) as i32 - 150;
    assert!(exponent < 0, "{n} / {d}");
    (significand, 1u128 << exponent.unsigned_abs())
}

/// The steps `n` of grid `1/d` (`0 <= n <= d`) whose `f32` value rounds to another `half` than
/// the step itself: exact, by integer arithmetic on the step and on the `f32` value.
fn steps_rounding_twice(d: u32) -> Vec<u32> {
    (0..=d)
        .filter(|&n| {
            let (p, q) = f32_step(n, d);
            half_of_ratio(u128::from(n), u128::from(d)) != half_of_ratio(p, q)
        })
        .collect()
}

/// Metal's `float` texel of every grid but the 16-bit normalized ones is a step `n / D` rounded to
/// `f32` to nearest even, and that value's rounding to `half` is the step's own, Metal's `half`,
/// for every step: the 8-bit normalized codes (`1/255`, `1/127`) and filter grids (`1/4080`,
/// `1/2032`), every packed code scale and filter grid, the sRGB table's `1/4095`, and the YCbCr
/// grids `1/2048` and `1/8192`. So on a device that samples through Metal's own texture unit the
/// device's nearest even `half` of the texel is Metal's for those classes, unnoted. The 16-bit
/// grids are not: `65455 / 65535`, `65519 / 65535` and `32759 / 32767` round twice, one unit in
/// the last place of the `half` from Metal's (`PrecisionNote::HalfTexelConversion` on a 16-bit
/// normalized slot). A signed step's value is the negation of its magnitude's, so the magnitudes
/// cover both signs. Exact: integer arithmetic on the step and on the `f32` value.
#[test]
fn metal_float_steps_round_to_their_half() {
    let mut grids = vec![255u32, 127, 4080, 2032, 4095, 2048, 8192];
    for p in PackedTexels::ALL {
        grids.extend(p.scales().into_iter().flatten());
        grids.extend(p.filter_grids().into_iter().flatten());
    }
    grids.sort_unstable();
    grids.dedup();
    for d in grids {
        let misses = steps_rounding_twice(d);
        assert!(misses.is_empty(), "the steps n / {d} at n in {misses:?}");
    }
    assert_eq!(steps_rounding_twice(65535), vec![65455, 65519]);
    assert_eq!(steps_rounding_twice(32767), vec![32759]);
}

/// The YCbCr field's ties away come from the nearest-even `half` and the texel, selected after the
/// rounding of the texel itself, so every rounding the routine leaves to the device (the
/// `Converted` form's `OpFConvert` to `half` and the `Device` form's `OpQuantizeToF16`) reads the
/// routine's texel parameter directly: where a device specializes a class without the field, the
/// selection drops it whole, and no `[[clang::optnone]]` call is left at the access on MoltenVK.
#[test]
fn the_device_rounding_reads_the_texel_alone() {
    use crate::spirv::serialize::serialize;
    use std::collections::BTreeSet;
    // SPIR-V opcodes.
    const OP_TYPE_FLOAT: u32 = 22;
    const OP_TYPE_VECTOR: u32 = 23;
    const OP_FUNCTION: u32 = 54;
    const OP_FUNCTION_PARAMETER: u32 = 55;
    const OP_FUNCTION_END: u32 = 56;
    const OP_FCONVERT: u32 = 115;
    const OP_QUANTIZE_TO_F16: u32 = 116;
    for q in [QuantizeForm::Device, QuantizeForm::Converted] {
        let m = kernel(&[v4(), int(32)], v4(), &|fb, x| {
            fb.module().set_quantize_form(q);
            half::define(fb.module())?;
            Ok(x[0])
        })
        .expect("build");
        let words = serialize(m).expect("serialize").words;
        let mut insts = Vec::new();
        let mut i = 5;
        while i < words.len() {
            let n = ((words[i] >> 16) as usize).max(1);
            insts.push(&words[i..(i + n).min(words.len())]);
            i += n;
        }
        // The `half` scalar and vector types, the results of a conversion to `half`.
        let mut halves = BTreeSet::new();
        for w in &insts {
            match w[0] & 0xFFFF {
                OP_TYPE_FLOAT if w.get(2) == Some(&16) => {
                    halves.insert(w[1]);
                }
                OP_TYPE_VECTOR if halves.contains(&w[2]) => {
                    halves.insert(w[1]);
                }
                _ => {}
            }
        }
        // The routine is the module's function whose first parameter is a `vec4` and that has
        // three parameters; its texel is that first parameter.
        let mut texel = None;
        let mut params = Vec::new();
        let mut roundings = Vec::new();
        let mut body = Vec::new();
        for w in &insts {
            match w[0] & 0xFFFF {
                OP_FUNCTION => {
                    params.clear();
                    body.clear();
                }
                OP_FUNCTION_PARAMETER => params.push(w[2]),
                OP_FUNCTION_END => {
                    if params.len() == 3 {
                        texel = Some(params[0]);
                        roundings = body.clone();
                    }
                }
                OP_FCONVERT if halves.contains(&w[1]) => body.push((w[2], w[3])),
                OP_QUANTIZE_TO_F16 => body.push((w[2], w[3])),
                _ => {}
            }
        }
        let texel = texel.expect("the routine");
        assert!(!roundings.is_empty(), "{q:?}: the routine rounds the texel");
        for (result, operand) in roundings {
            assert_eq!(
                operand, texel,
                "{q:?}: the device's rounding %{result} reads %{operand}, not the texel"
            );
        }
    }
}

/// The plain arm (`narrow::round_texel`, `SpecKey::PlainHalfTexels`) answers as the routine does
/// for every class that takes it: for each pixel format whose class `plain_on_metal_sampler` holds,
/// the routine at that class, one texel's lanes, a filtered value's and gathered channels', is the
/// texel rounded to nearest even with its NaN kept (a denormal the device's quantize's zero or
/// itself), as is the plain arm itself, in each form a device sampling through Metal's own texture
/// unit rounds in.
#[test]
fn the_plain_arm_is_the_metal_sampler_routine_for_its_classes() {
    let mut texels: Vec<u64> = vec![
        0,
        0x8000_0000,
        1,
        0x8000_0001,
        0x0080_0000,
        0x3380_0000,
        0x3380_0001,
        0x387F_E000,
        0x387F_F000,
        0x3880_0000,
        0x3F80_0000,
        0xBF80_0000,
        0x477F_E000,
        0x477F_EFFF,
        0x477F_F000,
        0x4780_0000,
        0x7F7F_FFFF,
        0x7F80_0000,
        0xFF80_0000,
        0x7FC0_0000,
        0x7F80_0001,
        0xFFC0_2000,
    ];
    // Every 8-bit code's value, and a spread of bit patterns.
    texels.extend((0..=255u32).map(|c| u64::from((c as f32 / 255.0).to_bits())));
    let mut s = 0x9E37_79B9_u32;
    for _ in 0..256 {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        texels.push(u64::from(s));
    }
    let quads = quads(&texels);
    let rte = |x: u64| -> u64 {
        let f = f32::from_bits(x as u32);
        if f.is_nan() {
            x
        } else {
            u64::from((f16_to_f64(f64_to_f16(f64::from(f))) as f32).to_bits())
        }
    };
    let roundings = [
        QuantizeForm::Device,
        QuantizeForm::Converted,
        QuantizeForm::Controlled,
    ];
    let mut plain_classes = std::collections::BTreeSet::new();
    for &pf in P::ALL {
        let class = texel_format(pf);
        if !class.plain_on_metal_sampler() || !plain_classes.insert(class.0) {
            continue;
        }
        let cases: Vec<Case> = quads
            .iter()
            .map(|t| vec![t.to_vec(), vec![u64::from(class.0)]])
            .collect();
        for q in roundings {
            for lanes in every_lanes() {
                check_accept(
                    &format!("{pf:?} {lanes:?} {q:?}"),
                    &[v4(), int(32)],
                    v4(),
                    &body_lanes(lanes, q),
                    &cases,
                    &|c| Some(c[0].iter().map(|x| rte(*x)).collect()),
                    &|g, w| texel_answers(q, g, w),
                );
            }
        }
    }
    assert!(
        plain_classes.len() > 10,
        "{} plain classes",
        plain_classes.len()
    );
    let cases: Vec<Case> = quads.iter().map(|t| vec![t.to_vec()]).collect();
    for q in roundings {
        check_accept(
            &format!("the plain arm {q:?}"),
            &[v4()],
            v4(),
            &move |fb: &mut FunctionBuilder<'_>, x: &[Value]| {
                fb.module().set_quantize_form(q);
                let half4 = Form {
                    held: Held::Float(FloatFmt::Half),
                    lanes: 4,
                };
                crate::lower::arith::narrow::round_texel(fb, half4, x[0])
            },
            &cases,
            &|c| Some(c[0].iter().map(|x| rte(*x)).collect()),
            &|g, w| texel_answers(q, g, w),
        );
    }
    // The predicate leaves out a YCbCr field on the grid 8192 alone.
    for (pf, plain) in [
        (P::YCBCR84202PSRGB, false),
        (P::YCBCR124202P, false),
        (P::RGBA32Float, true),
        (P::R32Float, true),
        (P::RGBA16Unorm, true),
        (P::RG16Snorm, true),
        (P::RGBA8Unorm, true),
        (P::BGRA8Unorm, true),
        (P::RGBA16Float, true),
        (P::RGB10A2Unorm, true),
    ] {
        assert_eq!(texel_format(pf).plain_on_metal_sampler(), plain, "{pf:?}");
    }
}
