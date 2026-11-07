//! Metal's private texture fills (`BlitCommandEncoder::fill_texture` and `fill_texture_color`)
//! whose bytes a blit's read-back does not show, on the device the cfg selects, under the
//! validation layer, against Metal's bytes: our probes `surface.m`, `tp.m`, `misc.m` and `slot.m`
//! on an Apple M3 Max under macOS 27.0.1 (no guest data).
//!
//! - **Over a surface's plane, or a buffer.** A two-plane YCbCr texture over plane 0 or 1 of a
//!   CoreVideo surface, the buffer here standing for the surface with its planes where the
//!   surface's lie, and a padded 10-bit texture over a buffer, every byte first `0xEE`: the runs
//!   of bytes one fill changed, every byte of every run. A YCbCr texture's unit is its plane's
//!   code, and a packed one's texels each take one zero byte at the texel's `x` of its row. The
//!   `579 p70` row's bytes are `probes/surface_buffer_fills.m`'s, because `surface.m` printed at
//!   most 24 bytes of a run and that row's runs are 32 bytes each.
//! - **Two-plane extended-range.** A `Shared` texture of its own memory, or at byte 0 of a
//!   placement heap of Metal's size, every texel's color word set to its index (each slice's and
//!   level's own top bits) by `replace_region`, one fill of `a1 a2 a3 a4`, and the texels whose
//!   word `get_bytes` reads changed: Metal fills level 0 of the slice at Metal's fill rows,
//!   whatever the level, so a width off a multiple of 4 moves the fill between the texels' rows.
//! - **A color with nothing to pack.** Fills packed in `Depth32Float`, `BC1_RGBA` and
//!   `RGBA8Unorm` after one packed in `RGBA32Uint`, in one encoder with fills alone between, as
//!   Metal's warm runs write them (its 16-byte texel, cut and overwritten as the runtime's slot
//!   is).
//! - **An empty pattern** is the runtime's own gap
//!   (`NotImplemented(RuntimeGap::EmptyFillPattern)`): Metal's API validation layer stops it, and
//!   plain Metal takes the fill and reads a unit's bytes from the pattern's address.
//!
//! A difference prints a `SURFACE-FILLS-DIFFERS` line.

use super::{buffer_with, read_buffer, WAIT};
use metal2vulkan::metal::{PixelFormat, TextureType, TextureUsage};
use metal2vulkan_runtime::{
    BlitOption, ClearColor, CommandQueue, Device, Error, HeapDescriptor, HeapType, IoSurfacePlane,
    IoSurfaceYCbCr, Origin, Region, ResourceOptions, RuntimeGap, Size, Texture, TextureDescriptor,
};

use super::degrade_notices::on_device;

/// A plane of a surface: its first byte in the surface and its `bytesPerRow`.
type Plane = (u64, u64);

/// One fill over a surface's plane (`ostype` `Some`) or a buffer.
struct Over {
    /// The surface's OSType, `None` for a buffer.
    ostype: Option<&'static [u8; 4]>,
    /// The surface's size, or the texture's over a buffer.
    size: [u64; 2],
    format: u32,
    /// The plane the texture is made over.
    plane: u64,
    /// The surface's bytes (a buffer's: its rows and 256 more) and planes (a buffer's texture
    /// at 0, its rows the first's `bytesPerRow`).
    length: u64,
    planes: [Plane; 2],
    /// The probe's case: `b<n>` the whole texture with a pattern of `n` bytes `01 02 ..`, `r<n>`
    /// the region at 1, 1 of 2 x 1, `x<x>,<y>,<n>` the region at `x`, `y` of 3 x 2, `p<pf>` the
    /// whole texture, the color (0.25, 0.5, 0.75, 1) packed in `pf`.
    case: &'static str,
    /// The changed runs Metal's probe printed.
    metal: &'static str,
}

/// The surfaces' layouts, as CoreVideo made them (`surface.m`).
const S8X4: (u64, [Plane; 2]) = (1344, [(512, 64), (1024, 64)]);
const S8X4_444: (u64, [Plane; 2]) = (1600, [(512, 64), (1024, 64)]);

const fn surf(
    ostype: &'static [u8; 4],
    size: [u64; 2],
    (format, plane): (u32, u64),
    (length, planes): (u64, [Plane; 2]),
    case: &'static str,
    metal: &'static str,
) -> Over {
    Over {
        ostype: Some(ostype),
        size,
        format,
        plane,
        length,
        planes,
        case,
        metal,
    }
}

const fn buf(
    format: u32,
    size: [u64; 2],
    row: u64,
    case: &'static str,
    metal: &'static str,
) -> Over {
    Over {
        ostype: None,
        size,
        format,
        plane: 0,
        length: row * size[1] + 256,
        planes: [(0, row), (0, 0)],
        case,
        metal,
    }
}

/// Metal's fills over a surface's plane or a buffer (`cases.sh`).
const OVER: [Over; 19] = [
    surf(b"420v", [8, 4], (500, 0), S8X4, "b4",
        " [0x200+8: 01 01 01 01 01 01 01 01] [0x240+8: 01 01 01 01 01 01 01 01] [0x280+8: 01 01 01 01 01 01 01 01] [0x2c0+8: 01 01 01 01 01 01 01 01]"),
    surf(b"420v", [8, 4], (500, 0), S8X4, "x5,2,3", " [0x285+3: 01 01 01] [0x2c5+3: 01 01 01]"),
    surf(b"420v", [8, 4], (520, 0), S8X4, "p70",
        " [0x200+8: 40 40 40 40 40 40 40 40] [0x240+8: 40 40 40 40 40 40 40 40] [0x280+8: 40 40 40 40 40 40 40 40] [0x2c0+8: 40 40 40 40 40 40 40 40]"),
    surf(b"x420", [8, 4], (505, 0), S8X4, "b4",
        " [0x200+16: 01 02 01 02 01 02 01 02 01 02 01 02 01 02 01 02] [0x240+16: 01 02 01 02 01 02 01 02 01 02 01 02 01 02 01 02] [0x280+16: 01 02 01 02 01 02 01 02 01 02 01 02 01 02 01 02] [0x2c0+16: 01 02 01 02 01 02 01 02 01 02 01 02 01 02 01 02]"),
    surf(b"x420", [8, 4], (505, 0), S8X4, "x5,2,3", " [0x28a+6: 01 02 01 02 01 02] [0x2ca+6: 01 02 01 02 01 02]"),
    surf(b"x420", [8, 4], (505, 0), S8X4, "p70",
        " [0x200+16: 40 80 40 80 40 80 40 80 40 80 40 80 40 80 40 80] [0x240+16: 40 80 40 80 40 80 40 80 40 80 40 80 40 80 40 80] [0x280+16: 40 80 40 80 40 80 40 80 40 80 40 80 40 80 40 80] [0x2c0+16: 40 80 40 80 40 80 40 80 40 80 40 80 40 80 40 80]"),
    surf(b"x420", [8, 4], (543, 0), S8X4, "p20",
        " [0x200+16: 00 40 00 40 00 40 00 40 00 40 00 40 00 40 00 40] [0x240+16: 00 40 00 40 00 40 00 40 00 40 00 40 00 40 00 40] [0x280+16: 00 40 00 40 00 40 00 40 00 40 00 40 00 40 00 40] [0x2c0+16: 00 40 00 40 00 40 00 40 00 40 00 40 00 40 00 40]"),
    surf(b"x420", [38, 6], (505, 0), (2128, [(512, 128), (1536, 128)]), "x35,4,3",
        " [0x446+6: 01 02 01 02 01 02] [0x4c6+6: 01 02 01 02 01 02]"),
    surf(b"xf20", [8, 4], (525, 0), S8X4, "x6,3,3", " [0x2cc+4: 01 02 01 02]"),
    surf(b"444v", [8, 4], (503, 1), S8X4_444, "b4",
        " [0x400+8: 01 01 01 01 01 01 01 01] [0x440+8: 01 01 01 01 01 01 01 01] [0x480+8: 01 01 01 01 01 01 01 01] [0x4c0+8: 01 01 01 01 01 01 01 01]"),
    surf(b"x444", [8, 4], (507, 1), S8X4_444, "x1,1,3", " [0x442+6: 01 02 01 02 01 02] [0x482+6: 01 02 01 02 01 02]"),
    surf(b"x422", [8, 4], (544, 0), S8X4_444, "b3",
        " [0x200+16: 01 02 01 02 01 02 01 02 01 02 01 02 01 02 01 02] [0x240+16: 01 02 01 02 01 02 01 02 01 02 01 02 01 02 01 02] [0x280+16: 01 02 01 02 01 02 01 02 01 02 01 02 01 02 01 02] [0x2c0+16: 01 02 01 02 01 02 01 02 01 02 01 02 01 02 01 02]"),
    surf(b"p420", [12, 4], (508, 0), S8X4, "x5,2,1", " [0x285+3: 00 00 00] [0x2c5+3: 00 00 00]"),
    surf(b"p444", [12, 4], (510, 0), S8X4_444, "p10",
        " [0x200+12: 00 00 00 00 00 00 00 00 00 00 00 00] [0x240+12: 00 00 00 00 00 00 00 00 00 00 00 00] [0x280+12: 00 00 00 00 00 00 00 00 00 00 00 00] [0x2c0+12: 00 00 00 00 00 00 00 00 00 00 00 00]"),
    surf(b"p422", [8, 4], (529, 0), S8X4_444, "b4",
        " [0x200+8: 00 00 00 00 00 00 00 00] [0x240+8: 00 00 00 00 00 00 00 00] [0x280+8: 00 00 00 00 00 00 00 00] [0x2c0+8: 00 00 00 00 00 00 00 00]"),
    buf(576, [8, 4], 64, "x5,2,3", " [0x8a+6: 01 02 01 02 01 02] [0xca+6: 01 02 01 02 01 02]"),
    buf(577, [7, 3], 32, "b4",
        " [0+14: 01 02 01 02 01 02 01 02 01 02 01 02 01 02] [0x20+14: 01 02 01 02 01 02 01 02 01 02 01 02 01 02] [0x40+14: 01 02 01 02 01 02 01 02 01 02 01 02 01 02]"),
    buf(578, [8, 4], 64, "r3", " [0x44+8: 01 02 03 01 01 02 03 01]"),
    // Each row's 32 bytes are the packed color eight times. `surface.m`'s form cut every run at
    // 24, so the last eight bytes of each were in no comparison; `probes/surface_buffer_fills.m`
    // measured the row whole on this chip (three runs, the same bytes each time).
    buf(579, [8, 4], 64, "p70",
        " [0+32: 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff] [0x40+32: 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff] [0x80+32: 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff] [0xc0+32: 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff 40 80 bf ff]"),
];

/// One fill of a two-plane extended-range texture (`tp.m`'s arguments): its format, extent,
/// levels and slices, the fill's level, slice and region, whether in a heap, and the lines
/// Metal's probe printed.
struct Planes {
    format: u32,
    extent: [u64; 2],
    levels: u64,
    slices: u64,
    level: u64,
    slice: u64,
    region: [u64; 4],
    heap: bool,
    metal: &'static [&'static str],
}

const fn planes(
    format: u32,
    (extent, levels, slices): ([u64; 2], u64, u64),
    (level, slice, region): (u64, u64, [u64; 4]),
    heap: bool,
    metal: &'static [&'static str],
) -> Planes {
    Planes {
        format,
        extent,
        levels,
        slices,
        level,
        slice,
        region,
        heap,
        metal,
    }
}

/// Metal's fills of two-plane extended-range textures (`lane-cases.sh`).
const PLANES: [Planes; 10] = [
    planes(550, ([5, 3], 1, 1), (0, 0, [0, 0, 5, 3]), false, &[
        "slice 0 level 0 (5x3): 0,0,0 1,0,0 2,0,0 3,0,0 4,0,0 0,1,0 1,1,0 2,1,0 3,1,0 4,1,0",
    ]),
    planes(550, ([5, 3], 1, 1), (0, 0, [1, 1, 2, 1]), false, &["slice 0 level 0 (5x3):"]),
    planes(551, ([7, 5], 1, 1), (0, 0, [2, 1, 3, 3]), false, &[
        "slice 0 level 0 (7x5): 6,0,0 0,1,0 2,1,0 3,1,0 4,1,0 6,1,0 0,2,0",
    ]),
    planes(550, ([3, 4], 1, 1), (0, 0, [0, 1, 2, 2]), false, &[
        "slice 0 level 0 (3x4): 0,1,0 1,1,0 0,2,0 1,2,0",
    ]),
    planes(550, ([8, 8], 3, 1), (1, 0, [0, 0, 4, 4]), false, &[
        "slice 0 level 0 (8x8): 0,0,0 1,0,0 2,0,0 3,0,0 0,1,0 1,1,0 2,1,0 3,1,0 0,2,0 1,2,0 2,2,0 3,2,0 0,3,0 1,3,0 2,3,0 3,3,0",
        "slice 0 level 1 (4x4):",
        "slice 0 level 2 (2x2):",
    ]),
    planes(551, ([6, 4], 1, 2), (0, 1, [1, 0, 3, 2]), false, &[
        "slice 0 level 0 (6x4):",
        "slice 1 level 0 (6x4): 1,0,0 2,0,0 3,0,0 5,0,0",
    ]),
    planes(550, ([5, 3], 1, 1), (0, 0, [0, 0, 5, 3]), true, &[
        "slice 0 level 0 (5x3): 0,0,0 1,0,0 2,0,0 3,0,0 4,0,0 0,1,0 1,1,0 2,1,0 3,1,0 4,1,0",
    ]),
    planes(550, ([8, 8], 3, 1), (2, 0, [0, 0, 1, 1]), true, &[
        "slice 0 level 0 (8x8): 0,0,0",
        "slice 0 level 1 (4x4):",
        "slice 0 level 2 (2x2):",
    ]),
    planes(551, ([6, 4], 1, 2), (0, 1, [1, 0, 3, 2]), true, &[
        "slice 0 level 0 (6x4):",
        "slice 1 level 0 (6x4): 1,0,0 2,0,0 3,0,0 5,0,0",
    ]),
    planes(550, ([9, 2], 1, 1), (0, 0, [3, 0, 100, 100]), false, &[
        "slice 0 level 0 (9x2): 3,0,0 4,0,0 5,0,0 6,0,0 7,0,0 8,0,0 0,1,0 1,1,0 2,1,0 3,1,0 4,1,0",
    ]),
];

fn format(raw: u32) -> PixelFormat {
    PixelFormat::try_from(raw).unwrap()
}

/// `bytes`' runs of bytes other than `0xEE`, as `probes/surface_buffer_fills.m` prints them: each
/// ` [<at>+<n>:`, every one of its bytes, and `]`. Printing all of them is the point: the form
/// `surface.m` printed cut each run at 24 bytes and wrote ` ..`, so the bytes past the 24th of a
/// 32-byte run were compared by nothing, inside a passing test.
fn runs(bytes: &[u8]) -> String {
    let mut out = String::new();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == 0xee {
            i += 1;
            continue;
        }
        let mut j = i;
        while j < bytes.len() && bytes[j] != 0xee {
            j += 1;
        }
        let at = if i == 0 {
            "0".to_owned()
        } else {
            format!("{i:#x}")
        };
        out += &format!(" [{at}+{}:", j - i);
        for b in &bytes[i..j] {
            out += &format!(" {b:02x}");
        }
        out += "]";
        i = j;
    }
    out
}

/// Every byte of `bytes` that a fill changed has a two-digit number in `runs`' `text`: the
/// property a truncating formatter breaks.
fn covers(text: &str, bytes: &[u8], case: &str) {
    // A run prints ` [<at>+<n>:` and then one number a byte, so the numbers after a run's colon
    // are the bytes of it the comparison holds.
    let printed: usize = text
        .split('[')
        .skip(1)
        .map(|run| {
            run.split_once(':').map_or(0, |(_, listed)| {
                (listed.split_whitespace())
                    .filter(|word| {
                        let word = word.strip_suffix(']').unwrap_or(word);
                        word.len() == 2 && word.bytes().all(|b| b.is_ascii_hexdigit())
                    })
                    .count()
            })
        })
        .sum();
    let changed = bytes.iter().filter(|&&b| b != 0xee).count();
    assert_eq!(
        printed, changed,
        "{case}: the compared runs print {printed} of the {changed} bytes the fill changed"
    );
}

/// The fill `case` (as [`Over::case`] reads) into `texture` of `width` by `height`.
fn fill_case(
    blit: &mut metal2vulkan_runtime::BlitCommandEncoder,
    texture: &Texture,
    [width, height]: [u64; 2],
    case: &str,
) -> Result<(), Error> {
    let pattern: Vec<u8> = (1..=64).collect();
    let number = |s: &str| s.parse::<u64>().unwrap();
    let (kind, rest) = case.split_at(1);
    match kind {
        "b" => blit.fill_texture(
            texture,
            0,
            0,
            Region::new_2d(0, 0, width, height),
            &pattern[..number(rest) as usize],
        ),
        "r" => blit.fill_texture(
            texture,
            0,
            0,
            Region::new_2d(1, 1, 2, 1),
            &pattern[..number(rest) as usize],
        ),
        "x" => {
            let v: Vec<u64> = rest.split(',').map(number).collect();
            blit.fill_texture(
                texture,
                0,
                0,
                Region::new_2d(v[0], v[1], 3, 2),
                &pattern[..v[2] as usize],
            )
        }
        "p" => blit.fill_texture_color(
            texture,
            0,
            0,
            Region::new_2d(0, 0, width, height),
            ClearColor::new(0.25, 0.5, 0.75, 1.0),
            Some(format(number(rest) as u32)),
        ),
        _ => unreachable!("no case {case}"),
    }
}

/// `over`'s texture: over its surface's plane (the plane's size: the surface's, or a 4:4:4
/// surface's chroma plane, the same), or over its buffer.
fn over_texture(over: &Over, surface: &metal2vulkan_runtime::Buffer) -> Texture {
    let [w, h] = over.size;
    let mut d = TextureDescriptor::texture_2d(format(over.format), w, h, false);
    d.resource_options = ResourceOptions::shared();
    let made = match over.ostype {
        Some(ostype) => {
            d.usage = TextureUsage::SHADER_READ;
            let (offset, row) = over.planes[over.plane as usize];
            let [_, (chroma, chroma_row)] = over.planes;
            let facts = IoSurfaceYCbCr::new(
                u32::from_be_bytes(*ostype),
                over.plane,
                over.size,
                chroma,
                chroma_row,
            );
            let plane = IoSurfacePlane::new(offset, row, true).with_ycbcr(facts);
            surface.new_texture_with_iosurface_plane(&d, plane)
        }
        None => {
            d.usage = TextureUsage::SHADER_READ | TextureUsage::SHADER_WRITE;
            surface.new_texture(&d, 0, over.planes[0].1)
        }
    };
    made.unwrap_or_else(|e| panic!("{} {}: {e}", over.format, over.case))
}

/// Each fill over a surface's plane or a buffer changes the bytes Metal's changes, and only
/// those.
#[test]
fn fills_over_surfaces_and_buffers_write_metal_s_bytes() {
    on_device(|device, _messages| {
        let queue = device.new_command_queue().unwrap();
        let mut wrong = Vec::new();
        for over in &OVER {
            let surface = buffer_with(device, &vec![0xee; over.length as usize]).unwrap();
            let texture = over_texture(over, &surface);
            let command_buffer = queue.command_buffer().unwrap();
            let mut blit = command_buffer.blit_command_encoder().unwrap();
            let size = [texture.width(), texture.height()];
            fill_case(&mut blit, &texture, size, over.case)
                .unwrap_or_else(|e| panic!("{} {}: {e}", over.format, over.case));
            blit.end_encoding().unwrap();
            command_buffer.commit().unwrap();
            command_buffer.wait_until_completed_for(WAIT).unwrap();
            let read = read_buffer(&surface, 0, over.length).unwrap();
            let ours = runs(&read);
            covers(&ours, &read, over.case);
            if ours != over.metal {
                println!(
                    "SURFACE-FILLS-DIFFERS {} {}:{ours} (Metal's{})",
                    over.format, over.case, over.metal
                );
                wrong.push((over.format, over.case));
            }
        }
        assert!(wrong.is_empty(), "fills unlike Metal's: {wrong:?}");
    });
}

/// A level's extent of `extent`.
fn level_extent([w, h]: [u64; 2], level: u64) -> [u64; 2] {
    [(w >> level).max(1), (h >> level).max(1)]
}

/// The word `tp.m` writes at texel `i` of `level` and `slice`.
fn index_word(slice: u64, level: u64, i: u64) -> u32 {
    (0x1000_0000 * (slice + 1) + 0x0100_0000 * level + i) as u32
}

/// `case`'s texture: of its own memory, or at byte 0 of a placement heap of Metal's size.
fn planes_texture(device: &Device, case: &Planes) -> Texture {
    let [w, h] = case.extent;
    let mut d = TextureDescriptor::texture_2d(format(case.format), w, h, false);
    d.mipmap_level_count = case.levels;
    if case.slices > 1 {
        d.texture_type = TextureType::Type2DArray;
        d.array_length = case.slices;
    }
    d.usage = TextureUsage::SHADER_READ;
    d.resource_options = ResourceOptions::shared();
    if !case.heap {
        return device.new_texture(&d).unwrap();
    }
    let size = device.heap_texture_size_and_align(&d).unwrap().size;
    let heap = device
        .new_heap(&HeapDescriptor::new(
            HeapType::Placement,
            size,
            ResourceOptions::shared(),
        ))
        .unwrap();
    heap.new_texture_at_offset(&d, 0).unwrap()
}

/// `case`'s lines as `tp.m` prints them: each slice's and level's texels whose word changed.
fn planes_case(device: &Device, queue: &CommandQueue, case: &Planes) -> Vec<String> {
    let texture = planes_texture(device, case);
    let each = || {
        (0..case.slices)
            .flat_map(|s| (0..case.levels).map(move |l| (s, l, level_extent(case.extent, l))))
    };
    for (slice, level, [w, h]) in each() {
        let words: Vec<u8> = (0..w * h)
            .flat_map(|i| index_word(slice, level, i).to_le_bytes())
            .collect();
        texture
            .replace_region(
                Region::new_2d(0, 0, w, h),
                level,
                slice,
                &words,
                4 * w,
                4 * w * h,
            )
            .unwrap();
    }
    let command_buffer = queue.command_buffer().unwrap();
    let mut blit = command_buffer.blit_command_encoder().unwrap();
    let [x, y, rw, rh] = case.region;
    blit.fill_texture(
        &texture,
        case.level,
        case.slice,
        Region::new_2d(x, y, rw, rh),
        &[0xa1, 0xa2, 0xa3, 0xa4],
    )
    .unwrap();
    blit.end_encoding().unwrap();
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    each()
        .map(|(slice, level, [w, h])| {
            let mut words = vec![0; (4 * w * h) as usize];
            texture
                .get_bytes(
                    &mut words,
                    4 * w,
                    4 * w * h,
                    Region::new_2d(0, 0, w, h),
                    level,
                    slice,
                )
                .unwrap();
            let mut line = format!("slice {slice} level {level} ({w}x{h}):");
            for (i, word) in words.as_chunks::<4>().0.iter().enumerate() {
                let (i, word) = (i as u64, u32::from_le_bytes(*word));
                let at = format!("{},{},0", i % w, i / w);
                if word == 0xa4a3_a2a1 {
                    line += &format!(" {at}");
                } else if word != index_word(slice, level, i) {
                    line += &format!(" {at}={word:08x}");
                }
            }
            line
        })
        .collect()
}

/// Each fill of a two-plane extended-range texture, of its own memory or a heap's, changes the
/// texels Metal's changes as `get_bytes` reads them, and only those.
#[test]
fn two_plane_fills_write_metal_s_texels() {
    on_device(|device, _messages| {
        let queue = device.new_command_queue().unwrap();
        let mut wrong = Vec::new();
        for (n, case) in PLANES.iter().enumerate() {
            let ours = planes_case(device, &queue, case);
            if ours != case.metal {
                println!(
                    "SURFACE-FILLS-DIFFERS planes {n}: {ours:?} (Metal's {:?})",
                    case.metal
                );
                wrong.push(n);
            }
        }
        assert!(wrong.is_empty(), "fills unlike Metal's: {wrong:?}");
    });
}

/// A 1-texel-high texture of `pf` and `width`, every byte `0x5A` (`slot.m`'s).
fn set_texture(
    device: &Device,
    queue: &CommandQueue,
    pf: PixelFormat,
    width: u64,
    texel: u64,
) -> Texture {
    let mut d = TextureDescriptor::texture_2d(pf, width, 1, false);
    d.usage = TextureUsage::SHADER_READ | TextureUsage::SHADER_WRITE;
    d.resource_options = ResourceOptions::private();
    let texture = device.new_texture(&d).unwrap();
    let bytes = buffer_with(device, &vec![0x5a; (width * texel) as usize]).unwrap();
    let command_buffer = queue.command_buffer().unwrap();
    let mut blit = command_buffer.blit_command_encoder().unwrap();
    blit.copy_from_buffer_to_texture(
        &bytes,
        0,
        width * texel,
        width * texel,
        Size::new(width, 1, 1),
        &texture,
        0,
        0,
        Origin::new(0, 0, 0),
        BlitOption::None,
    )
    .unwrap();
    blit.end_encoding().unwrap();
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    texture
}

/// `texture`'s bytes (`width` texels of `texel` bytes) through a blit into a buffer.
fn texture_bytes(device: &Device, queue: &CommandQueue, texture: &Texture, texel: u64) -> Vec<u8> {
    let length = texture.width() * texel;
    let out = buffer_with(device, &vec![0; length as usize]).unwrap();
    let command_buffer = queue.command_buffer().unwrap();
    let mut blit = command_buffer.blit_command_encoder().unwrap();
    blit.copy_from_texture_to_buffer(
        texture,
        0,
        0,
        Origin::new(0, 0, 0),
        Size::new(texture.width(), 1, 1),
        &out,
        0,
        length,
        length,
        BlitOption::None,
    )
    .unwrap();
    blit.end_encoding().unwrap();
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    read_buffer(&out, 0, length).unwrap()
}

/// A color fill packed where Metal packs no color fills the 16-byte texel of the encoder's last
/// packed fill, cut at the texture's texel and overwritten by a shorter one's bytes, as Metal's
/// warm runs write it with fills alone between (`slot.m`'s sequences `same`, `short`, `into8`
/// and `twice`); a fill packed in `BC1_RGBA` likewise.
#[test]
fn unpacked_color_fills_repeat_the_last_packed_texel() {
    on_device(|device, _messages| {
        let queue = device.new_command_queue().unwrap();
        let earlier = ClearColor::new(7.0, 9.0, 3.0, 1.0);
        let asked = ClearColor::new(0.25, 0.5, 0.75, 1.0);
        let whole = |t: &Texture| Region::new_2d(0, 0, t.width(), 1);
        let rgba32 = "07000000090000000300000001000000";
        let sequences: [(&str, [&str; 3]); 4] = [
            (
                "same",
                [rgba32, "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a", "5a5a5a5a"],
            ),
            (
                "short",
                [
                    "ffffffff090000000300000001000000",
                    "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a",
                    "5a5a5a5a",
                ],
            ),
            (
                "into8",
                [rgba32, "07000000070000000700000007000000", "07070707"],
            ),
            (
                "twice",
                [rgba32, "07000000070000000700000007000000", "5a5a5a5a"],
            ),
        ];
        for (name, metal) in sequences {
            let src = set_texture(device, &queue, PixelFormat::RGBA32Uint, 1, 16);
            let other = set_texture(device, &queue, PixelFormat::RGBA8Unorm, 4, 4);
            let t = set_texture(device, &queue, PixelFormat::RGBA32Uint, 1, 16);
            let t8 = set_texture(device, &queue, PixelFormat::RGBA8Unorm, 4, 4);
            let r8 = set_texture(device, &queue, PixelFormat::R8Unorm, 4, 1);
            let command_buffer = queue.command_buffer().unwrap();
            let mut blit = command_buffer.blit_command_encoder().unwrap();
            let mut fc = |texture: &Texture, color, pf| {
                blit.fill_texture_color(texture, 0, 0, whole(texture), color, Some(pf))
                    .unwrap();
            };
            let none = PixelFormat::Depth32Float;
            fc(&src, earlier, PixelFormat::RGBA32Uint);
            match name {
                "same" => fc(&t, asked, none),
                "short" => {
                    fc(
                        &other,
                        ClearColor::new(1.0, 1.0, 1.0, 1.0),
                        PixelFormat::RGBA8Unorm,
                    );
                    fc(&t, asked, none);
                }
                "into8" => {
                    fc(&t8, asked, none);
                    fc(&r8, asked, none);
                    fc(&t, asked, none);
                }
                _ => {
                    fc(&t, asked, none);
                    fc(
                        &t8,
                        ClearColor::new(0.5, 0.5, 0.5, 0.5),
                        PixelFormat::BC1RGBA,
                    );
                }
            }
            blit.end_encoding().unwrap();
            command_buffer.commit().unwrap();
            command_buffer.wait_until_completed_for(WAIT).unwrap();
            let hex = |bytes: Vec<u8>| bytes.iter().map(|b| format!("{b:02x}")).collect::<String>();
            let ours = [
                hex(texture_bytes(device, &queue, &t, 16)),
                hex(texture_bytes(device, &queue, &t8, 4)),
                hex(texture_bytes(device, &queue, &r8, 1)),
            ];
            assert_eq!(ours, metal.map(str::to_owned), "{name}");
        }
    });
}

/// A fill of no pattern bytes is refused the runtime's own gap -- plain Metal takes the fill and
/// reads a unit's bytes from the pattern's address, which the runtime holds none of -- and writes
/// nothing.
#[test]
fn an_empty_pattern_is_refused() {
    on_device(|device, _messages| {
        let queue = device.new_command_queue().unwrap();
        let t = set_texture(device, &queue, PixelFormat::RGBA8Unorm, 4, 4);
        let command_buffer = queue.command_buffer().unwrap();
        let mut blit = command_buffer.blit_command_encoder().unwrap();
        let refused = blit.fill_texture(&t, 0, 0, Region::new_2d(0, 0, 4, 1), &[]);
        assert_eq!(
            refused,
            Err(Error::NotImplemented(RuntimeGap::EmptyFillPattern))
        );
        blit.end_encoding().unwrap();
        command_buffer.commit().unwrap();
        command_buffer.wait_until_completed_for(WAIT).unwrap();
        assert_eq!(texture_bytes(device, &queue, &t, 4), vec![0x5a; 16]);
    });
}
