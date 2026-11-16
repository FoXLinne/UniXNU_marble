//! The imageblock size a compute encoder takes (`compute::imageblock_size`), and an indirect
//! command's (`compute::validated_imageblock_size`); and the offset an indirect dispatch reads its
//! arguments at (`indirect_dispatch::arguments_offset`).

use super::super::compute::{imageblock_size, validated_imageblock_size};
use super::super::error::{Error, Invalid, Problem};
use super::super::indirect_dispatch::arguments_offset;
use metal2vulkan::bind::ImageblockSize;

/// Metal's API validation takes a width and a height that are powers of two and at most 1024 cells,
/// and refuses the rest at `setImageblockWidth:height:`, the same on an M3 and an M4 whatever the
/// pipeline (probe 6). An indirect command's size is held to it.
#[test]
fn a_validated_imageblock_size_is_powers_of_two_of_at_most_1024_cells() {
    for (width, height) in [
        (1, 1),
        (8, 8),
        (16, 16),
        (32, 32),
        (1024, 1),
        (1, 1024),
        (64, 16),
    ] {
        assert_eq!(
            validated_imageblock_size(width, height),
            Ok(ImageblockSize::new(width as u32, height as u32))
        );
    }
    for (width, height) in [
        (0, 0),
        (0, 8),
        (8, 0),
        (3, 5),
        (5, 3),
        (48, 40),
        (64, 33),
        (64, 32),
        (2048, 1),
        (65536, 1),
        (4096, 4096),
        (1 << 40, 1),
        (1 << 63, 1 << 63),
        (u64::MAX, 1),
    ] {
        assert_eq!(
            validated_imageblock_size(width, height),
            Err(Problem::ImageblockSize { width, height }),
            "{width}x{height}"
        );
    }
}

/// Plain Metal's answers where its API validation asserts, measured on an M3 Max and an M4 alike,
/// and on an M4, each the size the kernel's `get_width` and `get_height` read. A side of 0 reads 1
/// by 1, a side that is not a power of two reads the power above, and more than 1024 cells in one
/// row or column read one cell. More than 1024 cells over more rows and columns read 1024 by 1 with
/// every cell 0, and a side past 65536 is unmeasured: both stay refused.
#[test]
fn an_imageblock_size_is_what_plain_metal_sets() {
    for ((width, height), read) in [
        ((8, 8), (8, 8)),
        ((0, 0), (1, 1)),
        ((0, 3), (1, 1)),
        ((0, 8), (1, 1)),
        ((5, 0), (1, 1)),
        ((8, 0), (1, 1)),
        ((0, 2048), (1, 1)),
        ((2048, 0), (1, 1)),
        ((3, 5), (4, 8)),
        ((5, 3), (8, 4)),
        ((6, 6), (8, 8)),
        ((17, 17), (32, 32)),
        ((3, 1), (4, 1)),
        ((1, 3), (1, 4)),
        ((1023, 1), (1024, 1)),
        ((2048, 1), (1, 1)),
        ((65536, 1), (1, 1)),
        ((1, 2048), (1, 1)),
    ] {
        assert_eq!(
            imageblock_size(width, height),
            Ok(ImageblockSize::new(read.0, read.1)),
            "{width}x{height}"
        );
    }
    for (width, height) in [
        (2, 1024),
        (33, 31),
        (40, 48),
        (48, 40),
        (64, 32),
        (64, 33),
        (64, 64),
        (65537, 1),
        (1, 1 << 40),
        (u64::MAX, u64::MAX),
    ] {
        assert_eq!(
            imageblock_size(width, height),
            Err(Problem::ImageblockSize { width, height }),
            "{width}x{height}"
        );
    }
}

/// An indirect dispatch whose arguments run past its buffer's length is input plain Metal takes:
/// it reads them out of the buffer's allocation, not out of its length. Measured on an Apple M3
/// Max under macOS 27.0.1 with the API validation layer off: a no-copy buffer of length 8 over a
/// 16 KiB page holding the group counts `(7, 1, 1)` ran its
/// `dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:0`, whose kernel counted 7
/// threadgroups, so Metal read the third `u32` at byte 8, past the length; the same call on a
/// 256-byte buffer of `(3, 1, 1)` counted 3, so the counts come from the buffer and not from a
/// copy the encoder kept. With the layer on Metal reported the range and ran the dispatch anyway.
///
/// So the runtime takes the arguments as far as the bytes it holds at Metal's address
/// (`Buffer::copy_reach`, a no-copy buffer's host pages to the end of its `VkBuffer`, else the
/// length), and refuses only past them, where it holds none of the bytes Metal reads -- the bound
/// a blit's read of a buffer is held to.
#[test]
fn indirect_dispatch_arguments_reach_past_the_buffer_s_length() {
    const CALL: &str = "dispatch_threadgroups_indirect";
    let refusal = |offset, bytes, length| {
        Error::Invalid(Invalid::new(
            CALL,
            Problem::IndirectDispatchRange {
                offset,
                bytes,
                length,
            },
        ))
    };
    // Inside the length: taken, whatever the reach.
    for (length, offset) in [(12, 0), (64, 40), (16384, 16372)] {
        assert_eq!(
            arguments_offset(CALL, (length, length), offset, 12),
            Ok(offset),
            "{offset} of {length}"
        );
    }
    // The measured case: the arguments run past the length and inside the allocation the runtime
    // holds, so they are taken and the dispatch runs.
    assert_eq!(arguments_offset(CALL, (8, 16384), 0, 12), Ok(0));
    assert_eq!(arguments_offset(CALL, (8, 16384), 0, 24), Ok(0));
    assert_eq!(arguments_offset(CALL, (8, 16384), 16372, 12), Ok(16372));
    // Past the reach: refused, naming the buffer's length as Metal's own report does.
    assert_eq!(
        arguments_offset(CALL, (8, 16384), 16376, 12).unwrap_err(),
        refusal(16376, 12, 8)
    );
    // A buffer whose reach is its length refuses the same arguments the measured one takes.
    assert_eq!(
        arguments_offset(CALL, (8, 8), 0, 12).unwrap_err(),
        refusal(0, 12, 8)
    );
    // An offset that is not a multiple of 4 reads at the multiple below, and is checked there:
    // 6 rounds to 4, whose 12 bytes end at 16, inside a reach of 16 and past one of 8.
    assert_eq!(arguments_offset(CALL, (8, 16), 6, 12), Ok(4));
    assert_eq!(
        arguments_offset(CALL, (8, 8), 6, 12).unwrap_err(),
        refusal(4, 12, 8)
    );
    // An offset whose arguments overflow is past the reach, never taken.
    assert_eq!(
        arguments_offset(CALL, (16, 16), u64::MAX - 3, 12).unwrap_err(),
        refusal(u64::MAX - 3, 12, 16)
    );
}
