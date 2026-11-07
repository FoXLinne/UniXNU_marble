//! Which row's bytes stand where the rows of a blit into a two-plane extended-range texture at a
//! width off 4 meet (`two_planes/buffer_rows.rs`), against our probe `bpm.m`'s meetings (the M3
//! Max under macOS 27.0.1, alike in two runs but between groups of 32 rows).

use crate::two_planes::later_row_stands;

/// Each case: the level's width, the region's origin, and Metal's meetings of its 70 rows, `E`
/// where the earlier row's bytes stood and `L` the later's.
const MEETINGS: [(u64, [u64; 2], &str); 8] = [
    (
        5,
        [0, 0],
        "ELELELELELELELELELELELELELELELEEELELELELELELELELELELELELELELELEEELELE",
    ),
    (
        5,
        [0, 1],
        "EEELEEELEEELEEELEEELEEELEEELEEEEEEELEEELEEELEEELEEELEEELEEELEEEEEEELE",
    ),
    (
        7,
        [1, 0],
        "EEELEEELEEELEEELEEELEEELEEELEEEEEEELEEELEEELEEELEEELEEELEEELEEEEEEELE",
    ),
    (
        7,
        [2, 3],
        "EEELEEELEEELEEELEEELEEELEEELEEEEEEELEEELEEELEEELEEELEEELEEELEEEEEEELE",
    ),
    (
        13,
        [0, 2],
        "ELELELELELELELELELELELELELELELEEELELELELELELELELELELELELELELELEEELELE",
    ),
    (
        19,
        [1, 1],
        "EEELEEELEEELEEELEEELEEELEEELEEEEEEELEEELEEELEEELEEELEEELEEELEEEEEEELE",
    ),
    (
        31,
        [2, 0],
        "ELELELELELELELELELELELELELELELEEELELELELELELELELELELELELELELELEEELELE",
    ),
    (
        31,
        [1, 2],
        "EEELEEELEEELEEELEEELEEELEEELEEEEEEELEEELEEELEEELEEELEEELEEELEEEEEEELE",
    ),
];

/// Below a width of 32 the later row's bytes stand between pairs of rows from an even origin and
/// between fours from another, the earlier row's within them, as Metal's did; between groups of
/// 32 rows, and at a width of 32 or more, Metal's order varies and the rule names none.
#[test]
fn rows_meet_as_metal_s_gpu_writes_them() {
    for (width, origin, metal) in MEETINGS {
        for (r, letter) in metal.chars().enumerate() {
            let r = r as u64;
            let stands = later_row_stands(width, origin, r);
            if r % 32 == 31 {
                assert_eq!(stands, None, "{width} {origin:?} meeting {r}");
            } else {
                assert_eq!(
                    stands,
                    Some(letter == 'L'),
                    "{width} {origin:?} meeting {r}"
                );
            }
        }
    }
    for width in [33, 37, 41, 101] {
        assert!((0..64).all(|r| later_row_stands(width, [0, 0], r).is_none()));
    }
}
