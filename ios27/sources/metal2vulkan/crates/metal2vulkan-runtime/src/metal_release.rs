//! The macOS release whose Metal every device answers as, where Metal's answers differ by release:
//! [`MACOS_RELEASE`], on every host, whatever release the host runs.
//!
//! macOS 27.0.1 (build 26A434), measured on an Apple M3 Max; when a newer release is measured,
//! these answers move to it in place. An Apple M4's answers differ where both were measured in the
//! formats from 630 to 645, which it describes, in `gpuCoreCount` and in
//! `supportsComputeCompressedTextureWrite`.
//!
//! The release's answers are the ones below and two tables of their own: Metal's description of
//! each pixel format (`MTLPixelFormatGetInfoForDevice`, `pixel_format_info.rs`), which says which
//! formats Metal describes, whose textures and attachments its validation stops otherwise, and
//! Metal's private device properties (`private_properties.rs`). What a device can do, its GPU
//! family, features and limits, follows its GPU.

use crate::resource_ids::ResourceId;
use crate::texture::heap_tiled::{BlockFamily, BlockPlacement};

/// The macOS release whose Metal every device answers as, as macOS names it (the module docs).
/// No Metal counterpart: Metal answers as the release it runs on.
pub const MACOS_RELEASE: &str = "27.0.1";

/// How the release's Metal places a block-compressed texture's blocks of `family` in a heap
/// texture's levels' tiles (`texture/heap_tiled.rs`): BC, ETC2 and EAC `Narrow`, ASTC and PVRTC
/// `Halved`, its GPU counting a level's tiles a row as its CPU copies do.
///
/// Every BC, ETC2, EAC and ASTC format, every element of every level of 815 textures on an Apple
/// M4; the M3 Max answers the 809 of `tests/heap_tiled_blocks.rs` alike, each one's size,
/// alignment, CPU span and GPU span. PVRTC on the M3 Max: 320 answers and every block of six
/// textures' levels (a PVRTC texture is square, its side a power of two, which both placements
/// place alike).
pub(crate) const fn heap_blocks(family: BlockFamily) -> BlockPlacement {
    match family {
        BlockFamily::Bc | BlockFamily::Etc => BlockPlacement::Narrow,
        BlockFamily::Astc | BlockFamily::Pvrtc => BlockPlacement::Halved,
    }
}

/// The alignment the release's `heapTextureSizeAndAlignWithDescriptor:` answers, with 0 bytes,
/// for a texture of `Unspecialized`, `RGBA16Float_XR`, `AnyDepth` or `AnyStencil` of a type with
/// layers, faces or depth: a 1D or 2D array, a cube, a cube array, a 3D texture or a 2D
/// multisample array (`texture/heap.rs`'s `no_texture_alignment`). Measured on the M3 Max (and an
/// Apple M4 alike): every type but a texture buffer 128, private and shared storage, `ShaderRead`
/// and `ShaderWrite` usage.
pub(crate) const NO_TEXTURE_STACK_ALIGNMENT: u64 = 128;

/// What the release's plain Metal answers for a depth/stencil state's `gpuResourceID` on a device
/// that does not claim Apple10 (`depth_stencil.rs`): 0. Our probe on the M3 Max (plain Metal): 0
/// for a depth state and for a depth-and-stencil one, the call returning; an earlier run of three
/// states over two runs answered 0 alike. Metal's own API validation stops the call ("Obtaining
/// the resourceID of a DepthStencilState object requires support for MTLGPUFamilyApple10"), which
/// is not what plain Metal does.
pub(crate) const DEPTH_STENCIL_RESOURCE_ID: ResourceId = ResourceId(0);
