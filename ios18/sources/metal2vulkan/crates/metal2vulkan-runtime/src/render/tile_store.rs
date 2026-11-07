//! What a render pass's store keeps of the texels its draws do not write, where Metal's tile store
//! rewrites them: the kept-attachment notices a draw announces.
//!
//! - **Metal's answer** (an Apple M3 Max under macOS 27.0.1, and an Apple M4). An Apple GPU stores
//!   a render pass's attachments a 32 x 32 tile at a time, and a pass that loads a color attachment
//!   and records a draw writes every texel of each tile a draw's primitives reach back, through its
//!   tile memory: the texels no fragment covers, fragments that discard or fail a test, and
//!   channels a write mask leaves out among them. A tile no draw reaches keeps its bytes, and a
//!   pass that draws nothing keeps every texel.
//!   - An extended-range or padded 10-bit attachment (`BGR10_XR`, `BGRA10_XR`, `RGB10A8_2P_XR10`,
//!     `R10Unorm_X6`, `RG10Unorm_X12` or one of their `_sRGB` twins) keeps each texel's codes with
//!     the bits they leave unused made canonical: `BGR10_XR`'s bits 30 and 31 ones,
//!     `BGRA10_XR`'s low six bits of each field zeros and its alpha code clamped to `[384, 894]`,
//!     the two-plane color word's top two bits zeros, a padded word's low six bits zeros, a padded
//!     `_sRGB` twin's codes through Metal's table.
//!   - A bfloat attachment (`R16Bfloat`, `RG16Bfloat`, `RGBA16Bfloat`) holds `half` values in its
//!     tiles: every code of each tile it stores back goes through `half` to nearest even and back
//!     to the nearest bfloat code, a NaN to `0x7fc0` (on the M4, every code of each format: 56,829
//!     move; on the M3 Max our probe `bfl`, every code of the tile a small triangle reached).
//!   - A single-sampled signed normalized attachment stores a channel's minimum code (-128, or
//!     -32768) as the code above it, and an `RGB9E5Float` one normalizes a word whose largest
//!     mantissa is below 256 under an exponent above 0 (the M3 Max).
//! - **The runtime's answer: the device's load and store.** The device's pass keeps every byte its
//!   draws do not write (`VK_ATTACHMENT_STORE_OP_STORE` of a loaded attachment), on every device:
//!   no code is rewritten before or after the pass. A draw announces what differs, naming the
//!   attachments of its pass: `extended-range-bits-kept` for an extended-range, padded or bfloat
//!   one ([`bits_kept`]), `snorm-minimum-kept` ([`snorm_minimum_kept`]) and
//!   `shared-exponent-kept` ([`shared_exponent_kept`]), the last two but on a driver whose pass
//!   stores them as Metal's (`DriverFact::TileStoreIsMetals`: MoltenVK's pass is a Metal pass). A
//!   pass whose store discards the attachment (`DontCare`) keeps nothing either way. The first
//!   notice holds on MoltenVK too: an extended-range, padded or bfloat attachment is an image of
//!   an unsigned integer format on every device (`metal::format::ExtendedRange`), whose codes a
//!   Metal pass stores as they are.

use crate::device::Device;
use crate::render_pass::{ColorTarget, PassLayout, Target};
use crate::render_values::{LoadAction, StoreAction};
use metal2vulkan::device::DriverFact;
use metal2vulkan::metal::format::ExtendedRange;
use metal2vulkan::metal::PixelFormat;

/// The texture's own target of `color`: a stand-in's (`attachment_stand_in.rs`) is the one it
/// stands in for.
fn own(color: &ColorTarget) -> &Target {
    color.stand_in.as_ref().map_or(&color.target, |s| &s.own)
}

/// Whether `device`'s render pass stores a signed normalized minimum code and an `RGB9E5Float`
/// word as Metal's tile store does (`DriverFact::TileStoreIsMetals`: MoltenVK's pass is a Metal
/// pass).
pub(super) fn metals_store(device: &Device) -> bool {
    device
        .properties()
        .driver_facts
        .contains(&DriverFact::TileStoreIsMetals)
}

/// Whether `color` is an attachment of an extended-range, padded 10-bit or bfloat format, loaded
/// and not discarded at the store (a store action the encoder decides later counts as storing):
/// Metal's pass, once it draws, rewrites the texels its draws do not write, where the device's
/// pass keeps their bytes (the module docs).
pub(super) fn bits_kept(color: &ColorTarget) -> bool {
    let discarded = color.store == StoreAction::DontCare && !color.late_store;
    ExtendedRange::of(own(color).texture.pixel_format()).is_some()
        && color.load == LoadAction::Load
        && !color.memoryless
        && !discarded
}

/// The signed normalized color formats whose minimum code Metal's tile store rewrites, as
/// measured on an Apple M3 Max (`snorm-minimum-kept`).
const SNORM_MINIMUM: [PixelFormat; 6] = [
    PixelFormat::R8Snorm,
    PixelFormat::RG8Snorm,
    PixelFormat::RGBA8Snorm,
    PixelFormat::R16Snorm,
    PixelFormat::RG16Snorm,
    PixelFormat::RGBA16Snorm,
];

/// Whether `color` is a single-sampled attachment of a signed normalized format
/// ([`SNORM_MINIMUM`]), loaded and not discarded at the store: Metal's pass, once it draws, writes
/// each tile a draw reaches back through the channels' values, so a minimum code (-128, or -32768)
/// is stored as the code above it, where the device's pass keeps the bytes its draws do not write
/// (`degrade_snorm_minimum`, which reads `DriverFact::TileStoreIsMetals`: a driver whose pass
/// stores them as Metal's).
pub(super) fn snorm_minimum_kept(color: &ColorTarget) -> bool {
    kept_through_store(color, |format| SNORM_MINIMUM.contains(&format))
}

/// Whether `color` is a single-sampled `RGB9E5Float` attachment, loaded and not discarded at the
/// store: Metal's pass, once it draws, writes each tile a draw reaches back through its texels'
/// values, so a word whose largest mantissa is below 256 under an exponent above 0 (one no Metal
/// write leaves) is stored normalized, its mantissas shifted up as its exponent steps down, where
/// the device's pass keeps the bytes its draws do not write (`degrade_shared_exponent`, which reads
/// `DriverFact::TileStoreIsMetals`).
pub(super) fn shared_exponent_kept(color: &ColorTarget) -> bool {
    kept_through_store(color, |format| format == PixelFormat::RGB9E5Float)
}

/// Whether `color` is a single-sampled attachment of a format `of` takes, loaded and not
/// discarded at the store.
fn kept_through_store(color: &ColorTarget, of: impl Fn(PixelFormat) -> bool) -> bool {
    let target = own(color);
    let discarded = color.store == StoreAction::DontCare && !color.late_store;
    of(target.texture.pixel_format())
        && target.samples == 1
        && color.load == LoadAction::Load
        && !color.memoryless
        && !discarded
}

/// The color attachments of a pass's layout that each of a draw's kept-attachment notices names, as
/// `color {index}, {format:?}`: the extended-range, padded and bfloat ones ([`bits_kept`],
/// `degrade_unused_bits`) and, on a driver whose pass does not store as Metal's
/// ([`metals_store`]), the signed normalized ones ([`snorm_minimum_kept`],
/// `degrade_snorm_minimum`) and the `RGB9E5Float` ones ([`shared_exponent_kept`],
/// `degrade_shared_exponent`). They read the layout's colors, their textures' formats and samples,
/// and the device's driver facts; all but the layout are fixed at creation, so an encoder decides
/// them once for each layout it records under.
#[derive(Debug, Default, PartialEq)]
pub(super) struct KeptAttachments {
    pub(super) unused_bits: Vec<String>,
    pub(super) snorm_minimum: Vec<String>,
    pub(super) shared_exponent: Vec<String>,
}

impl KeptAttachments {
    /// The kept attachments of `layout`, a pass of `device`'s.
    pub(super) fn of(layout: &PassLayout, device: &Device) -> Self {
        let metals = metals_store(device);
        let mut kept = Self::default();
        for (index, color) in layout.colors.iter().enumerate() {
            let Some(color) = color else {
                continue;
            };
            let name = || format!("color {index}, {:?}", color.target.texture.pixel_format());
            if bits_kept(color) {
                kept.unused_bits.push(name());
            }
            if !metals && snorm_minimum_kept(color) {
                kept.snorm_minimum.push(name());
            }
            if !metals && shared_exponent_kept(color) {
                kept.shared_exponent.push(name());
            }
        }
        kept
    }
}
