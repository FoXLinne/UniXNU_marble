//! `air.pack` and `air.unpack` of the normalized formats by the device's conversion
//! ([`super::super::pack`]): the four formats `GLSL.std.450` names are its `Pack` and `Unpack`
//! instruction alone; `rgb10a2` and `rgb565` the instruction's definition by the device's clamp,
//! multiply, `RoundEven` and division; `unorm4x8_srgb` the MSL transfer functions by `Pow` around
//! the 8-bit instruction. Values on the interpreter's model of each instruction, one answer Vulkan
//! allows (its `Round` ties to even).

use super::*;
use crate::air::intrinsics::{PackDir, PackFormat as P};
use crate::lower::arith::pack;
use crate::spv::GlslStd450Op as G;

/// The `GLSL.std.450` instruction numbers module `m` uses, once per use, and its `OpFMul` count.
fn instructions(m: TypedModule) -> (Vec<u32>, usize) {
    const OP_EXT_INST: u32 = 12;
    const OP_FMUL: u32 = 133;
    let words = crate::spirv::serialize::serialize(m).expect("words").words;
    let (mut ext, mut fmul) = (Vec::new(), 0);
    let mut i = 5;
    while i < words.len() {
        let (n, code) = ((words[i] >> 16) as usize, words[i] & 0xFFFF);
        if code == OP_EXT_INST {
            ext.extend(words.get(i + 4).copied());
        }
        if code == OP_FMUL {
            fmul += 1;
        }
        i += n.max(1);
    }
    ext.sort_unstable();
    (ext, fmul)
}

/// The channel layout of `format`: each field's width, and whether it is signed.
fn layout(format: P) -> (Vec<u32>, bool) {
    let (widths, snorm) = math::pack_layout(format).expect("lowered");
    (widths.to_vec(), snorm)
}

/// The GLSL definition of a pack's field of `w` bits: `round(clamp(c) * s)`, the product rounded
/// to `float` and the tie to even, masked to the field.
fn code(c: f32, w: u32, snorm: bool) -> u64 {
    let s = if snorm {
        ((1u32 << (w - 1)) - 1) as f32
    } else {
        ((1u32 << w) - 1) as f32
    };
    let lo = if snorm { -1.0 } else { 0.0 };
    let q = (c.clamp(lo, 1.0) * s).round_ties_even() as i32;
    u64::from(q as u32) & ((1u64 << w) - 1)
}

/// An unpack's field `f` of `w` bits: `f / s` correctly rounded by an instruction (`named`), else
/// `f` times `float(1 / s)`; a signed one sign-extended and at least `-1`.
fn value(f: u64, w: u32, snorm: bool, named: bool) -> f32 {
    let (v, s) = if snorm {
        (
            ((f << (64 - w)) as i64 >> (64 - w)) as f32,
            ((1u32 << (w - 1)) - 1) as f32,
        )
    } else {
        (f as f32, ((1u32 << w) - 1) as f32)
    };
    let q = if named { v / s } else { v * (1.0 / s) };
    if snorm {
        q.max(-1.0)
    } else {
        q
    }
}

const VALUES: [f32; 18] = [
    0.0,
    -0.0,
    1.0,
    -1.0,
    0.5,
    -0.5,
    0.25,
    0.75,
    0.1,
    -0.3,
    1.5,
    -2.0,
    0.998,
    0.0019,
    0.501_960_8,
    0.498_039_2,
    1e-30,
    -1e30,
];

/// A pack of the four named formats is its `GLSL.std.450` instruction alone, with no product of
/// its own; `rgb10a2` and `rgb565` take the device's `NClamp`, one product and `RoundEven`; an
/// unpack of the four named formats is the `Unpack` instruction, and of the others the device's
/// product with the scale's reciprocal. Every field answers the instruction's definition, on
/// `float` and `half` lanes.
#[test]
fn normalized_packs_are_the_device_instructions() {
    let formats = [
        (
            P::Unorm4x8,
            Some((GlslOp::PackUnorm4x8, GlslOp::UnpackUnorm4x8)),
        ),
        (
            P::Snorm4x8,
            Some((GlslOp::PackSnorm4x8, GlslOp::UnpackSnorm4x8)),
        ),
        (
            P::Unorm2x16,
            Some((GlslOp::PackUnorm2x16, GlslOp::UnpackUnorm2x16)),
        ),
        (
            P::Snorm2x16,
            Some((GlslOp::PackSnorm2x16, GlslOp::UnpackSnorm2x16)),
        ),
        (P::UnormRgb10a2, None),
        (P::SnormRgb10a2, None),
        (P::UnormRgb565, None),
    ];
    for (format, named) in formats {
        let (widths, snorm) = layout(format);
        let lanes = widths.len() as u8;
        let word = int(math::packed_bits(format));
        assert_eq!(
            pack::instruction(format, PackDir::Pack).is_some(),
            named.is_some(),
            "{format:?}"
        );
        for lane in [float(32), float(16)] {
            let f = vec(lane, lanes);
            let half = lane.float_fmt() == Some(FloatFmt::Half);
            let words =
                |m: TypedModule| crate::spirv::serialize::serialize(m).expect("words").words;
            let packed =
                kernel(&[f], word, &|fb, x| pack::pack(fb, format, f, x[0])).expect("pack kernel");
            let unpacked = kernel(&[word], f, &|fb, x| pack::unpack(fb, format, f, x[0]))
                .expect("unpack kernel");
            match named {
                Some((p, u)) => {
                    // The instruction alone: the module is the bare instruction's (an unpack to
                    // `half` rounded once).
                    let bare = kernel(&[f], word, &|fb, x| fb.glsl(p, &[x[0]])).expect("bare pack");
                    assert_eq!(words(packed), words(bare), "{format:?} {lane:?}");
                    let bare = kernel(&[word], f, &|fb, x| {
                        let q = fb.glsl(u, &[x[0]])?;
                        crate::lower::arith::narrow::round(fb, f, q)
                    })
                    .expect("bare unpack");
                    assert_eq!(words(unpacked), words(bare), "{format:?} {lane:?}");
                }
                None => {
                    let (pack_ext, pack_fmul) = instructions(packed);
                    for g in [G::NClamp, G::RoundEven] {
                        assert!(pack_ext.contains(&(g as u32)), "{format:?} {g:?}");
                    }
                    if !half {
                        assert_eq!(pack_fmul, 1, "{format:?} {lane:?}");
                    }
                }
            }
            // A lane's bits in its format, and its value.
            let bits = |c: f32| {
                if half {
                    u64::from(f64_to_f16(f64::from(c)))
                } else {
                    u64::from(c.to_bits())
                }
            };
            let read = |b: u64| {
                if half {
                    f16_to_f64(b as u16) as f32
                } else {
                    f32::from_bits(b as u32)
                }
            };
            let cases: Vec<Case> = (0..VALUES.len())
                .map(|i| {
                    vec![(0..usize::from(lanes))
                        .map(|k| bits(VALUES[(i + 3 * k) % VALUES.len()]))
                        .collect()]
                })
                .collect();
            check(
                &format!("pack {format:?} from {lane:?}"),
                &[f],
                word,
                &|fb, x| pack::pack(fb, format, f, x[0]),
                &cases,
                &|c| {
                    let mut at = 0;
                    let mut out = 0u64;
                    for (k, w) in widths.iter().enumerate() {
                        out |= code(read(c[0][k]), *w, snorm) << at;
                        at += w;
                    }
                    Some(vec![out])
                },
                false,
            );
            let mut rng = Rng(0x5EED_0000 | u64::from(lanes));
            let words: Vec<Case> = (0..64)
                .map(|_| vec![vec![rng.next() & mask(math::packed_bits(format))]])
                .chain([vec![vec![0]], vec![vec![mask(math::packed_bits(format))]]])
                .collect();
            check(
                &format!("unpack {format:?} to {lane:?}"),
                &[word],
                f,
                &|fb, x| pack::unpack(fb, format, f, x[0]),
                &words,
                &|c| {
                    let mut at = 0;
                    let mut out = Vec::new();
                    for w in &widths {
                        let field = (c[0][0] >> at) & ((1u64 << w) - 1);
                        let v = value(field, *w, snorm, named.is_some());
                        out.push(bits(v));
                        at += w;
                    }
                    Some(out)
                },
                false,
            );
        }
    }
}

/// `unorm4x8_srgb`'s colour channels are the MSL transfer functions by the device's `Pow`, around
/// the 8-bit instruction, no table read: an encode takes one `Pow` and `PackUnorm4x8`, a decode
/// `UnpackUnorm4x8` and one `Pow`. (The interpreter models no `Pow`, so no value is run.)
#[test]
fn srgb_packs_are_the_transfer_functions_by_the_devices_pow() {
    let f = vec(float(32), 4);
    let word = int(32);
    let packed = kernel(&[f], word, &|fb, x| {
        pack::pack(fb, P::Unorm4x8Srgb, f, x[0])
    })
    .expect("pack");
    let (ext, _) = instructions(packed);
    assert_eq!(ext, vec![G::Pow as u32, G::PackUnorm4x8 as u32]);
    let unpacked = kernel(&[word], f, &|fb, x| {
        pack::unpack(fb, P::Unorm4x8Srgb, f, x[0])
    })
    .expect("unpack");
    let (ext, _) = instructions(unpacked);
    assert_eq!(ext, vec![G::Pow as u32, G::UnpackUnorm4x8 as u32]);
}
