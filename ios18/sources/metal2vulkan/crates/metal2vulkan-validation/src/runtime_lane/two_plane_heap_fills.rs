//! Fills of a heap's two-plane extended-range texture of any shape the runtime holds over the
//! heap's bytes (`texture/heap_two_planes.rs`), on the device the cfg selects, under the validation
//! layer, against the bytes Metal's fills write into the heap (`blit_fill.rs`'s module docs).
//!
//! Metal's answer (an Apple M3 Max under macOS 27.0.1; `fillTexture:level:slice:region:bytes:
//! length:` of an `RGB10A8_2P_XR10` texture at byte 0 of a `Shared` placement heap of Metal's size
//! and 64 KiB more, every byte of it `0xEE` through a buffer over it, and the heap's bytes read
//! back, for a whole level's region and one inset by a texel, at every level of 2D textures of
//! 10 x 10, 13 x 9, 22 x 22, 37 x 37, 100 x 40, 255 x 7 and 5 x 20 texels and 3 to 6 levels, slice 1
//! of 2D arrays of 16 x 16 and 37 x 9 texels, 3 levels and 2 slices, every slice of 1D arrays of
//! 37, 5 and 16 texels, 1D textures of 37 and 64, and every depth image of every level of 3D
//! textures of 10 x 10 x 4, 37 x 9 x 4, 16 x 16 x 8 and 8 x 8 x 2 texels and 1 to 4 levels): a
//! fill of any level writes the pattern's first 4 bytes, plane 0's color word, at byte
//! `slice · S + y · R + 4 x` from the texture's first, for each texel `(x, y)` of the region cut
//! at the level's extent, `R` level 0's row of `4 w` bytes rounded down to 16 (16 below a width of
//! 4), `S` an array's slice of `align(h · align(4 w, 16), 128)` bytes, `w` and `h` level 0's
//! extent; rows meet where `4 w` is not a multiple of 16. A 3D texture's depth images write its
//! first's bytes, but a mipmapped one's past the first write none (a one-level one's past the
//! first wrote none in some runs: the lane takes the first image's bytes, Metal's other answer).
//! No other byte changes.

use super::degrade_notices::on_device;
use super::extended_range_two_planes::{get, put};
use metal2vulkan::metal::{PixelFormat, TextureType, TextureUsage};
use metal2vulkan_runtime::{HeapDescriptor, HeapType, Region, ResourceOptions, TextureDescriptor};
use std::time::Duration;

/// The longest a command buffer may take.
const WAIT: Duration = Duration::from_secs(60);

/// The fill's pattern, of which Metal writes the first 4 bytes.
const PATTERN: [u8; 8] = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88];

/// The probes' textures: type, then width, height, depth, levels and slices.
const SHAPES: [(TextureType, [u64; 5]); 19] = [
    (TextureType::Type2D, [10, 10, 1, 4, 1]),
    (TextureType::Type2D, [13, 9, 1, 4, 1]),
    (TextureType::Type2D, [22, 22, 1, 5, 1]),
    (TextureType::Type2D, [37, 37, 1, 6, 1]),
    (TextureType::Type2D, [100, 40, 1, 4, 1]),
    (TextureType::Type2D, [255, 7, 1, 3, 1]),
    (TextureType::Type2D, [5, 20, 1, 3, 1]),
    (TextureType::Type2DArray, [16, 16, 1, 3, 2]),
    (TextureType::Type2DArray, [37, 9, 1, 3, 2]),
    (TextureType::Type1D, [37, 1, 1, 1, 1]),
    (TextureType::Type1D, [64, 1, 1, 1, 1]),
    (TextureType::Type1DArray, [37, 1, 1, 1, 2]),
    (TextureType::Type1DArray, [5, 1, 1, 1, 3]),
    (TextureType::Type1DArray, [16, 1, 1, 1, 3]),
    (TextureType::Type3D, [10, 10, 4, 1, 1]),
    (TextureType::Type3D, [10, 10, 4, 2, 1]),
    (TextureType::Type3D, [37, 9, 4, 3, 1]),
    (TextureType::Type3D, [16, 16, 8, 4, 1]),
    (TextureType::Type3D, [8, 8, 2, 1, 1]),
];

/// One fill: its level, slice, depth image, and region (origin and size).
struct Fill {
    level: u64,
    slice: u64,
    z: u64,
    region: ([u64; 2], [u64; 2]),
}

/// The heap's bytes, all `0xEE` at first, after Metal's `fill` of a texture of `ty` and
/// `levels` levels, level 0's extent `[w, h]`, at byte 0 (the module docs).
fn metal_bytes(length: u64, ty: TextureType, [w, h, levels]: [u64; 3], fill: &Fill) -> Vec<u8> {
    let mut bytes = vec![0xee; length as usize];
    if ty == TextureType::Type3D && levels > 1 && fill.z > 0 {
        return bytes;
    }
    let rows = ((4 * w) & !15).max(16);
    let slice_bytes = (h * (4 * w).next_multiple_of(16)).next_multiple_of(128);
    let [lw, lh] = [(w >> fill.level).max(1), (h >> fill.level).max(1)];
    let ([x, y], [rw, rh]) = fill.region;
    for row in y..(y + rh).min(lh) {
        for texel in x..(x + rw).min(lw) {
            let at = (fill.slice * slice_bytes + row * rows + 4 * texel) as usize;
            bytes[at..at + 4].copy_from_slice(&PATTERN[..4]);
        }
    }
    bytes
}

/// The runs of bytes that differ between `got` and `want`, as `offset+length`.
fn differing(got: &[u8], want: &[u8]) -> String {
    let mut runs = Vec::new();
    let mut i = 0;
    while i < got.len() {
        if got[i] == want[i] {
            i += 1;
            continue;
        }
        let start = i;
        while i < got.len() && got[i] != want[i] {
            i += 1;
        }
        runs.push(format!("{start}+{}", i - start));
    }
    runs.join(" ")
}

/// The probes' fills of a texture of `levels` levels, level 0's extent `[w, h, d]`, and `slices`
/// slices: at every level, slice and depth image, the level's whole region and the one inset by
/// a texel (where 3 texels wide, and in `y` where 3 high).
fn fills([w, h, d, levels, slices]: [u64; 5]) -> Vec<Fill> {
    let mut fills = Vec::new();
    for level in 0..levels {
        let [lw, lh, ld] = [w, h, d].map(|e| (e >> level).max(1));
        for slice in 0..slices {
            for z in 0..ld {
                fills.push(Fill {
                    level,
                    slice,
                    z,
                    region: ([0, 0], [lw, lh]),
                });
                if lw >= 3 {
                    let (y, rh) = if lh >= 3 { (1, lh - 2) } else { (0, lh) };
                    fills.push(Fill {
                        level,
                        slice,
                        z,
                        region: ([1, y], [lw - 2, rh]),
                    });
                }
            }
        }
    }
    fills
}

/// Every fill of the probes at every level, slice and depth image of a heap's two-plane texture
/// writes the heap's bytes Metal's fill writes and no other.
#[test]
fn fills_of_a_heap_s_two_plane_texture_write_metal_s_bytes() {
    on_device(|device, _messages| {
        let queue = device.new_command_queue().unwrap();
        let options = ResourceOptions::shared();
        let mut wrong = Vec::new();
        let mut cases = 0;
        for (ty, shape) in SHAPES {
            let [w, h, d, levels, slices] = shape;
            let mut descriptor =
                TextureDescriptor::texture_2d(PixelFormat::RGB10A82PXR10, w, h, false);
            descriptor.texture_type = ty;
            descriptor.depth = d;
            descriptor.mipmap_level_count = levels;
            descriptor.array_length = slices;
            descriptor.usage = TextureUsage::SHADER_READ | TextureUsage::SHADER_WRITE;
            descriptor.resource_options = options;
            let answer = device.heap_texture_size_and_align(&descriptor).unwrap();
            let length = answer.size + 0x10000;
            for fill in fills(shape) {
                let heap = device
                    .new_heap(&HeapDescriptor::new(HeapType::Placement, length, options))
                    .unwrap();
                let over = heap.new_buffer_at_offset(length, options, 0).unwrap();
                put(device, &queue, &over, &vec![0xee; length as usize]);
                let texture = heap.new_texture_at_offset(&descriptor, 0).unwrap();
                let command_buffer = queue.command_buffer().unwrap();
                let mut blit = command_buffer.blit_command_encoder().unwrap();
                let ([x, y], [rw, rh]) = fill.region;
                let region = Region::new_3d(x, y, fill.z, rw, rh, 1);
                blit.fill_texture(&texture, fill.level, fill.slice, region, &PATTERN)
                    .unwrap();
                blit.end_encoding().unwrap();
                command_buffer.commit().unwrap();
                command_buffer.wait_until_completed_for(WAIT).unwrap();
                let got = get(device, &queue, &over, length);
                let want = metal_bytes(length, ty, [w, h, levels], &fill);
                cases += 1;
                if got != want {
                    let what = format!(
                        "{ty:?} {w}x{h}x{d} levels {levels} slices {slices}: level {} slice {} \
                         z {} region {:?}",
                        fill.level, fill.slice, fill.z, fill.region
                    );
                    let blank = vec![0xee; got.len()];
                    println!(
                        "XR-TWO-PLANE-HEAP-FILLS-DIFFERS {what}: bytes {}; written {}; Metal's {}",
                        differing(&got, &want),
                        differing(&got, &blank),
                        differing(&want, &blank)
                    );
                    wrong.push(what);
                }
                drop((texture, over, heap));
            }
        }
        println!(
            "XR-TWO-PLANE-HEAP-FILLS {cases} fills, {} unlike Metal's",
            wrong.len()
        );
        assert!(wrong.is_empty(), "fills unlike Metal's: {wrong:#?}");
    });
}
