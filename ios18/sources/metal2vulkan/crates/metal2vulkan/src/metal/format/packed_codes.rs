//! The two formats whose texels are a stream of packed 10-bit codes: `R10Unorm_PACKED` (588) and
//! `RG10Unorm_PACKED` (589).
//!
//! A code crosses byte boundaries, so a texel has no whole number of bytes: the codes run three
//! to a little-endian 32-bit word, from the word's low bits up (bits 0-9, 10-19, 20-29, the top
//! two unread), and a texel takes **one** code of that stream for `R10Unorm_PACKED` and **two**
//! for `RG10Unorm_PACKED`. So three texels of the first lie in one word and three of the second in
//! two — 4/3 and 8/3 bytes a texel — and a row of either starts a fresh stream.
//!
//! **What the two anchors below fix, and what they do not.** The codes of texels 0 and 1 are
//! measured, and they are bits 0-9 and 10-19 of the row's first word, which a word of three codes
//! and an unpadded bit stream of codes agree on. The first texel that tells those two readings
//! apart is `R10Unorm_PACKED`'s texel 3 — the fourth code, at the second word's bit 0 under three
//! to a word and at the stream's bit 30 without the padding — and no code of ours is measured
//! there. So the two unread bits at the top of each word are this module's reading of the
//! measurement's own words ("three 10-bit codes per 32-bit word") and not a figure held to a
//! texel, and a row of four or more texels is where an implementation of the read has to be
//! measured before it is believed.
//!
//! **Metal lays out no image storage for them.** That one rule is what its creators show: every
//! creator that would have to lay the image out itself answers nil, and every creator that makes
//! one either holds nothing (0 or 16 bytes) or lies in memory the caller already owns.
//! [`packed_codes`] is that rule's structural test, and
//! [`Footprint::NoTexture`](super::Footprint::NoTexture) — which it is the whole membership of —
//! is how a caller outside this crate asks it, as well as being the 0 bytes the heap size query
//! counts for such a format.
//!
//! # Metal's answers
//!
//! Measured on an Apple M3 Max under macOS 27.0.1 (26A434), plain Metal with no API validation
//! layer, a 64 x 64 2D texture under `ShaderRead` usage, one cell per fresh process and four
//! identical runs, with an `RGBA8Unorm` control made beside every cell so that a cell's nil could
//! not be read as its storage mode's:
//!
//! | creator | storage | answer |
//! |---|---|---|
//! | `newTextureWithDescriptor:` | `Shared`, `Managed`, `Private` | nil |
//! | an **automatic** heap's `newTextureWithDescriptor:` | `Shared`, `Private` | nil |
//! | a **placement** heap's `newTextureWithDescriptor:offset:` at 0 | `Shared`, `Private` | a texture, `allocatedSize` **0** |
//! | `newSharedTextureWithDescriptor:` | `Private` | a texture, `allocatedSize` **16** (the control's 16528) |
//! | a buffer's `newTextureWithDescriptor:offset:bytesPerRow:` at 256 | `Shared`, `Private` | a texture, `allocatedSize` 65536, the buffer's own |
//!
//! `newSharedTextureWithDescriptor:` answers nil in `Shared` and `Managed` storage for these
//! formats and for the control alike, which is the storage mode's rule and says nothing about the
//! format. The shared creator's texture answers `iosurface` nil, `buffer` nil and a **non-nil**
//! `newSharedTextureHandle`, so its 16 bytes are not an IOSurface wrapper.
//!
//! `Memoryless` storage from the device's own creator answers a **texture** of both, where every
//! mode that holds memory answers nil (an earlier probe on the same venue: a 2D 8 x 8 texture, 4
//! of 4 runs under each of `ShaderRead`, `ShaderRead | ShaderWrite`,
//! `ShaderRead | RenderTarget` and `RenderTarget` usage). A memoryless texture holds no memory
//! either, so it is the same rule.
//!
//! # What a made texture does
//!
//! Over a `Shared` buffer at 256 bytes a row, and over an IOSurface at 4 bytes a texel and 256
//! bytes a row, each row filled with the byte pattern `(i ^ (i >> 8)) & 0xff` from the row's
//! start (row `r` at `r * 256`), on the same venue:
//!
//! - **A read and a sample are the code stream.** `R10Unorm_PACKED`'s texel 0 reads
//!   `0x100 / 1023` (0.250244) out of the row's first word `0x03020100` and its texel 1
//!   `0x080 / 1023`, which is the word's bits 0-9 and 10-19; `RG10Unorm_PACKED` takes two codes
//!   a texel from the same stream. `read` equals `sample` at every texel measured, and the
//!   IOSurface's texture reads the buffer's value for value.
//! - **A blit moves 64 bytes a row** for **both** formats at width 64 (4096 bytes of 16384), the
//!   source's own bytes.
//! - **`getBytes:` writes nothing**: 0 bytes of 16384, the destination's 0xEE fill left whole,
//!   where the `RGBA8Unorm` control writes 16320.
//! - **A row pitch has a floor Metal asserts on**, its own validation with no layer: 64 bytes at
//!   width 64 for `R10Unorm_PACKED` (63, 48, 32 and 16 each end the process) and 128 for
//!   `RG10Unorm_PACKED`, so one and two bytes a texel.
//! - **A render pass into one hangs the GPU.** A pass into `R10Unorm_PACKED` over a buffer left
//!   its command buffer in status 5 with `kIOGPUCommandBufferCallbackErrorHang`. No
//!   `RG10Unorm_PACKED` and no IOSurface render cell was run, and none may be: the hang takes
//!   unrelated work on the machine down with it.
//!
//! # Metal's own answers disagree with each other
//!
//! Two of those rows contradict Metal's other answers for the same format, which is Metal's
//! inconsistency and not a layout a rule could be fitted to:
//!
//! - A blit of `RG10Unorm_PACKED` moves 64 bytes a row where the same format's row pitch has to
//!   be at least 128: Metal's copy and its own stride validation disagree about the format's
//!   bytes a texel.
//! - `getBytes:` writes nothing for either format, where a blit of the same texture moves its
//!   rows: Metal's CPU and GPU paths disagree about whether the format has a layout at all.
//!
//! Both floors are below the stream's own 4/3 and 8/3 bytes a texel, so Metal's stride validation
//! undercounts the layout its own reads use.

use crate::metal::PixelFormat;

/// Whether `pf` is one of the two packed 10-bit formats, for which Metal lays out no image
/// storage (the module docs): `R10Unorm_PACKED` and `RG10Unorm_PACKED`.
///
/// This is the structural test a creator's rule asks: Metal makes a texture of such a format
/// exactly where the creator needs no image laid out — a placement heap's offset, a shared
/// texture of 16 bytes, a texture over the caller's buffer or IOSurface plane, and `Memoryless`
/// storage — and answers nil from the device's own creator and from an automatic heap, which
/// would each have to lay the image out. It is **not** support for the format: this crate holds
/// no image of a packed code stream, so a texture of one answers
/// `Unsupported(Format, NotImplemented)` from the creators Metal makes one from.
pub(crate) fn packed_codes(pf: PixelFormat) -> bool {
    matches!(
        pf,
        PixelFormat::R10UnormPACKED | PixelFormat::RG10UnormPACKED
    )
}
