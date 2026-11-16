//! Copies of Apple's two-plane extended-range textures on the device the cfg selects, under the
//! validation layer, against Metal's bytes.
//!
//! Each case replays one of a Metal probe `tx1`'s, on a 48 x 20 texture of `RGB10A8_2P_XR10` (`p2`)
//! or its sRGB twin (`p2s`) holding the probe's random texels: seed 1 of its xorshift generator
//! ([`Rng`]) gives each texel's plane-0 word (bits 30 and 31 random too) and alpha byte, written as
//! the probe writes them, the alpha bytes by a kernel's `float` write of `b / 255` (the owned
//! fixture `xr_two_planes`), then the words by `replace_region`. A texture's planes read back as
//! the probe reads them: plane 0 by `get_bytes`, 4 bytes a texel, and plane 1 as `round(255 a)` of
//! a kernel's `float` read.
//!
//! - `get`: `get_bytes` into 16,384 bytes of `0xab`, and `t2b`: a blit into a buffer of them, in
//!   each variant: rows of 4, 5 or 8 bytes a texel (`r4`, `r5`, `r8`), `r4` with images twice the
//!   plane apart (`r4i`), and a 17 x 9 region at (5, 3) into rows of 80 bytes from offset 256
//!   (`sub`);
//! - `rep`: `replace_region` of seed 2's 16,384 random bytes, and `b2t`: a blit of them from a
//!   buffer, in each variant (`sub`: a 13 x 11 region at (7, 2)), both planes read back;
//! - `t2t`: a blit into a texture of seed 3's texels, whole (`copy_texture`), of a 17 x 9 region at
//!   (3, 5) to (11, 2), and whole into a texture of the other format (`twin`), both planes read
//!   back.
//!
//! Metal's answer: the SHA-256 of each output ([`METAL`]; an Apple M4 under macOS
//! 27.0.1, plain and under Metal's validation layer alike, which stops only `twin`), which the
//! probe keeps. A texture-to-texture copy's planes are also checked against Metal's byte model
//! (`bind::plane_copy`, which Metal's measured copies pin), at 48 x 20 and at widths that are not a
//! multiple of 4; a copy whose rows overlap in the destination is announced once
//! (`two-plane-texture-copy`). A buffer blit at a width off 4 is checked against `bp.m`'s words
//! (an Apple M3 Max under macOS 27.0.1, `ROW_BLITS`): Metal's rows, and where rows meet in a blit
//! into the texture the row whose bytes Metal's stand, announced (`two-plane-buffer-blit`) where
//! Metal's order varies from run to run; and a fill read back by
//! such a blit against `pad.m`'s bytes (`PAD_CASES`), the bytes between rows included.
//! A differing output is printed whole, on one `XR-TWO-PLANE-COPIES-DIFFERS` line.

use super::degrade_notices::{counts, on_device};
use super::extended_range_two_planes::{digest, reads, texture, write, Dispatch, Kernels};
use super::{buffer_with, read_buffer};
use metal2vulkan::bind::PlaneRows;
use metal2vulkan::metal::PixelFormat;
use metal2vulkan_runtime::{
    BlitOption, Buffer, CommandBufferStatus, CommandQueue, Degradation, Device, Origin, Region,
    Size, Texture,
};
use sha2::{Digest, Sha256};
use std::time::Duration;

/// The longest a command buffer may take.
const WAIT: Duration = Duration::from_secs(60);

/// The texture's extent.
const W: u64 = 48;
const H: u64 = 20;
/// The bytes of each buffer and host copy.
const BIG: usize = 16 * (W * H) as usize + 1024;

/// The kinds a blit between two such textures, and one between a buffer and such a texture at
/// a width that is not a multiple of 4, announce: a wide one's, and a narrower one's whose rows
/// meet between Metal's groups of 32 rows.
pub(super) const TWO_PLANE_COPIES: [Degradation; 3] = [
    Degradation::TwoPlaneTextureCopy,
    Degradation::TwoPlaneBufferBlit,
    Degradation::TwoPlaneBufferBlitOrder,
];

/// Metal's outputs: the probe's case, the output, and the SHA-256 of its bytes.
const METAL: [(&str, &str, &str); 66] = [
    (
        "get.p2.r4",
        "out",
        "214d8cf2b34e79bee5712a5be1ebf67682b737eee5032760e2410e5b6c580e8c",
    ),
    (
        "get.p2.r5",
        "out",
        "b6139db33b5a7d978edaaefdafb7701ffdda5da49c9b8e991ccd1d7f81302fe6",
    ),
    (
        "get.p2.r8",
        "out",
        "5a074aa7d8aab53a5fffbdece7dd573c28ab3014d11016efcda39d3e7ed75726",
    ),
    (
        "get.p2.r4i",
        "out",
        "214d8cf2b34e79bee5712a5be1ebf67682b737eee5032760e2410e5b6c580e8c",
    ),
    (
        "get.p2.sub",
        "out",
        "30bc6e0ffed19dbc6b6713e8c839924ba33c58f43e21a2c5c1f98403759f5d6e",
    ),
    (
        "t2b.p2.r4",
        "out",
        "214d8cf2b34e79bee5712a5be1ebf67682b737eee5032760e2410e5b6c580e8c",
    ),
    (
        "t2b.p2.r5",
        "out",
        "b6139db33b5a7d978edaaefdafb7701ffdda5da49c9b8e991ccd1d7f81302fe6",
    ),
    (
        "t2b.p2.r8",
        "out",
        "5a074aa7d8aab53a5fffbdece7dd573c28ab3014d11016efcda39d3e7ed75726",
    ),
    (
        "t2b.p2.r4i",
        "out",
        "214d8cf2b34e79bee5712a5be1ebf67682b737eee5032760e2410e5b6c580e8c",
    ),
    (
        "t2b.p2.sub",
        "out",
        "30bc6e0ffed19dbc6b6713e8c839924ba33c58f43e21a2c5c1f98403759f5d6e",
    ),
    (
        "rep.p2.r4",
        "out.p0",
        "9695b6fdf1c2e40ab8125aced617a0565ea07ccbf8535a54eb19cfc5c569238b",
    ),
    (
        "rep.p2.r4",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "rep.p2.r5",
        "out.p0",
        "6148d632f2fd771873f460fb73f196c8f9887ded5623cfc4f2b719c2cbe2ec73",
    ),
    (
        "rep.p2.r5",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "rep.p2.r8",
        "out.p0",
        "bc41bd6aed35ff11c21b475829e83eb36be1a781dc623fab587b5607a44d9125",
    ),
    (
        "rep.p2.r8",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "rep.p2.r4i",
        "out.p0",
        "9695b6fdf1c2e40ab8125aced617a0565ea07ccbf8535a54eb19cfc5c569238b",
    ),
    (
        "rep.p2.r4i",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "rep.p2.sub",
        "out.p0",
        "9b696b573816b7cde2182005a0ef1d4b57a79c766194ced7e2efdc920b011e34",
    ),
    (
        "rep.p2.sub",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "b2t.p2.r4",
        "out.p0",
        "9695b6fdf1c2e40ab8125aced617a0565ea07ccbf8535a54eb19cfc5c569238b",
    ),
    (
        "b2t.p2.r4",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "b2t.p2.r5",
        "out.p0",
        "6148d632f2fd771873f460fb73f196c8f9887ded5623cfc4f2b719c2cbe2ec73",
    ),
    (
        "b2t.p2.r5",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "b2t.p2.r8",
        "out.p0",
        "bc41bd6aed35ff11c21b475829e83eb36be1a781dc623fab587b5607a44d9125",
    ),
    (
        "b2t.p2.r8",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "b2t.p2.r4i",
        "out.p0",
        "9695b6fdf1c2e40ab8125aced617a0565ea07ccbf8535a54eb19cfc5c569238b",
    ),
    (
        "b2t.p2.r4i",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "b2t.p2.sub",
        "out.p0",
        "9b696b573816b7cde2182005a0ef1d4b57a79c766194ced7e2efdc920b011e34",
    ),
    (
        "b2t.p2.sub",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "t2t.p2.whole",
        "out.p0",
        "bc4690d9570ceddf89ff6728f4e8c0288ae8a6141f24eac93edca8b0cf15c1fa",
    ),
    (
        "t2t.p2.sub",
        "out.p0",
        "f3436a44523a2b8cd955d14440cb4f02227cb2be5a97559e8bc52bafc3a4d4d2",
    ),
    (
        "t2t.p2.twin",
        "out.p0",
        "bc4690d9570ceddf89ff6728f4e8c0288ae8a6141f24eac93edca8b0cf15c1fa",
    ),
    (
        "get.p2s.r4",
        "out",
        "214d8cf2b34e79bee5712a5be1ebf67682b737eee5032760e2410e5b6c580e8c",
    ),
    (
        "get.p2s.r5",
        "out",
        "b6139db33b5a7d978edaaefdafb7701ffdda5da49c9b8e991ccd1d7f81302fe6",
    ),
    (
        "get.p2s.r8",
        "out",
        "5a074aa7d8aab53a5fffbdece7dd573c28ab3014d11016efcda39d3e7ed75726",
    ),
    (
        "get.p2s.r4i",
        "out",
        "214d8cf2b34e79bee5712a5be1ebf67682b737eee5032760e2410e5b6c580e8c",
    ),
    (
        "get.p2s.sub",
        "out",
        "30bc6e0ffed19dbc6b6713e8c839924ba33c58f43e21a2c5c1f98403759f5d6e",
    ),
    (
        "t2b.p2s.r4",
        "out",
        "214d8cf2b34e79bee5712a5be1ebf67682b737eee5032760e2410e5b6c580e8c",
    ),
    (
        "t2b.p2s.r5",
        "out",
        "b6139db33b5a7d978edaaefdafb7701ffdda5da49c9b8e991ccd1d7f81302fe6",
    ),
    (
        "t2b.p2s.r8",
        "out",
        "5a074aa7d8aab53a5fffbdece7dd573c28ab3014d11016efcda39d3e7ed75726",
    ),
    (
        "t2b.p2s.r4i",
        "out",
        "214d8cf2b34e79bee5712a5be1ebf67682b737eee5032760e2410e5b6c580e8c",
    ),
    (
        "t2b.p2s.sub",
        "out",
        "30bc6e0ffed19dbc6b6713e8c839924ba33c58f43e21a2c5c1f98403759f5d6e",
    ),
    (
        "rep.p2s.r4",
        "out.p0",
        "9695b6fdf1c2e40ab8125aced617a0565ea07ccbf8535a54eb19cfc5c569238b",
    ),
    (
        "rep.p2s.r4",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "rep.p2s.r5",
        "out.p0",
        "6148d632f2fd771873f460fb73f196c8f9887ded5623cfc4f2b719c2cbe2ec73",
    ),
    (
        "rep.p2s.r5",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "rep.p2s.r8",
        "out.p0",
        "bc41bd6aed35ff11c21b475829e83eb36be1a781dc623fab587b5607a44d9125",
    ),
    (
        "rep.p2s.r8",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "rep.p2s.r4i",
        "out.p0",
        "9695b6fdf1c2e40ab8125aced617a0565ea07ccbf8535a54eb19cfc5c569238b",
    ),
    (
        "rep.p2s.r4i",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "rep.p2s.sub",
        "out.p0",
        "9b696b573816b7cde2182005a0ef1d4b57a79c766194ced7e2efdc920b011e34",
    ),
    (
        "rep.p2s.sub",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "b2t.p2s.r4",
        "out.p0",
        "9695b6fdf1c2e40ab8125aced617a0565ea07ccbf8535a54eb19cfc5c569238b",
    ),
    (
        "b2t.p2s.r4",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "b2t.p2s.r5",
        "out.p0",
        "6148d632f2fd771873f460fb73f196c8f9887ded5623cfc4f2b719c2cbe2ec73",
    ),
    (
        "b2t.p2s.r5",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "b2t.p2s.r8",
        "out.p0",
        "bc41bd6aed35ff11c21b475829e83eb36be1a781dc623fab587b5607a44d9125",
    ),
    (
        "b2t.p2s.r8",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "b2t.p2s.r4i",
        "out.p0",
        "9695b6fdf1c2e40ab8125aced617a0565ea07ccbf8535a54eb19cfc5c569238b",
    ),
    (
        "b2t.p2s.r4i",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "b2t.p2s.sub",
        "out.p0",
        "9b696b573816b7cde2182005a0ef1d4b57a79c766194ced7e2efdc920b011e34",
    ),
    (
        "b2t.p2s.sub",
        "out.pa",
        "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    ),
    (
        "t2t.p2s.whole",
        "out.p0",
        "bc4690d9570ceddf89ff6728f4e8c0288ae8a6141f24eac93edca8b0cf15c1fa",
    ),
    (
        "t2t.p2s.sub",
        "out.p0",
        "f3436a44523a2b8cd955d14440cb4f02227cb2be5a97559e8bc52bafc3a4d4d2",
    ),
    (
        "t2t.p2s.twin",
        "out.p0",
        "bc4690d9570ceddf89ff6728f4e8c0288ae8a6141f24eac93edca8b0cf15c1fa",
    ),
];

/// The probe's xorshift generator.
pub(super) struct Rng(u64);

impl Rng {
    pub(super) fn new(seed: u64) -> Rng {
        let mut state = seed
            .wrapping_mul(0x9E37_79B9_7F4A_7C15)
            .wrapping_add(0x632B_E59B_D9B4_E019);
        if state == 0 {
            state = 1;
        }
        let mut rng = Rng(state);
        for _ in 0..8 {
            rng.next();
        }
        rng
    }

    pub(super) fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(2_685_821_657_736_338_717)
    }

    pub(super) fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

/// Seed `seed`'s `n` texels: plane 0's words (4 bytes each) and the alpha bytes.
pub(super) fn planes(seed: u64, n: u64) -> (Vec<u8>, Vec<u8>) {
    let mut rng = Rng::new(seed);
    let (mut words, mut alpha) = (Vec::new(), Vec::new());
    for _ in 0..n {
        let high = rng.below(4);
        let red = rng.below(1024);
        let green = rng.below(1024);
        let blue = rng.below(1024);
        let word = (high << 30 | red << 20 | green << 10 | blue) as u32;
        words.extend(word.to_le_bytes());
        alpha.push(rng.below(256) as u8);
    }
    (words, alpha)
}

/// Seed `seed`'s `n` random bytes.
fn random_bytes(seed: u64, n: usize) -> Vec<u8> {
    let mut rng = Rng::new(seed);
    (0..n).map(|_| rng.below(256) as u8).collect()
}

/// The device, its queue and the fixture's kernels.
pub(super) struct Lane<'a> {
    pub(super) device: &'a Device,
    pub(super) queue: CommandQueue,
    kernels: Kernels,
}

impl Lane<'_> {
    /// A `w` x `h` texture of `format` holding seed `seed`'s texels, written as the probe writes
    /// them, and the texels.
    pub(super) fn filled(
        &self,
        format: PixelFormat,
        seed: u64,
        (w, h): (u64, u64),
    ) -> (Texture, Vec<u8>, Vec<u8>) {
        let (words, alpha) = planes(seed, w * h);
        let t = self.holding(format, (&words, &alpha), (w, h));
        (t, words, alpha)
    }

    /// A `w` x `h` texture of `format` holding the color words `words` (4 bytes each) and the
    /// alpha bytes `alpha`, written as the probe writes them: the alpha bytes by a kernel's
    /// `float` write of `b / 255`, then the words by `replace_region`.
    pub(super) fn holding(
        &self,
        format: PixelFormat,
        (words, alpha): (&[u8], &[u8]),
        (w, h): (u64, u64),
    ) -> Texture {
        let t = texture(self.device, format, w, h);
        let values: Vec<u8> = alpha
            .iter()
            .flat_map(|&b| [0.0, 0.0, 0.0, f32::from(b) / 255.0])
            .flat_map(f32::to_le_bytes)
            .collect();
        let dispatch = Dispatch::Texels(w, h);
        write(
            self.device,
            &self.queue,
            &self.kernels,
            &t,
            (&values, false),
            dispatch,
        );
        t.replace_region(Region::new_2d(0, 0, w, h), 0, 0, words, 4 * w, 0)
            .unwrap();
        t
    }

    /// The planes of `t`, 48 x 20, read back as the probe reads them.
    fn planes_of(&self, t: &Texture) -> (Vec<u8>, Vec<u8>) {
        self.planes_sized(t, (W, H))
    }

    /// The planes of `t`, `w` x `h`, read back as the probe reads them.
    fn planes_sized(&self, t: &Texture, (w, h): (u64, u64)) -> (Vec<u8>, Vec<u8>) {
        let mut words = vec![0; (4 * w * h) as usize];
        t.get_bytes(&mut words, 4 * w, 0, Region::new_2d(0, 0, w, h), 0, 0)
            .unwrap();
        let [_, floats] = reads(
            self.device,
            &self.queue,
            &self.kernels,
            t,
            Dispatch::Texels(w, h),
        );
        let alpha = floats
            .as_chunks::<16>()
            .0
            .iter()
            .map(|c| {
                let a = f32::from_le_bytes(c[12..16].try_into().unwrap());
                (a * 255.0).round_ties_even() as u8
            })
            .collect();
        (words, alpha)
    }

    /// Runs `body` in a blit encoder, commits and waits.
    fn blit(&self, body: impl FnOnce(&mut metal2vulkan_runtime::BlitCommandEncoder)) {
        let command_buffer = self.queue.command_buffer().unwrap();
        let mut encoder = command_buffer.blit_command_encoder().unwrap();
        body(&mut encoder);
        encoder.end_encoding().unwrap();
        command_buffer.commit().unwrap();
        command_buffer.wait_until_completed_for(WAIT).unwrap();
        assert_eq!(command_buffer.status(), CommandBufferStatus::Completed);
        assert_eq!(command_buffer.error(), None);
    }
}

/// A variant's region and layout: its origin, size, rows, images and buffer offset; `sub` the
/// size and origin of its region.
fn variant(name: &str, sub: [u64; 4]) -> (Origin, Size, u64, u64, u64) {
    let whole = |bpr: u64, bpi: u64| (Origin::new(0, 0, 0), Size::new(W, H, 1), bpr, bpi, 0);
    match name {
        "r4" => whole(4 * W, 4 * W * H),
        "r5" => whole(5 * W, 5 * W * H),
        "r8" => whole(8 * W, 8 * W * H),
        "r4i" => whole(4 * W, 8 * W * H),
        _ => {
            let [w, h, x, y] = sub;
            (Origin::new(x, y, 0), Size::new(w, h, 1), 80, 80 * h, 256)
        }
    }
}

const VARIANTS: [&str; 5] = ["r4", "r5", "r8", "r4i", "sub"];

/// The formats and the probe's names for them.
pub(super) const FORMATS: [(PixelFormat, &str); 2] = [
    (PixelFormat::RGB10A82PXR10, "p2"),
    (PixelFormat::RGB10A82PXR10SRGB, "p2s"),
];

/// Checks output `what` of `case` against Metal's digest, keeping a difference in `wrong`.
fn check(wrong: &mut Vec<String>, case: &str, what: &str, bytes: &[u8]) {
    let want = METAL
        .iter()
        .find(|m| m.0 == case && m.1 == what)
        .map(|m| m.2)
        .unwrap_or_else(|| panic!("no Metal digest of {case} {what}"));
    let got = digest(bytes);
    if got != want {
        let hex: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
        wrong.push(format!("{case} {what}: {got}, Metal {want}; bytes {hex}"));
    }
}

/// Prints `wrong`'s lines and fails when there is one.
fn report(device: &Device, wrong: &[String]) {
    for line in wrong {
        println!("XR-TWO-PLANE-COPIES-DIFFERS {}: {line}", device.name());
    }
    assert!(
        wrong.is_empty(),
        "{} outputs differ from Metal",
        wrong.len()
    );
}

/// The lane's kernels on `device`, and a queue.
pub(super) fn lane(device: &Device) -> Lane<'_> {
    Lane {
        device,
        queue: device.new_command_queue().unwrap(),
        kernels: Kernels::new(device),
    }
}

/// `get_bytes` and `replace_region` move each texel's color word at the caller's rows and leave
/// the alpha plane and every byte between the rows as they are, as Metal's do.
#[test]
fn cpu_copies_move_the_color_words_as_metal_does() {
    on_device(|device, _| {
        let lane = lane(device);
        let mut wrong = Vec::new();
        for (format, name) in FORMATS {
            for v in VARIANTS {
                let (t, _, _) = lane.filled(format, 1, (W, H));
                let (origin, size, bpr, bpi, offset) = variant(v, [17, 9, 5, 3]);
                let mut out = vec![0xab; BIG];
                t.get_bytes(
                    &mut out[offset as usize..],
                    bpr,
                    bpi,
                    Region { origin, size },
                    0,
                    0,
                )
                .unwrap();
                check(&mut wrong, &format!("get.{name}.{v}"), "out", &out);

                let (t, _, _) = lane.filled(format, 1, (W, H));
                let (origin, size, bpr, bpi, offset) = variant(v, [13, 11, 7, 2]);
                let input = random_bytes(2, BIG);
                t.replace_region(
                    Region { origin, size },
                    0,
                    0,
                    &input[offset as usize..],
                    bpr,
                    bpi,
                )
                .unwrap();
                let (words, alpha) = lane.planes_of(&t);
                check(&mut wrong, &format!("rep.{name}.{v}"), "out.p0", &words);
                check(&mut wrong, &format!("rep.{name}.{v}"), "out.pa", &alpha);
            }
        }
        report(device, &wrong);
    });
}

/// Blits between a buffer and the texture move each texel's color word at the buffer's rows and
/// leave the alpha plane and every byte between the rows as they are, as Metal's do.
#[test]
fn buffer_blits_move_the_color_words_as_metal_does() {
    on_device(|device, _| {
        let lane = lane(device);
        let mut wrong = Vec::new();
        for (format, name) in FORMATS {
            for v in VARIANTS {
                let (t, _, _) = lane.filled(format, 1, (W, H));
                let (origin, size, bpr, bpi, offset) = variant(v, [17, 9, 5, 3]);
                let out = buffer_with(device, &[0xab; BIG]).unwrap();
                lane.blit(|e| {
                    e.copy_from_texture_to_buffer(
                        &t,
                        0,
                        0,
                        origin,
                        size,
                        &out,
                        offset,
                        bpr,
                        bpi,
                        BlitOption::None,
                    )
                    .unwrap()
                });
                let bytes = read_buffer(&out, 0, BIG as u64).unwrap();
                check(&mut wrong, &format!("t2b.{name}.{v}"), "out", &bytes);

                let (t, _, _) = lane.filled(format, 1, (W, H));
                let (origin, size, bpr, bpi, offset) = variant(v, [13, 11, 7, 2]);
                let input: Buffer = buffer_with(device, &random_bytes(2, BIG)).unwrap();
                lane.blit(|e| {
                    e.copy_from_buffer_to_texture(
                        &input,
                        offset,
                        bpr,
                        bpi,
                        size,
                        &t,
                        0,
                        0,
                        origin,
                        BlitOption::None,
                    )
                    .unwrap()
                });
                let (words, alpha) = lane.planes_of(&t);
                check(&mut wrong, &format!("b2t.{name}.{v}"), "out.p0", &words);
                check(&mut wrong, &format!("b2t.{name}.{v}"), "out.pa", &alpha);
            }
        }
        report(device, &wrong);
    });
}

/// A texture's planes: its color words, then its alpha bytes.
type Planes = (Vec<u8>, Vec<u8>);

/// A blit of `size` texels from `from` of a texture of `source` texels (width, height) whose
/// planes are `planes.0` (color words, then alpha bytes) to `to` of one of `destination` texels
/// holding `planes.1`, as Metal's blit moves the bytes (`bind::plane_copy` states the model, which
/// Metal's measured copies pin): each row of the copy in turn, the later row's bytes kept where two
/// land on one, a byte in a row's padding or past a plane landing on no texel and read as 0. The
/// destination's planes after it.
fn modelled(
    (source, destination): ((u64, u64), (u64, u64)),
    (from, size, to): ([u64; 2], [u64; 2], [u64; 2]),
    planes: (&Planes, &Planes),
) -> Planes {
    let rows = |w: u64| PlaneRows::of(w as u32);
    let (s, d) = (rows(source.0), rows(destination.0));
    let mut out = planes.1.clone();
    for (unit, s_rows, d_rows) in [(4, s.color, d.color), (1, s.alpha, d.alpha)] {
        let (s_rows, d_rows) = (u64::from(s_rows), u64::from(d_rows));
        let mut plane = std::collections::BTreeMap::new();
        for r in 0..size[1] {
            for c in 0..size[0] {
                let read = (from[1] + r) * u64::from(s.blit) + unit * (from[0] + c);
                let write = (to[1] + r) * u64::from(d.blit) + unit * (to[0] + c);
                let (y, x) = (read / s_rows, read % s_rows / unit);
                let texel = (x < source.0 && y < source.1).then_some(y * source.0 + x);
                plane.insert(write, texel);
            }
        }
        for (at, texel) in plane {
            let (y, x) = (at / d_rows, at % d_rows / unit);
            if x >= destination.0 || y >= destination.1 {
                continue;
            }
            let i = (y * destination.0 + x) as usize;
            if unit == 4 {
                let word = texel.map_or([0; 4], |t| {
                    let t = 4 * t as usize;
                    planes.0 .0[t..t + 4].try_into().unwrap()
                });
                out.0[4 * i..4 * i + 4].copy_from_slice(&word);
            } else {
                out.1[i] = texel.map_or(0, |t| planes.0 .1[t as usize]);
            }
        }
    }
    out
}

/// Blits between two such textures move both planes' bytes as Metal's blit does: at 48 x 20 plane 0
/// against Metal's digests and both planes against the model, whole, a region and
/// into the other format; at widths that are not a multiple of 4 (plane 0's texels lost or moved
/// too) whole copies and regions whose rows do not overlap against the model; none of them
/// announced. A copy whose rows overlap in the destination with other bytes, where Metal's blit
/// writes them in an order that varies, is announced once (`two-plane-texture-copy`).
#[test]
fn texture_blits_move_both_planes_as_metal_does() {
    on_device(|device, messages| {
        let lane = lane(device);
        let mut wrong = Vec::new();
        let mut model_wrong = Vec::new();
        let blit = |format, into, (w, h): (u64, u64), copy: ([u64; 2], [u64; 2], [u64; 2])| {
            let (s, source_words, source_alpha) = lane.filled(format, 1, (w, h));
            let (d, words, alpha) = lane.filled(into, 3, (w, h));
            let (from, size, to) = copy;
            lane.blit(|e| {
                if copy == ([0, 0], [w, h], [0, 0]) {
                    e.copy_texture(&s, &d).unwrap()
                } else {
                    e.copy_from_texture_to_texture(
                        &s,
                        0,
                        0,
                        Origin::new(from[0], from[1], 0),
                        Size::new(size[0], size[1], 1),
                        &d,
                        0,
                        0,
                        Origin::new(to[0], to[1], 0),
                    )
                    .unwrap()
                }
            });
            let got = lane.planes_sized(&d, (w, h));
            let want = modelled(
                ((w, h), (w, h)),
                copy,
                (&(source_words, source_alpha), &(words, alpha)),
            );
            (got, want)
        };
        for (format, name) in FORMATS {
            let other = FORMATS.iter().find(|f| f.0 != format).unwrap().0;
            for v in ["whole", "sub", "twin"] {
                let into = if v == "twin" { other } else { format };
                let copy = match v {
                    "sub" => ([3, 5], [17, 9], [11, 2]),
                    _ => ([0, 0], [W, H], [0, 0]),
                };
                let (got, want) = blit(format, into, (W, H), copy);
                check(&mut wrong, &format!("t2t.{name}.{v}"), "out.p0", &got.0);
                if got != want {
                    model_wrong.push(format!("t2t.{name}.{v}"));
                }
            }
        }
        report(device, &wrong);
        let format = FORMATS[0].0;
        for (size, copy) in [
            ((5, 5), ([0, 0], [5, 5], [0, 0])),
            ((9, 7), ([0, 0], [9, 7], [0, 0])),
            ((257, 3), ([0, 0], [257, 3], [0, 0])),
            ((13, 6), ([1, 2], [6, 3], [5, 1])),
            ((7, 9), ([1, 0], [3, 8], [4, 1])),
        ] {
            let (got, want) = blit(format, format, size, copy);
            if got != want {
                model_wrong.push(format!("{size:?} {copy:?}"));
            }
        }
        assert!(
            model_wrong.is_empty(),
            "not the model's bytes: {model_wrong:?}"
        );
        let notices = counts(&messages.take_notices());
        assert_eq!(notices.get(&TWO_PLANE_COPIES[0]), None, "{notices:?}");
        // Rows overlapping in the destination (9 wide: rows 36 bytes, blit rows 32) from other
        // rows of the source: the model's bytes, announced.
        let copy = ([0, 1], [9, 3], [0, 0]);
        let (got, want) = blit(format, format, (9, 7), copy);
        assert_eq!(got, want, "an overlapping copy keeps the later row's bytes");
        let notices = counts(&messages.take_notices());
        assert_eq!(
            notices.get(&TWO_PLANE_COPIES[0]).copied(),
            Some(1),
            "{notices:?}"
        );
    });
}

/// Metal's blit keeps the bytes it writes outside a level's texels (a row's padding, past a
/// plane) and reads them back in a later blit; the kernel holds the texels
/// alone, so a copy reading such bytes of a texture an earlier blit wrote them into is announced
/// once (`two-plane-texture-copy`), and the earlier copy, from a fresh texture, is not.
#[test]
fn chained_blits_reading_kept_bytes_are_announced() {
    on_device(|device, messages| {
        let lane = lane(device);
        let format = FORMATS[0].0;
        let (w, h) = (9u64, 7u64);
        let (a, _, _) = lane.filled(format, 1, (w, h));
        let (b, _, _) = lane.filled(format, 3, (w, h));
        let (c, _, _) = lane.filled(format, 5, (w, h));
        lane.blit(|e| e.copy_texture(&a, &b).unwrap());
        let notices = counts(&messages.take_notices());
        assert_eq!(notices.get(&TWO_PLANE_COPIES[0]), None, "{notices:?}");
        lane.blit(|e| e.copy_texture(&b, &c).unwrap());
        let notices = counts(&messages.take_notices());
        assert_eq!(
            notices.get(&TWO_PLANE_COPIES[0]).copied(),
            Some(1),
            "{notices:?}"
        );
    });
}

/// One of `bp.m`'s blits between a buffer and a two-plane texture of one level at a width off 4
/// (an Apple M3 Max under macOS 27.0.1): the texture's width, height and slices, whether in a
/// placement heap of Metal's size, the slice, a fill of `a1 a2 a3 a4` first (x, y, width,
/// height), out of the texture or into it, the region (x, y, width, height), and Metal's words as
/// the probe printed them: out of the texture, the buffer's; into it, the texels' by `get_bytes`
/// (`t<s>.<i>` texel `i`'s word of slice `s` as `replace_region` set it, `t<i>` slice 0's).
struct RowBlit {
    extent: [u64; 3],
    heap: bool,
    slice: u64,
    fill: Option<[u64; 4]>,
    out: bool,
    region: [u64; 4],
    metal: &'static str,
}

const fn row_blit(
    (extent, heap, slice): ([u64; 3], bool, u64),
    fill: Option<[u64; 4]>,
    (out, region): (bool, [u64; 4]),
    metal: &'static str,
) -> RowBlit {
    RowBlit {
        extent,
        heap,
        slice,
        fill,
        out,
        region,
        metal,
    }
}

const OWN: bool = false;
const OUT: bool = true;
const IN: bool = false;

/// Metal's blits that the runtime answers alike and announces nothing for, rows that meet into
/// the texture below a width of 32 among them (in pairs of rows from an even origin, in fours
/// from another).
const ROW_BLITS: [RowBlit; 22] = [
    row_blit(([5, 3, 1], OWN, 0), None, (OUT, [0, 0, 5, 3]),
        "t0 t1 t2 t3 t4 | t4 00000000 00000000 00000000 t5 | t5 t6 t7 t8 t9"),
    row_blit(([5, 3, 1], OWN, 0), None, (OUT, [1, 1, 3, 2]), "00000000 00000000 00000000 | t6 t7 t8"),
    row_blit(([7, 4, 1], OWN, 0), None, (OUT, [2, 1, 4, 3]),
        "t6 00000000 t7 t8 | t9 t10 t11 t12 | t13 00000000 t14 t15"),
    row_blit(([3, 4, 1], OWN, 0), None, (OUT, [0, 0, 3, 4]),
        "t0 t1 t2 | 00000000 00000000 00000000 | 00000000 00000000 00000000 | 00000000 00000000 00000000"),
    row_blit(([3, 4, 1], OWN, 0), None, (OUT, [1, 0, 2, 3]),
        "t1 t2 | 00000000 00000000 | 00000000 00000000"),
    row_blit(([1, 5, 1], OWN, 0), None, (OUT, [0, 0, 1, 5]),
        "t0 | 00000000 | 00000000 | 00000000 | 00000000"),
    row_blit(([5, 3, 2], OWN, 1), None, (OUT, [0, 0, 5, 3]),
        "t1.0 t1.1 t1.2 t1.3 t1.4 | t1.4 00000000 00000000 00000000 t1.5 | t1.5 t1.6 t1.7 t1.8 t1.9"),
    row_blit(([7, 4, 1], OWN, 0), None, (IN, [2, 1, 4, 3]),
        "t0 t1 t2 t3 t4 t5 b0000000 | b0000002 b0000003 b0000100 b0000101 b0000102 b0000103 b0000200 | b0000202 b0000203 t16 t17 t18 t19 t20 | t21 t22 t23 t24 t25 t26 t27"),
    row_blit(([3, 4, 1], OWN, 0), None, (IN, [0, 0, 3, 4]),
        "b0000000 b0000001 b0000002 | b0000100 b0000101 b0000102 | b0000200 b0000201 b0000202 | b0000300 b0000301 b0000302"),
    row_blit(([3, 4, 1], OWN, 0), None, (IN, [1, 1, 2, 2]),
        "t0 t1 t2 | t3 b0000000 b0000001 | t6 b0000100 b0000101 | t9 t10 t11"),
    row_blit(([5, 3, 1], true, 0), Some([1, 1, 2, 1]), (OUT, [0, 0, 5, 3]),
        "t0 t1 t2 t3 t4 | t4 a4a3a2a1 a4a3a2a1 00000000 t5 | t5 t6 t7 t8 t9"),
    row_blit(([7, 4, 1], true, 0), None, (IN, [2, 1, 4, 3]),
        "t0 t1 t2 t3 t4 t5 b0000000 | b0000002 b0000003 b0000100 b0000101 b0000102 b0000103 b0000200 | b0000202 b0000203 t16 t17 t18 t19 t20 | t21 t22 t23 t24 t25 t26 t27"),
    row_blit(([3, 4, 1], true, 0), None, (OUT, [0, 0, 3, 4]),
        "t0 t1 t2 | 00000000 00000000 00000000 | 00000000 00000000 00000000 | 00000000 00000000 00000000"),
    row_blit(([5, 3, 1], OWN, 0), Some([1, 1, 2, 1]), (OUT, [0, 0, 5, 3]),
        "t0 t1 t2 t3 t4 | t4 a4a3a2a1 a4a3a2a1 00000000 t5 | t5 t6 t7 t8 t9"),    row_blit(([5, 6, 1], OWN, 0), None, (IN, [0, 1, 5, 4]),
        "t0 t1 t2 t3 b0000000 | b0000004 b0000101 b0000102 b0000103 b0000104 | b0000204 b0000301 b0000302 b0000303 b0000304 | t15 t16 t17 t18 t19 | t20 t21 t22 t23 t24 | t25 t26 t27 t28 t29"),
    row_blit(([5, 6, 1], true, 0), None, (IN, [0, 1, 5, 4]),
        "t0 t1 t2 t3 b0000000 | b0000004 b0000101 b0000102 b0000103 b0000104 | b0000204 b0000301 b0000302 b0000303 b0000304 | t15 t16 t17 t18 t19 | t20 t21 t22 t23 t24 | t25 t26 t27 t28 t29"),
    row_blit(([7, 4, 3], OWN, 2), None, (IN, [1, 1, 5, 3]),
        "t2.0 t2.1 t2.2 t2.3 t2.4 b0000000 b0000001 | b0000003 b0000004 b0000101 b0000102 b0000103 b0000104 b0000201 | b0000203 b0000204 t2.16 t2.17 t2.18 t2.19 t2.20 | t2.21 t2.22 t2.23 t2.24 t2.25 t2.26 t2.27"),
    row_blit(([7, 3, 1], OWN, 0), None, (IN, [0, 0, 7, 3]),
        "b0000000 b0000001 b0000002 b0000003 b0000004 b0000005 b0000006 | b0000200 b0000201 b0000202 b0000203 b0000204 b0000205 b0000206 | t14 t15 t16 t17 t18 t19 t20"),
    row_blit(([5, 8, 1], OWN, 0), None, (IN, [0, 1, 5, 6]),
        "t0 t1 t2 t3 b0000000 | b0000004 b0000101 b0000102 b0000103 b0000104 | b0000204 b0000301 b0000302 b0000303 b0000400 | b0000404 b0000501 b0000502 b0000503 b0000504 | t20 t21 t22 t23 t24 | t25 t26 t27 t28 t29 | t30 t31 t32 t33 t34 | t35 t36 t37 t38 t39"),
    row_blit(([5, 6, 1], OWN, 0), None, (IN, [0, 0, 5, 6]),
        "b0000000 b0000001 b0000002 b0000003 b0000004 | b0000200 b0000201 b0000202 b0000203 b0000204 | b0000400 b0000401 b0000402 b0000403 b0000404 | b0000504 t16 t17 t18 t19 | t20 t21 t22 t23 t24 | t25 t26 t27 t28 t29"),
    row_blit(([6, 5, 1], OWN, 0), None, (IN, [0, 0, 6, 5]),
        "b0000000 b0000001 b0000002 b0000003 b0000004 b0000005 | b0000200 b0000201 b0000202 b0000203 b0000204 b0000205 | b0000400 b0000401 b0000402 b0000403 b0000404 b0000405 | t18 t19 t20 t21 t22 t23 | t24 t25 t26 t27 t28 t29"),
    row_blit(([9, 4, 1], OWN, 0), None, (IN, [0, 0, 9, 4]),
        "b0000000 b0000001 b0000002 b0000003 b0000004 b0000005 b0000006 b0000007 b0000008 | b0000104 b0000105 b0000106 b0000107 b0000200 b0000201 b0000202 b0000203 b0000204 | b0000208 b0000301 b0000302 b0000303 b0000304 b0000305 b0000306 b0000307 b0000308 | t27 t28 t29 t30 t31 t32 t33 t34 t35"),
];

/// Metal's blits that the runtime announces: into the texture with rows that meet where Metal's
/// order varies from run to run, 32 texels wide or wider (`two-plane-buffer-blit`) and between
/// groups of 32 rows (`two-plane-buffer-blit-order`), each answered as Metal's in three runs here,
/// where the earlier row's bytes stood.
const ANNOUNCED_ROW_BLITS: [RowBlit; 2] = [
    row_blit(([41, 2, 1], OWN, 0), None, (IN, [0, 0, 41, 2]),
        "b0000000 b0000001 b0000002 b0000003 b0000004 b0000005 b0000006 b0000007 b0000008 b0000009 b000000a b000000b b000000c b000000d b000000e b000000f b0000010 b0000011 b0000012 b0000013 b0000014 b0000015 b0000016 b0000017 b0000018 b0000019 b000001a b000001b b000001c b000001d b000001e b000001f b0000020 b0000021 b0000022 b0000023 b0000024 b0000025 b0000026 b0000027 b0000028 | b0000104 b0000105 b0000106 b0000107 b0000108 b0000109 b000010a b000010b b000010c b000010d b000010e b000010f b0000110 b0000111 b0000112 b0000113 b0000114 b0000115 b0000116 b0000117 b0000118 b0000119 b000011a b000011b b000011c b000011d b000011e b000011f b0000120 b0000121 b0000122 b0000123 b0000124 b0000125 b0000126 b0000127 b0000128 t78 t79 t80 t81"),
    row_blit(([5, 33, 1], OWN, 0), None, (IN, [0, 0, 5, 33]),
        "b0000000 b0000001 b0000002 b0000003 b0000004 | b0000200 b0000201 b0000202 b0000203 b0000204 | b0000400 b0000401 b0000402 b0000403 b0000404 | b0000600 b0000601 b0000602 b0000603 b0000604 | b0000800 b0000801 b0000802 b0000803 b0000804 | b0000a00 b0000a01 b0000a02 b0000a03 b0000a04 | b0000c00 b0000c01 b0000c02 b0000c03 b0000c04 | b0000e00 b0000e01 b0000e02 b0000e03 b0000e04 | b0001000 b0001001 b0001002 b0001003 b0001004 | b0001200 b0001201 b0001202 b0001203 b0001204 | b0001400 b0001401 b0001402 b0001403 b0001404 | b0001600 b0001601 b0001602 b0001603 b0001604 | b0001800 b0001801 b0001802 b0001803 b0001804 | b0001a00 b0001a01 b0001a02 b0001a03 b0001a04 | b0001c00 b0001c01 b0001c02 b0001c03 b0001c04 | b0001e00 b0001e01 b0001e02 b0001e03 b0001e04 | b0001f04 b0002001 b0002002 b0002003 b0002004 | t85 t86 t87 t88 t89 | t90 t91 t92 t93 t94 | t95 t96 t97 t98 t99 | t100 t101 t102 t103 t104 | t105 t106 t107 t108 t109 | t110 t111 t112 t113 t114 | t115 t116 t117 t118 t119 | t120 t121 t122 t123 t124 | t125 t126 t127 t128 t129 | t130 t131 t132 t133 t134 | t135 t136 t137 t138 t139 | t140 t141 t142 t143 t144 | t145 t146 t147 t148 t149 | t150 t151 t152 t153 t154 | t155 t156 t157 t158 t159 | t160 t161 t162 t163 t164"),
];

/// The words `printed` names, as `bp.m` printed them.
fn printed_words(printed: &str) -> Vec<u32> {
    printed
        .split_whitespace()
        .filter(|token| *token != "|")
        .map(|token| match token.strip_prefix('t') {
            Some(texel) => {
                let (slice, index) = texel.split_once('.').unwrap_or(("0", texel));
                0x1000_0000
                    + (u32::from_str_radix(slice, 16).unwrap() << 16)
                    + index.parse::<u32>().unwrap()
            }
            None => u32::from_str_radix(token, 16).unwrap(),
        })
        .collect()
}

/// `blit`'s words, as the runtime answers them.
fn row_blit_words(device: &Device, queue: &CommandQueue, blit: &RowBlit) -> Vec<u32> {
    let [w, h, slices] = blit.extent;
    let mut d = metal2vulkan_runtime::TextureDescriptor::texture_2d(FORMATS[0].0, w, h, false);
    if slices > 1 {
        d.texture_type = metal2vulkan::metal::TextureType::Type2DArray;
        d.array_length = slices;
    }
    d.usage = metal2vulkan::metal::TextureUsage::SHADER_READ;
    d.resource_options = metal2vulkan_runtime::ResourceOptions::shared();
    let t = match blit.heap {
        true => {
            let size = device.heap_texture_size_and_align(&d).unwrap().size;
            let heap = device
                .new_heap(&metal2vulkan_runtime::HeapDescriptor::new(
                    metal2vulkan_runtime::HeapType::Placement,
                    size,
                    metal2vulkan_runtime::ResourceOptions::shared(),
                ))
                .unwrap();
            heap.new_texture_at_offset(&d, 0).unwrap()
        }
        false => device.new_texture(&d).unwrap(),
    };
    for s in 0..slices {
        let words: Vec<u8> = (0..w * h)
            .flat_map(|i| (0x1000_0000 + (s << 16) as u32 + i as u32).to_le_bytes())
            .collect();
        t.replace_region(Region::new_2d(0, 0, w, h), 0, s, &words, 4 * w, 4 * w * h)
            .unwrap();
    }
    let run = |body: &dyn Fn(&mut metal2vulkan_runtime::BlitCommandEncoder)| {
        let command_buffer = queue.command_buffer().unwrap();
        let mut encoder = command_buffer.blit_command_encoder().unwrap();
        body(&mut encoder);
        encoder.end_encoding().unwrap();
        command_buffer.commit().unwrap();
        command_buffer.wait_until_completed_for(WAIT).unwrap();
        assert_eq!(command_buffer.status(), CommandBufferStatus::Completed);
    };
    if let Some([x, y, fw, fh]) = blit.fill {
        run(&|e| {
            e.fill_texture(
                &t,
                0,
                0,
                Region::new_2d(x, y, fw, fh),
                &[0xa1, 0xa2, 0xa3, 0xa4],
            )
            .unwrap()
        });
    }
    let [x, y, rw, rh] = blit.region;
    let row = 4 * rw;
    let words = |bytes: Vec<u8>| -> Vec<u32> {
        bytes
            .as_chunks::<4>()
            .0
            .iter()
            .map(|b| u32::from_le_bytes(*b))
            .collect()
    };
    if blit.out {
        let buffer = buffer_with(device, &vec![0xee; (row * rh) as usize]).unwrap();
        run(&|e| {
            e.copy_from_texture_to_buffer(
                &t,
                blit.slice,
                0,
                Origin::new(x, y, 0),
                Size::new(rw, rh, 1),
                &buffer,
                0,
                row,
                row * rh,
                BlitOption::None,
            )
            .unwrap()
        });
        return words(read_buffer(&buffer, 0, row * rh).unwrap());
    }
    let input: Vec<u8> = (0..rh)
        .flat_map(|r| (0..rw).map(move |c| 0xb000_0000u32 + ((r as u32) << 8) + c as u32))
        .flat_map(u32::to_le_bytes)
        .collect();
    let buffer = buffer_with(device, &input).unwrap();
    run(&|e| {
        e.copy_from_buffer_to_texture(
            &buffer,
            0,
            row,
            row * rh,
            Size::new(rw, rh, 1),
            &t,
            blit.slice,
            0,
            Origin::new(x, y, 0),
            BlitOption::None,
        )
        .unwrap()
    });
    let mut texels = vec![0; (4 * w * h) as usize];
    t.get_bytes(
        &mut texels,
        4 * w,
        4 * w * h,
        Region::new_2d(0, 0, w, h),
        0,
        blit.slice,
    )
    .unwrap();
    words(texels)
}

/// A blit between a buffer and a two-plane texture of one level at a width off 4, of its own
/// memory or a heap's, moves Metal's words at Metal's rows: out of the texture the bytes between
/// rows too (0 in a new texture, as a fill left them, held apart in a texture of the device's
/// memory), and below a width of 4 only the first row; into it, where rows meet, the row whose
/// bytes Metal's GPU leaves; announced only where rows meet in a blit into it where Metal's order
/// varies from run to run.
#[test]
fn buffer_blits_at_widths_off_4_take_metal_s_rows() {
    on_device(|device, messages| {
        let queue = device.new_command_queue().unwrap();
        let mut wrong = Vec::new();
        let mut check = |n: usize, blit: &RowBlit, want: &str| {
            let ours = row_blit_words(device, &queue, blit);
            if ours != printed_words(want) {
                println!("XR-TWO-PLANE-COPIES-DIFFERS rows {n}: {ours:08x?} (Metal's {want})");
                wrong.push(n);
            }
        };
        for (n, blit) in ROW_BLITS.iter().enumerate() {
            check(n, blit, blit.metal);
        }
        let notices = counts(&messages.take_notices());
        assert_eq!(notices.get(&TWO_PLANE_COPIES[1]), None, "{notices:?}");
        assert_eq!(notices.get(&TWO_PLANE_COPIES[2]), None, "{notices:?}");
        for (n, blit) in ANNOUNCED_ROW_BLITS.iter().enumerate() {
            check(100 + n, blit, blit.metal);
        }
        // The wide blit's kind and the one across groups of 32 rows', each once.
        let notices = counts(&messages.take_notices());
        for kind in &TWO_PLANE_COPIES[1..] {
            assert_eq!(notices.get(kind).copied(), Some(1), "{notices:?}");
        }
        assert!(wrong.is_empty(), "blits unlike Metal's: {wrong:?}");
    });
}

/// Our probe `pad.m`'s cases (an Apple M3 Max under macOS 27.0.1, alike in two runs): a private
/// texture of the format, width, height and slices, every color word set to `0xee` by a blit
/// from a buffer, one fill of a slice's region (a pattern, or a color packed in `RGBA8Unorm`),
/// and every slice read back by a blit into a buffer, rows `4 w` bytes apart; Metal's answer, the
/// first 16 hex digits of the SHA-256 of the probe's lines (`s<slice> r<row>:` and the row's
/// bytes).
type PadCase = (
    u32,
    [u64; 3],
    u64,
    [u64; 4],
    Result<&'static str, [f64; 4]>,
    &'static str,
);

const PAD_CASES: [PadCase; 9] = [
    (
        550,
        [7, 3, 1],
        0,
        [0, 0, 7, 3],
        Ok("0102030405060708"),
        "a22dfa6fdd93570d",
    ),
    (
        550,
        [7, 3, 1],
        0,
        [2, 1, 9, 5],
        Ok("090a0b"),
        "5b9fcd63521fb5dc",
    ),
    (
        551,
        [33, 5, 1],
        0,
        [3, 1, 20, 3],
        Err([1.0, 0.5, 0.25, 1.0]),
        "ce168b61643ecebb",
    ),
    (
        550,
        [6, 2, 1],
        0,
        [0, 0, 6, 2],
        Ok("0102030405060708"),
        "dcc033fcbab11d58",
    ),
    (
        550,
        [7, 3, 1],
        0,
        [0, 0, 0, 0],
        Ok("01"),
        "37ba266b851a2342",
    ),
    (
        550,
        [13, 4, 1],
        0,
        [1, 1, 5, 2],
        Ok("a1b2c3d4"),
        "94b181522346b0df",
    ),
    (
        550,
        [5, 3, 2],
        1,
        [0, 0, 5, 3],
        Ok("a1b2c3d4"),
        "fea04cfb6251b2ab",
    ),
    (
        550,
        [3, 4, 1],
        0,
        [0, 0, 3, 4],
        Ok("a1b2c3d4"),
        "683193cef365dc59",
    ),
    (
        551,
        [9, 3, 1],
        0,
        [1, 0, 7, 3],
        Ok("c1c2c3c4"),
        "b6719f0cdbc1e202",
    ),
];

/// `case`'s lines, as the runtime answers them.
fn pad_lines(device: &Device, queue: &CommandQueue, case: &PadCase) -> Vec<String> {
    let &(raw, [w, h, slices], slice, [x, y, rw, rh], fill, _) = case;
    let pf = PixelFormat::try_from(raw).unwrap();
    let mut d = metal2vulkan_runtime::TextureDescriptor::texture_2d(pf, w, h, false);
    if slices > 1 {
        d.texture_type = metal2vulkan::metal::TextureType::Type2DArray;
        d.array_length = slices;
    }
    d.usage = metal2vulkan::metal::TextureUsage::SHADER_READ;
    d.resource_options = metal2vulkan_runtime::ResourceOptions::private();
    let t = device.new_texture(&d).unwrap();
    let (row, image) = (4 * w, 4 * w * h);
    let source = buffer_with(device, &vec![0xee; image as usize]).unwrap();
    let out = buffer_with(device, &vec![0x55; (image * slices) as usize]).unwrap();
    let command_buffer = queue.command_buffer().unwrap();
    let mut e = command_buffer.blit_command_encoder().unwrap();
    for s in 0..slices {
        e.copy_from_buffer_to_texture(
            &source,
            0,
            row,
            image,
            Size::new(w, h, 1),
            &t,
            s,
            0,
            Origin::new(0, 0, 0),
            BlitOption::None,
        )
        .unwrap();
    }
    let region = Region::new_2d(x, y, rw, rh);
    match fill {
        Ok(pattern) => {
            let bytes: Vec<u8> = (0..pattern.len())
                .step_by(2)
                .map(|at| u8::from_str_radix(&pattern[at..at + 2], 16).unwrap())
                .collect();
            e.fill_texture(&t, 0, slice, region, &bytes).unwrap();
        }
        Err([r, g, b, a]) => {
            let color = metal2vulkan_runtime::ClearColor::new(r, g, b, a);
            e.fill_texture_color(&t, 0, slice, region, color, Some(PixelFormat::RGBA8Unorm))
                .unwrap();
        }
    }
    for s in 0..slices {
        e.copy_from_texture_to_buffer(
            &t,
            s,
            0,
            Origin::new(0, 0, 0),
            Size::new(w, h, 1),
            &out,
            s * image,
            row,
            image,
            BlitOption::None,
        )
        .unwrap();
    }
    e.end_encoding().unwrap();
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    assert_eq!(command_buffer.status(), CommandBufferStatus::Completed);
    let bytes = read_buffer(&out, 0, image * slices).unwrap();
    (0..slices)
        .flat_map(|s| (0..h).map(move |r| (s, r)))
        .map(|(s, r)| {
            let at = (s * image + r * row) as usize;
            let hex: String = bytes[at..at + row as usize]
                .iter()
                .map(|b| format!(" {b:02x}"))
                .collect();
            format!("s{s} r{r}:{hex}")
        })
        .collect()
}

/// A fill of a two-plane texture of the device's memory at a width off 4, read back by a blit
/// into a buffer, gives Metal's bytes: the words the fill (and the blit that set the texture up)
/// wrote between the rows read back where Metal's do.
#[test]
fn fills_read_back_between_rows_as_metal_s() {
    on_device(|device, messages| {
        let queue = device.new_command_queue().unwrap();
        let mut wrong = Vec::new();
        for (n, case) in PAD_CASES.iter().enumerate() {
            let lines = pad_lines(device, &queue, case);
            let sum = Sha256::digest(lines.join("\n").as_bytes());
            let got: String = sum.iter().take(8).map(|b| format!("{b:02x}")).collect();
            if got != case.5 {
                println!(
                    "XR-TWO-PLANE-COPIES-DIFFERS pad {n}: {got} (Metal's {}): {lines:?}",
                    case.5
                );
                wrong.push(n);
            }
        }
        messages.take_notices();
        assert!(wrong.is_empty(), "fills unlike Metal's: {wrong:?}");
    });
}

/// The SHA-256 of seed 1's planes and of seed 2's bytes, pinned beside the FNV-1a words the probe
/// printed of them, whose 32 bits alone leave a case's inputs loosely held.
const INPUT_SHA: [&str; 3] = [
    "bc4690d9570ceddf89ff6728f4e8c0288ae8a6141f24eac93edca8b0cf15c1fa",
    "120c1f4739647fc263a294ff9132f8546e4faddd27fa3cb1a517213734d7cf16",
    "6882fdfc3de4778888d9a66594366153eb4f4aedd40e58eb0c885e025256ffb4",
];

/// The generator gives the probe's texels: the FNV-1a digests the probe printed of seed 1's
/// planes and of seed 2's bytes (the probe's results), and their SHA-256.
#[test]
fn the_generator_is_the_probes() {
    use sha2::Digest;
    let fnv = |bytes: &[u8]| {
        bytes.iter().fold(2_166_136_261u32, |h, &b| {
            (h ^ u32::from(b)).wrapping_mul(16_777_619)
        })
    };
    let sha = |bytes: &[u8]| crate::hash::hex_encode(sha2::Sha256::digest(bytes));
    let (words, alpha) = planes(1, W * H);
    let big = random_bytes(2, BIG);
    assert_eq!(fnv(&words), 0xa7c5_fb98);
    assert_eq!(fnv(&alpha), 0x5cc7_aa14);
    assert_eq!(fnv(&big), 0x778b_70a0);
    for (bytes, want) in [&words, &alpha, &big].into_iter().zip(INPUT_SHA) {
        assert_eq!(sha(bytes), want);
    }
}
