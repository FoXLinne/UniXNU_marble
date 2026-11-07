//! The layout of a texture's host shadow (`texture/host_shadow.rs`): where each subresource's
//! packed texels lie, and the rows a region reads out of them.
//!
//! The shadow is exact only where its layout is the one a copy through a staging buffer reads and
//! writes (`copy_plan::shape`): every row of every region at the offset that buffer would have
//! held it at. These tests hold the arithmetic against that layout with no device: the regions
//! that make the shadow cover every byte of it once, and a region's rows read and write exactly
//! the bytes its blocks lie in.

use crate::copy_plan::{CopyUnit, ImageRegion};
use crate::texture::host_shadow::Layout;
use metal2vulkan::vk::ImageAspectFlags;

/// A `BC1_RGBA`-shaped unit: 8 bytes a 4 x 4 block of colour.
const BC1: CopyUnit = CopyUnit {
    bytes: 8,
    extent: [4, 4],
    aspect: ImageAspectFlags::COLOR,
};

/// A 4-byte texel unit of colour.
const TEXEL: CopyUnit = CopyUnit {
    bytes: 4,
    extent: [1, 1],
    aspect: ImageAspectFlags::COLOR,
};

/// The region of `size` at `origin` of `layer` of `level`, of colour.
fn region(level: u32, layer: u32, origin: [u32; 3], size: [u32; 3]) -> ImageRegion {
    ImageRegion {
        aspect: ImageAspectFlags::COLOR,
        level,
        layer,
        layers: 1,
        origin,
        size,
    }
}

/// The regions the shadow is made by cover every byte of it exactly once, each at the offset the
/// layout gives it, and their bytes come to the layout's own total.
#[test]
fn the_making_copies_cover_every_byte_of_the_shadow_once() {
    // A 13 x 7 `BC1_RGBA` with four levels: 4 x 2, 2 x 1, 1 x 1 and 1 x 1 blocks.
    let layout = Layout::new([13, 7, 1], 4, 1, false, &BC1);
    assert_eq!(layout.total(), 8 * (8 + 2 + 1 + 1));
    let mut covered = vec![0u8; layout.total() as usize];
    for (region, at) in layout.regions() {
        let across = u64::from(region.size[0].div_ceil(4));
        let down = u64::from(region.size[1].div_ceil(4));
        let bytes = 8 * across * down * u64::from(region.size[2]);
        for byte in &mut covered[at as usize..(at + bytes) as usize] {
            *byte += 1;
        }
    }
    assert!(
        covered.iter().all(|&n| n == 1),
        "the making's copies cover every byte of the shadow once"
    );
    // A 2D array of three layers and two levels: each level's layers in order, levels outermost.
    let array = Layout::new([8, 8, 1], 2, 3, false, &BC1);
    // Three layers of 2 x 2 blocks, then three of one block each.
    assert_eq!(array.total(), 3 * 32 + 3 * 8);
    let regions = array.regions();
    assert_eq!(regions.len(), 6);
    let places: Vec<(u32, u32, u64)> = (regions.iter())
        .map(|(region, at)| (region.level, region.layer, *at))
        .collect();
    assert_eq!(
        places,
        vec![
            (0, 0, 0),
            (0, 1, 32),
            (0, 2, 64),
            (1, 0, 96),
            (1, 1, 104),
            (1, 2, 112),
        ]
    );
    // A 3D image: one region a level, holding its depth slices.
    let volume = Layout::new([8, 8, 3], 2, 1, true, &BC1);
    // Three depth slices of 2 x 2 blocks, then one slice of one block.
    assert_eq!(volume.total(), 3 * 32 + 8);
    let sizes: Vec<[u32; 3]> = (volume.regions().iter())
        .map(|(region, _)| region.size)
        .collect();
    assert_eq!(sizes, vec![[8, 8, 3], [4, 4, 1]]);
}

/// A region's rows read and write exactly the bytes its blocks lie in: level 0 of a `BC1_RGBA`
/// shadow read whole, a block-aligned part of it read, and a part written back.
#[test]
fn a_regions_rows_lie_where_its_blocks_do() {
    let layout = Layout::new([13, 7, 1], 4, 1, false, &BC1);
    // Each byte its own, so a row read from the wrong place shows.
    let texels: Vec<u8> = (0..layout.total()).map(|i| (i % 251) as u8).collect();
    // Level 0 whole: 4 x 2 blocks, its bytes from the start.
    let mut packed = vec![0; 64];
    assert!(layout.read(&region(0, 0, [0; 3], [13, 7, 1]), &texels, &mut packed));
    assert_eq!(packed, texels[..64]);
    // Blocks (1, 0) to (2, 1) of level 0: two rows of two blocks, 32 bytes apart in the shadow.
    let mut part = vec![0; 32];
    assert!(layout.read(&region(0, 0, [4, 0, 0], [8, 8, 1]), &texels, &mut part));
    assert_eq!(part[..16], texels[8..24]);
    assert_eq!(part[16..], texels[40..56]);
    // Level 2, one block, past the two levels before it (8 and 2 blocks).
    let at = 8 * (8 + 2);
    let mut one = vec![0; 8];
    assert!(layout.read(&region(2, 0, [0; 3], [3, 1, 1]), &texels, &mut one));
    assert_eq!(one, texels[at..at + 8]);
    // A write of those two rows of two blocks lands in the same bytes and touches no others.
    let mut after = texels.clone();
    let written: Vec<u8> = (0..32).map(|i| 0x80 | i as u8).collect();
    layout.write(&region(0, 0, [4, 0, 0], [8, 8, 1]), &written, &mut after);
    let mut wanted = texels.clone();
    wanted[8..24].copy_from_slice(&written[..16]);
    wanted[40..56].copy_from_slice(&written[16..]);
    assert_eq!(after, wanted);
}

/// A region the layout does not hold reads nothing, so the call falls back to the submission path
/// rather than answering bytes from the wrong place: another aspect, more than one layer, a level
/// or layer past the image's, a region past a level's blocks, and an origin off a block.
#[test]
fn a_region_the_layout_does_not_hold_reads_nothing() {
    let layout = Layout::new([13, 7, 1], 4, 2, false, &BC1);
    let texels = vec![0u8; layout.total() as usize];
    let mut packed = vec![0; 64];
    let refused = [
        ImageRegion {
            aspect: ImageAspectFlags::DEPTH,
            ..region(0, 0, [0; 3], [13, 7, 1])
        },
        ImageRegion {
            layers: 2,
            ..region(0, 0, [0; 3], [13, 7, 1])
        },
        region(4, 0, [0; 3], [13, 7, 1]),
        region(0, 2, [0; 3], [13, 7, 1]),
        region(0, 0, [0, 0, 0], [20, 7, 1]),
        region(0, 0, [0, 0, 0], [13, 7, 2]),
        region(0, 0, [2, 0, 0], [8, 8, 1]),
        region(0, 0, [0, 1, 0], [8, 8, 1]),
    ];
    for region in refused {
        assert!(
            !layout.read(&region, &texels, &mut packed),
            "{region:?} was read out of a shadow that does not hold it"
        );
    }
    // The region the layout does hold, for contrast: the same call reads it.
    assert!(layout.read(&region(0, 1, [0; 3], [13, 7, 1]), &texels, &mut packed));
}

/// An uncompressed unit's layout is the same arithmetic with a 1 x 1 block: a plain 2D image's
/// rows, so the layout is not a block format's special case.
#[test]
fn an_uncompressed_units_rows_lie_the_same_way() {
    let layout = Layout::new([3, 2, 1], 1, 1, false, &TEXEL);
    assert_eq!(layout.total(), 24);
    let texels: Vec<u8> = (0..24).collect();
    let mut packed = vec![0; 16];
    assert!(layout.read(&region(0, 0, [1, 0, 0], [2, 2, 1]), &texels, &mut packed));
    assert_eq!(
        packed,
        vec![4, 5, 6, 7, 8, 9, 10, 11, 16, 17, 18, 19, 20, 21, 22, 23]
    );
}
