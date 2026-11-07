//! Metal's private texture fills of texture buffers (`BlitCommandEncoder::fill_texture` and
//! `fill_texture_color`), on the device the cfg selects, under the validation layer, against
//! Metal's bytes.
//!
//! Each case replays our probe `tb.m`'s (an Apple M3 Max under macOS 27.0.1): a texture buffer
//! 12 texels wide, of a format of 1 to 16 bytes a texel, at an offset into a `Shared` buffer of
//! 512 bytes whose byte `i` is first `i · 37 mod 251` (256 bytes a row), its fills in one
//! encoder, and every byte of the buffer read back. A fill is the probe's argument
//! `x,w[,y,h,d,level,slice,z]:` (`y` 0, `h` and `d` 1, the rest 0 where left out) and its pattern:
//! `p<n>` the `n` bytes `a1 a2 ..`, `p<n>@<b>` from the byte `b`, `c<pf>` the color
//! (0.25, -0.5, 1.25, 0.75) packed in `pf`. Metal's answer is the runs of bytes its fills changed
//! ([`METAL_RUNS`], one line a case), beside the first 16 hex digits of the SHA-256 of the
//! buffer's bytes the probe printed; a group's digest, of its cases' buffers in order. A differing
//! case names its first differing byte.
//!
//! - **Streams**: patterns longer than the texel, of lengths not a power of two, and up to 256
//!   bytes; regions past the texture's width, past the buffer's end, of no rows, of no images, at
//!   a level, slice, row and depth past 0, and of no texels; a fill after a fill (the consumer's 9
//!   cases among them). Metal writes the buffer's bytes, the pattern across them, and the bytes of
//!   its block past the pattern's copies as zeros, which the runtime writes and announces once
//!   (`texture-buffer-fill-leftover`).
//! - **Groups** (the consumer's set): each format at offsets 0 and 64, and 4 and 68 for a texel of
//!   4 bytes or fewer, a pattern of each power of two of bytes up to the texel whole, inside,
//!   ending at the width and at a level, slice, row, height and depth past 0 and 1, a color packed
//!   in each format of no more bytes than the texel, and fills of no texels between others.
//!
//! - **Long patterns** (our probe `tblf.m`, the same M3 Max): patterns of 4097 to 32752 bytes, a
//!   xorshift stream, into texture buffers of `R8Uint`, `R32Uint` and `RGBA32Uint` up to 65,536
//!   texels wide, at offsets 4 to 64 of a `Shared` buffer 64 bytes past the texels, its byte `i`
//!   first `i · 37 mod 251`, one fill of every texel, every byte read back: Metal writes them by
//!   the same rule. A pattern of more than 32752 bytes ends Metal's process in the call, which
//!   the runtime refuses (`Problem::LongTextureBufferFillPattern`).
//!
//! `tests::blit_fill` checks the bytes of the fills without a device. A differing case prints a
//! `TEXTURE-BUFFER-FILLS-DIFFERS` line with the runs of bytes its fills changed.

use super::degrade_notices::{
    assert_once, forced, on_device, shared_in_system_memory, TEXTURE_BUFFER_FILL,
};
use super::{buffer_with, read_buffer, WAIT};
use metal2vulkan::metal::{PixelFormat, TextureUsage};
use metal2vulkan_runtime::{
    ClearColor, CommandQueue, Degradation, Device, Error, Invalid, Origin, Problem, Region,
    ResourceOptions, Size, TextureDescriptor,
};
use sha2::{Digest, Sha256};

/// The buffer's bytes.
const BYTES: usize = 512;

/// Metal's bytes of every stream and group case, as the runs its fills changed in the buffer's
/// first bytes: a line `<case> | <offset>: <bytes> | ...` each, the case named as the tables name
/// it (a group's case `<group>#<k>`), recovered on an Apple M3 Max under MoltenVK beside the
/// digests the probes printed.
const METAL_RUNS: &str = include_str!("../../fixtures/public/texture_buffer_fills/metal_runs.txt");

/// The SHA-256 of [`METAL_RUNS`].
const METAL_RUNS_SHA: &str = "74e7623a2f205ae68228c27eef22d0817b3f1c10baa4ed15c3dd0e95c65d6847";

/// A stream: its name, the format's raw value, the texture's offset, its fills, and Metal's
/// answer.
type Stream = (
    &'static str,
    u32,
    u64,
    &'static [&'static str],
    &'static str,
);

/// A group: its name, the format's raw value, the texture's offset, its cases' fills, and
/// Metal's answer.
type Group = (
    &'static str,
    u32,
    u64,
    &'static [&'static [&'static str]],
    &'static str,
);

/// The streams, as the probe ran them.
const STREAMS: [Stream; 26] = [
    ("r8-8-whole", 10, 0, &["0,12:p8"], "0ee2aff21c7fc968"),
    ("r8-8-odd", 10, 4, &["3,5:p8"], "4e2a928549345a34"),
    ("rgba8-8-whole", 70, 0, &["0,12:p8"], "84f5d37ca71f4e7f"),
    ("rgba8-8-x4", 70, 64, &["4,8:p8"], "5a2c0a26da0f6e23"),
    ("rgba8-c115", 70, 0, &["4,6:c115"], "4db1e92e18d57d38"),
    ("r8-past-width", 10, 0, &["12,3:p1"], "0260417c8c16e763"),
    ("rgba8-ending-past", 70, 0, &["10,5:p4"], "e78f2f4f540bb93d"),
    ("r8-no-rows", 10, 0, &["1,2,0,0,1:p1"], "4bd9db7e5278e209"),
    (
        "rgba8-no-images",
        70,
        64,
        &["1,2,0,1,0:p4"],
        "85a8c2ef070cc49a",
    ),
    ("rgba8-3-whole", 70, 0, &["0,12:p3"], "e3999e838bee0e1c"),
    (
        "rgba8-9-after-8",
        70,
        0,
        &["0,12:p8", "0,12:p9@b1"],
        "456210529ae74916",
    ),
    ("rgba8-12-x1", 70, 4, &["1,10:p12"], "19ce1fc8bdec16f0"),
    ("rgba32-17", 125, 0, &["0,12:p17"], "d31c4c6fb6e0e200"),
    ("rgba32-24-x1", 125, 64, &["1,10:p24"], "2178521d4be0aa92"),
    ("rgba8-33-past", 70, 0, &["0,40:p33"], "5f7b1f135056b512"),
    ("r8-65-long", 10, 0, &["3,300:p65"], "3366332bf93b2371"),
    ("rgba8-100", 70, 4, &["1,100:p100"], "8d688b0eca7b3728"),
    ("rgba8-256", 70, 0, &["0,100:p256"], "9096b69c455dc4e0"),
    ("rg8-5-x6", 30, 0, &["6,40:p5"], "f2b4ec3d60883b4e"),
    ("rgba16-24", 115, 64, &["3,30:p24"], "53fb1bd384886492"),
    (
        "rgba32-past-end",
        125,
        64,
        &["20,20:p16"],
        "b11323ede905084e",
    ),
    ("r8-past-buffer", 10, 0, &["600,3:p1"], "d71bdb461ee51a7e"),
    ("rgba8-width-0", 70, 0, &["2,0:p3"], "d71bdb461ee51a7e"),
    (
        "rgba8-level-slice-z",
        70,
        4,
        &["2,4,5,3,2,3,7,4:p3"],
        "c8766e71a2d1ea36",
    ),
    (
        "rgba8-c115-then-12",
        70,
        0,
        &["0,12:c115", "0,12:p12"],
        "f5e35d82b4b3bcd8",
    ),
    ("r8-3-odd", 10, 4, &["1,30:p3"], "8faf563f27272b2b"),
];

/// The fills of a group of `r8` texels, one case each.
const R8: &[&[&str]] = &[
    &["0,12:p1"],
    &["3,5:p1"],
    &["7,5:p1"],
    &["2,4,5,3,2,3,7,4:p1"],
    &["1,6,4,2,1,1,2,0:c10"],
    &["0,12:p1", "5,0:p1", "12,0:p1", "9,2:c10"],
];

/// The fills of a group of `rg8` texels, one case each.
const RG8: &[&[&str]] = &[
    &["0,12:p1"],
    &["3,5:p1"],
    &["7,5:p1"],
    &["2,4,5,3,2,3,7,4:p1"],
    &["0,12:p2"],
    &["3,5:p2"],
    &["7,5:p2"],
    &["2,4,5,3,2,3,7,4:p2"],
    &["1,6,4,2,1,1,2,0:c10"],
    &["1,6,4,2,1,1,2,0:c30"],
    &["0,12:p1", "5,0:p1", "12,0:p1", "9,2:c10"],
];

/// The fills of a group of `rgba8` texels, one case each.
const RGBA8: &[&[&str]] = &[
    &["0,12:p1"],
    &["3,5:p1"],
    &["7,5:p1"],
    &["2,4,5,3,2,3,7,4:p1"],
    &["0,12:p2"],
    &["3,5:p2"],
    &["7,5:p2"],
    &["2,4,5,3,2,3,7,4:p2"],
    &["0,12:p4"],
    &["3,5:p4"],
    &["7,5:p4"],
    &["2,4,5,3,2,3,7,4:p4"],
    &["1,6,4,2,1,1,2,0:c10"],
    &["1,6,4,2,1,1,2,0:c30"],
    &["1,6,4,2,1,1,2,0:c70"],
    &["0,12:p1", "5,0:p1", "12,0:p1", "9,2:c10"],
];

/// The fills of a group of `rgba16` texels, one case each.
const RGBA16: &[&[&str]] = &[
    &["0,12:p1"],
    &["3,5:p1"],
    &["7,5:p1"],
    &["2,4,5,3,2,3,7,4:p1"],
    &["0,12:p2"],
    &["3,5:p2"],
    &["7,5:p2"],
    &["2,4,5,3,2,3,7,4:p2"],
    &["0,12:p4"],
    &["3,5:p4"],
    &["7,5:p4"],
    &["2,4,5,3,2,3,7,4:p4"],
    &["0,12:p8"],
    &["3,5:p8"],
    &["7,5:p8"],
    &["2,4,5,3,2,3,7,4:p8"],
    &["1,6,4,2,1,1,2,0:c10"],
    &["1,6,4,2,1,1,2,0:c30"],
    &["1,6,4,2,1,1,2,0:c70"],
    &["1,6,4,2,1,1,2,0:c115"],
    &["0,12:p1", "5,0:p1", "12,0:p1", "9,2:c10"],
];

/// The fills of a group of `rgba32` texels, one case each.
const RGBA32: &[&[&str]] = &[
    &["0,12:p1"],
    &["3,5:p1"],
    &["7,5:p1"],
    &["2,4,5,3,2,3,7,4:p1"],
    &["0,12:p2"],
    &["3,5:p2"],
    &["7,5:p2"],
    &["2,4,5,3,2,3,7,4:p2"],
    &["0,12:p4"],
    &["3,5:p4"],
    &["7,5:p4"],
    &["2,4,5,3,2,3,7,4:p4"],
    &["0,12:p8"],
    &["3,5:p8"],
    &["7,5:p8"],
    &["2,4,5,3,2,3,7,4:p8"],
    &["0,12:p16"],
    &["3,5:p16"],
    &["7,5:p16"],
    &["2,4,5,3,2,3,7,4:p16"],
    &["1,6,4,2,1,1,2,0:c10"],
    &["1,6,4,2,1,1,2,0:c30"],
    &["1,6,4,2,1,1,2,0:c70"],
    &["1,6,4,2,1,1,2,0:c115"],
    &["1,6,4,2,1,1,2,0:c125"],
    &["0,12:p1", "5,0:p1", "12,0:p1", "9,2:c10"],
];

/// The groups, as the probe ran them.
const GROUPS: [Group; 16] = [
    ("r8-at-0", 10, 0, R8, "715e234bffb7dcf7"),
    ("r8-at-4", 10, 4, R8, "e1f8ea12e12b9db9"),
    ("r8-at-64", 10, 64, R8, "de7442c0bb4f290f"),
    ("r8-at-68", 10, 68, R8, "1a85c929b8d80219"),
    ("rg8-at-0", 30, 0, RG8, "b8fcc13a29cea9c1"),
    ("rg8-at-4", 30, 4, RG8, "62103fb7d6791aea"),
    ("rg8-at-64", 30, 64, RG8, "fa35a4670beb6d3a"),
    ("rg8-at-68", 30, 68, RG8, "9ddd3dab429ace88"),
    ("rgba8-at-0", 70, 0, RGBA8, "94b6756df9a34a32"),
    ("rgba8-at-4", 70, 4, RGBA8, "dde8bea1606de5aa"),
    ("rgba8-at-64", 70, 64, RGBA8, "14795fa2287dd1c0"),
    ("rgba8-at-68", 70, 68, RGBA8, "bf7d8a1f8a105af8"),
    ("rgba16-at-0", 115, 0, RGBA16, "f9d0a1f338d87e0b"),
    ("rgba16-at-64", 115, 64, RGBA16, "c2a934eb4a41e124"),
    ("rgba32-at-0", 125, 0, RGBA32, "c9332419d8b8bfad"),
    ("rgba32-at-64", 125, 64, RGBA32, "ebaf73cdcde2c014"),
];

/// The longest pattern a texture buffer's fill takes, Metal's.
const LONGEST: usize = 32752;

/// The long patterns' fills (`tblf.m`): the format's raw value, its texel's bytes, the texture's
/// offset and width, the pattern's bytes, and Metal's answer.
const LONG: [(u32, u64, u64, u64, usize, &str); 6] = [
    (13, 1, 4, 400, LONGEST, "fb41732d0bf8d61d"),
    (13, 1, 4, 12331, 4097, "270ffc1b247b7363"),
    (13, 1, 4, 49195, 16385, "b9e0af569f1c7db6"),
    (13, 1, 4, 65536, LONGEST, "4a510282aef3fc37"),
    (53, 4, 16, 16384, 30000, "154908c746d80c08"),
    (123, 16, 64, 4096, 5000, "8d4a10e63b972ade"),
];

/// The buffer's first bytes: byte `i` is `i · 37 mod 251`.
fn first_bytes() -> Vec<u8> {
    stripes(BYTES)
}

/// `length` bytes, byte `i` of them `i · 37 mod 251`.
fn stripes(length: usize) -> Vec<u8> {
    (0..length).map(|i| (i * 37 % 251) as u8).collect()
}

/// The `n` bytes of `tblf.m`'s pattern, a xorshift stream.
fn probe_stream(n: usize) -> Vec<u8> {
    let mut state: u64 = 0x9e37_79b9_7f4a_7c15;
    (0..n)
        .map(|_| {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            (state >> 24) as u8 | 1
        })
        .collect()
}

/// The buffer's bytes after `fills` of a texture buffer of `format` at `offset`.
fn case(
    device: &Device,
    queue: &CommandQueue,
    format: u32,
    offset: u64,
    fills: &[&str],
) -> Vec<u8> {
    let buffer = buffer_with(device, &first_bytes()).unwrap();
    let d = TextureDescriptor::texture_buffer(
        PixelFormat::try_from(format).unwrap(),
        12,
        ResourceOptions::shared(),
        TextureUsage::SHADER_READ,
    );
    let texture = buffer.new_texture(&d, offset, 256).unwrap();
    let command_buffer = queue.command_buffer().unwrap();
    let mut blit = command_buffer.blit_command_encoder().unwrap();
    for fill in fills {
        let (spec, pattern) = fill.split_once(':').unwrap();
        let mut v = [0, 0, 0, 1, 1, 0, 0, 0];
        for (k, number) in spec.split(',').enumerate() {
            v[k] = number.parse().unwrap();
        }
        let [x, w, y, h, depth, level, slice, z] = v;
        let region = Region {
            origin: Origin::new(x, y, z),
            size: Size::new(w, h, depth),
        };
        if let Some(packed) = pattern.strip_prefix('c') {
            let pf = PixelFormat::try_from(packed.parse::<u32>().unwrap()).unwrap();
            let color = ClearColor::new(0.25, -0.5, 1.25, 0.75);
            blit.fill_texture_color(&texture, level, slice, region, color, Some(pf))
                .unwrap();
        } else {
            let counted = pattern.strip_prefix('p').unwrap();
            let (n, from) = counted.split_once('@').unwrap_or((counted, "a1"));
            let n: usize = n.parse().unwrap();
            let from = u8::from_str_radix(from, 16).unwrap();
            let bytes: Vec<u8> = (0..n).map(|i| from.wrapping_add(i as u8)).collect();
            blit.fill_texture(&texture, level, slice, region, &bytes)
                .unwrap();
        }
    }
    blit.end_encoding().unwrap();
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    read_buffer(&buffer, 0, BYTES as u64).unwrap()
}

/// The first 16 hex digits of the SHA-256 of `bytes`.
fn digest(bytes: &[u8]) -> String {
    Sha256::digest(bytes)
        .iter()
        .take(8)
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// The SHA-256 of the buffer's first bytes, which every stream and group fills over, and of the
/// longest of `tblf.m`'s patterns, whose stream the long cases fill with. Metal's own answer to
/// the cases that change nothing (`r8-past-buffer`, `rgba8-width-0`) is this buffer's digest, so
/// the probes filled over these bytes.
const FIRST_BYTES_SHA: &str = "d71bdb461ee51a7eaa2204acf6826288db6dacdeffe7f165722cb1d4befe7ae0";
const LONGEST_PATTERN_SHA: &str =
    "b9f88571970f75c87bc561728251f69c4f906906dadfdda9cf0f58e489e49bd0";

/// The inputs are the probes': the buffer's first bytes and the longest pattern, before any case
/// claims Metal's bytes of a fill over them.
#[test]
fn the_inputs_are_the_probes() {
    let sha = |bytes: &[u8]| crate::hash::hex_encode(Sha256::digest(bytes));
    assert_eq!(
        sha(&first_bytes()),
        FIRST_BYTES_SHA,
        "the buffer's first bytes"
    );
    assert_eq!(
        sha(&probe_stream(LONGEST)),
        LONGEST_PATTERN_SHA,
        "the longest pattern"
    );
}

/// Metal's bytes of the case `key`: the buffer's first bytes with its recorded runs written in.
fn metal_bytes(key: &str) -> Vec<u8> {
    let line = METAL_RUNS
        .lines()
        .find(|line| {
            line.split_once(' ')
                .map_or(*line == key, |(name, _)| name == key)
        })
        .unwrap_or_else(|| panic!("no Metal runs of {key}"));
    let mut bytes = first_bytes();
    for run in line.split('|').skip(1) {
        let (at, values) = run.split_once(':').expect("a run's offset");
        let at = usize::from_str_radix(at.trim().trim_start_matches("0x"), 16).expect("an offset");
        for (i, value) in values.split_whitespace().enumerate() {
            bytes[at + i] = u8::from_str_radix(value, 16).expect("a byte");
        }
    }
    bytes
}

/// The byte `ours` and Metal's part at, or `None` where they are alike.
fn unlike_metal(ours: &[u8], metal: &[u8]) -> Option<String> {
    ours.iter()
        .zip(metal)
        .position(|(got, want)| got != want)
        .map(|at| {
            format!(
                "byte {at:#x} is {:02x}, Metal's {:02x}",
                ours[at], metal[at]
            )
        })
}

/// The recorded runs are the probes': their own SHA-256, one line a case, and every case's
/// digest as the probe printed it.
#[test]
fn the_recorded_runs_are_the_probes() {
    assert_eq!(
        crate::hash::hex_encode(Sha256::digest(METAL_RUNS)),
        METAL_RUNS_SHA,
        "the recorded runs"
    );
    let mut lines = 0;
    for (name, _, _, _, metal) in STREAMS {
        assert_eq!(digest(&metal_bytes(name)), metal, "{name}");
        lines += 1;
    }
    for (name, _, _, cases, metal) in GROUPS {
        let bytes: Vec<Vec<u8>> = (0..cases.len())
            .map(|k| metal_bytes(&format!("{name}#{k}")))
            .collect();
        assert_eq!(digest(&bytes.concat()), metal, "{name}");
        lines += cases.len();
    }
    assert_eq!(METAL_RUNS.lines().count(), lines, "a line a case");
}

/// The runs of `bytes` that differ from the buffer's first bytes, as the probe prints them.
fn runs(bytes: &[u8]) -> String {
    let first = first_bytes();
    let mut line = String::new();
    let mut previous = false;
    for (at, (&byte, &was)) in bytes.iter().zip(&first).enumerate() {
        let changed = byte != was;
        if changed && !previous {
            line += &format!(" | {at:#x}:");
        }
        if changed {
            line += &format!(" {byte:02x}");
        }
        previous = changed;
    }
    line
}

/// Every stream and group writes the buffer's bytes as Metal's fills do, and the fills that read
/// bytes past their pattern's copies announce it once.
#[test]
fn texture_buffer_fills_write_metal_s_bytes() {
    on_device(|device, messages| {
        let queue = device.new_command_queue().unwrap();
        let mut wrong = Vec::new();
        for (name, format, offset, fills, _) in STREAMS {
            let ours = case(device, &queue, format, offset, fills);
            if let Some(unlike) = unlike_metal(&ours, &metal_bytes(name)) {
                println!(
                    "TEXTURE-BUFFER-FILLS-DIFFERS {name}: {unlike}:{}",
                    runs(&ours)
                );
                wrong.push(name.to_owned());
            }
        }
        for (name, format, offset, cases, _) in GROUPS {
            for (k, fills) in cases.iter().enumerate() {
                let ours = case(device, &queue, format, offset, fills);
                let key = format!("{name}#{k}");
                if let Some(unlike) = unlike_metal(&ours, &metal_bytes(&key)) {
                    println!(
                        "TEXTURE-BUFFER-FILLS-DIFFERS {key}: {unlike}:{}",
                        runs(&ours)
                    );
                    wrong.push(key);
                }
            }
        }
        let mut kinds = forced(&[(
            Degradation::SharedInSystemMemory,
            shared_in_system_memory(device),
        )]);
        kinds.extend(TEXTURE_BUFFER_FILL);
        assert_once(&messages.take_notices(), &Vec::from_iter(kinds));
        assert!(wrong.is_empty(), "fills unlike Metal's: {wrong:?}");
    });
}

/// Each long pattern's fill writes the buffer's bytes as Metal's does, and a pattern one byte
/// longer than Metal's longest is refused, as Metal's process ends there, and writes nothing
/// (the module docs): the bytes past the pattern's copies announced once.
#[test]
fn long_patterns_write_metal_s_bytes_and_a_longer_one_is_refused() {
    on_device(|device, messages| {
        let queue = device.new_command_queue().unwrap();
        let mut wrong = Vec::new();
        for (format, bytes, offset, width, n, metal) in LONG {
            let length = offset + width * bytes + 64;
            let buffer = buffer_with(device, &stripes(length as usize)).unwrap();
            let d = TextureDescriptor::texture_buffer(
                PixelFormat::try_from(format).unwrap(),
                width,
                ResourceOptions::shared(),
                TextureUsage::SHADER_READ,
            );
            let row = (width * bytes).next_multiple_of(256);
            let texture = buffer.new_texture(&d, offset, row).unwrap();
            let command_buffer = queue.command_buffer().unwrap();
            let mut blit = command_buffer.blit_command_encoder().unwrap();
            let region = Region::new_1d(0, width);
            blit.fill_texture(&texture, 0, 0, region, &probe_stream(n))
                .unwrap();
            if n == LONGEST {
                let longer = probe_stream(n + 1);
                let refused = blit.fill_texture(&texture, 0, 0, Region::new_1d(0, 0), &longer);
                let length = n as u64 + 1;
                let invalid = Invalid::new(
                    "fill_texture",
                    Problem::LongTextureBufferFillPattern { length },
                );
                assert_eq!(refused, Err(Error::Invalid(invalid)), "{format} {width}");
            }
            blit.end_encoding().unwrap();
            command_buffer.commit().unwrap();
            command_buffer.wait_until_completed_for(WAIT).unwrap();
            let ours = read_buffer(&buffer, 0, length).unwrap();
            if digest(&ours) != metal {
                println!("TEXTURE-BUFFER-FILLS-DIFFERS {format} {width} {n}");
                wrong.push((format, width, n));
            }
        }
        let mut kinds = forced(&[(
            Degradation::SharedInSystemMemory,
            shared_in_system_memory(device),
        )]);
        kinds.extend(TEXTURE_BUFFER_FILL);
        assert_once(&messages.take_notices(), &Vec::from_iter(kinds));
        assert!(wrong.is_empty(), "fills unlike Metal's: {wrong:?}");
    });
}
