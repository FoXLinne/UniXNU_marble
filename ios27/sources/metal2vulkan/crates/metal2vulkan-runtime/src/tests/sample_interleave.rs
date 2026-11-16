//! The buffer's side of a blit between a buffer and a multisampled texture (`sample_interleave.rs`):
//! each row lies where plain Metal places it, in the buffer, or the copy is refused.

use super::super::blit_plan::BufferLayout;
use super::super::sample_interleave::{placed_rows, Unplaced};

fn layout(offset: u64, bytes_per_row: u64) -> BufferLayout {
    BufferLayout {
        offset,
        bytes_per_row,
        bytes_per_image: 0,
    }
}

/// [`placed_rows`] of 4 rows of `width` texels of `samples` samples of `bpp` bytes, in a buffer
/// of 4096 bytes.
fn rows(
    samples: u64,
    bpp: u64,
    width: u64,
    (offset, bytes_per_row): (u64, u64),
    into_texture: bool,
) -> Result<Vec<u64>, Unplaced> {
    let pixel = samples * bpp;
    let l = layout(offset, bytes_per_row);
    placed_rows(&l, pixel, width * pixel, 4, 4096, into_texture)
}

/// The rows of 4-row copies as plain Metal placed them on an Apple M3 Max under macOS 27.0.1
/// (each case one the probe measured, by its pixel format, sample count and width).
#[test]
fn rows_lie_where_plain_metal_places_them() {
    // R16Uint, 2 samples, 2 wide, out of the texture: the pixel's layout as it is.
    assert_eq!(rows(2, 2, 2, (4, 8), false), Ok(vec![4, 12, 20, 28]));
    // R8Uint, 4 samples: the offset rounds down to the 4-byte pixel.
    assert_eq!(rows(4, 1, 1, (61, 4), true), Ok(vec![60, 64, 68, 72]));
    // RG8Uint, 4 samples, 2 wide: to 4 bytes of an 8-byte pixel.
    assert_eq!(rows(4, 2, 2, (3, 16), true), Ok(vec![0, 16, 32, 48]));
    // Depth32Float, 4 samples, 2 wide, both ways: the pitch rounds down to the 16-byte pixel.
    for into_texture in [true, false] {
        assert_eq!(
            rows(4, 4, 2, (2, 37), into_texture),
            Ok(vec![0, 32, 64, 96])
        );
    }
    // RGBA32Uint, 2 samples (a 32-byte pixel): the offset rounds to 2 bytes.
    assert_eq!(rows(2, 16, 1, (7, 32), true), Ok(vec![6, 38, 70, 102]));
    // RGBA32Uint, 4 samples (64 bytes): to 4 bytes.
    assert_eq!(rows(4, 16, 1, (4, 64), true), Ok(vec![4, 68, 132, 196]));
    // R32Uint, 4 samples, 2 wide, into the texture at a pitch below the 32-byte row: the rows
    // overlap, the pitch rounded to 16 bytes.
    for bytes_per_row in 20..30 {
        assert_eq!(
            rows(4, 4, 2, (5, bytes_per_row), true),
            Ok(vec![4, 20, 36, 52])
        );
    }
    // R16Uint, 4 samples: a pitch that rounds to 0 from an offset off 16 puts every row at the
    // first; R32Uint, 4 samples, a pitch of 0 the same.
    assert_eq!(rows(4, 2, 1, (4, 4), true), Ok(vec![4, 4, 4, 4]));
    assert_eq!(rows(4, 4, 1, (4, 0), true), Ok(vec![4, 4, 4, 4]));
}

/// The layouts plain Metal places by a rule the runtime does not hold are its gap, and those it
/// copies no defined bytes for are refused.
#[test]
fn layouts_without_a_held_placement_are_refused() {
    // R8Uint, 2 samples: a pitch of 0 from offset 0 (block order); from 16, a single row too.
    assert_eq!(rows(2, 1, 1, (0, 0), true), Err(Unplaced::Gap));
    let one_row = placed_rows(&layout(16, 0), 2, 2, 1, 4096, true);
    assert_eq!(one_row, Err(Unplaced::Gap));
    assert_eq!(placed_rows(&layout(0, 0), 2, 2, 1, 4096, true), Ok(vec![0]));
    // RG32Uint, 4 samples (a 32-byte pixel): a pitch off the pixel, and one below the row.
    assert_eq!(rows(4, 8, 1, (0, 92), false), Err(Unplaced::Gap));
    assert_eq!(rows(4, 8, 1, (2, 14), true), Err(Unplaced::Gap));
    // R8Uint, 2 samples: a pitch below the pixel from an offset that is a multiple of 16 reads
    // bytes the buffer does not hold.
    assert_eq!(rows(2, 1, 1, (0, 1), true), Err(Unplaced::BytesPerRow));
    // Out of the texture, rows that overlap race.
    assert_eq!(rows(4, 4, 2, (4, 20), false), Err(Unplaced::BytesPerRow));
}

/// Rows of 4 texels at 4 samples of 4 bytes (64 bytes) reach the buffer's last byte and no
/// further; a guest's offset or `bytesPerRow` whose rows pass `u64::MAX` lies past every buffer,
/// never a wrapped offset inside it.
#[test]
fn rows_lie_in_the_buffer_up_to_its_last_byte() {
    let last = 4096 - (3 * 64 + 64);
    assert!(rows(4, 4, 4, (last, 64), true).is_ok());
    assert_eq!(
        rows(4, 4, 4, (last + 4, 64), true),
        Err(Unplaced::BufferRange)
    );
    // Rows that overlap, into the texture: the last row ends past every other.
    let last = 4096 - (3 * 16 + 64);
    assert!(rows(4, 4, 4, (last, 16), true).is_ok());
    assert_eq!(
        rows(4, 4, 4, (last + 4, 16), true),
        Err(Unplaced::BufferRange)
    );
    for (offset, bytes_per_row) in [
        (u64::MAX - 15, 64),
        (u64::MAX - 63, 64),
        (0, 1 << 63),
        (0, 0x5555_5555_5555_5560),
        (0, u64::MAX - 15),
    ] {
        let placed = rows(4, 4, 4, (offset, bytes_per_row), true);
        assert_eq!(
            placed,
            Err(Unplaced::BufferRange),
            "{offset} {bytes_per_row}"
        );
    }
    // An empty region places no rows.
    let empty = placed_rows(&layout(u64::MAX, u64::MAX), 16, 0, 4, 0, true);
    assert_eq!(empty, Ok(Vec::new()));
}
