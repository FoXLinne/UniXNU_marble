//! Metal's precise division by the device's `OpFDiv` ([`divide::native`]), on the interpreter,
//! whose division is correctly rounded.

use super::*;
use crate::lower::arith::divide;

/// A precise `float` division is the device's `OpFDiv` of its operands, a divisor past `2^126` and
/// its dividend first scaled by `2^-2` into Vulkan's range: exactly, so on the interpreter the
/// quotient is IEEE's on scalars and vectors, at the largest divisors, the infinities, the zeros
/// and NaN among them (a dividend the scale makes a denormal has a quotient far below `2^-149`
/// there).
#[test]
fn a_precise_division_is_the_device_s_quotient() {
    let edges: [u64; 16] = [
        0,
        0x8000_0000,
        0x0080_0000,
        0x0000_0003,
        0x3DCC_CCCD,
        0x3F80_0000,
        0xBF80_0000,
        0x4040_0000,
        0x4B00_0001,
        0x7E80_0000,
        0x7F00_0001,
        0x7F7F_FFFF,
        0xFF7F_FFFF,
        0x7F80_0000,
        0xFF80_0000,
        0x7FC0_0000,
    ];
    let mut pairs: Vec<[u64; 2]> = Vec::new();
    for x in edges {
        for y in edges {
            pairs.push([x, y]);
        }
    }
    let mut rng = Rng(0xD1F1_D1DE);
    for _ in 0..512 {
        let r = rng.next();
        pairs.push([r & 0xFFFF_FFFF, r >> 32]);
    }
    for lanes in [1u8, 3] {
        let f = vec(float(32), lanes);
        let n = usize::from(lanes);
        let cases: Vec<Case> = pairs
            .chunks(n)
            .filter(|c| c.len() == n)
            .map(|c| (0..2).map(|k| c.iter().map(|p| p[k]).collect()).collect())
            .collect();
        check(
            &format!("precise division x{lanes}"),
            &[f, f],
            f,
            &|fb, x| divide::native(fb, f, x[0], x[1]),
            &cases,
            &|c| {
                Some(
                    (0..c[0].len())
                        .map(|l| u64::from((f32v(c[0][l]) / f32v(c[1][l])).to_bits()))
                        .collect(),
                )
            },
            false,
        );
    }
}
