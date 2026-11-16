//! A texture a case describes whole: `MTLTextureDescriptor` as the format families need one.
//!
//! [`crate::case::TextureSpec`] says "a one-level 2D texture of shared storage, in this role, at
//! this row pitch", which is what the first cases needed. The format families need the rest of the
//! descriptor: a level count, an array's slices, a cube's faces, private storage,
//! `allowGPUOptimizedContents`, a swizzle, and a block-compressed format whose level 0 is loaded
//! from block bytes rather than texel bytes. They also load more than one level, so the bytes a
//! texture starts from are a list of `replaceRegion:` calls instead of one.
//!
//! The vocabulary is Metal's own, as the translator's public API names it
//! (`MTLTextureType`, `MTLTextureUsage`, `MTLTextureSwizzleChannels`), so a value a case writes
//! reaches both sides' descriptors unchanged and there is no second spelling to keep in step. The
//! format is a raw `MTLPixelFormat`, as elsewhere in the suite, so a case may name one of Apple's
//! private formats as readily as a public one.
//!
//! Nothing here reads a device name, a vendor or an environment variable: a case that asks for a
//! texture the venue cannot make finds out from the typed refusal the call answers.

use crate::case::Handle;
use metal2vulkan::metal::{TextureSwizzleChannels, TextureType, TextureUsage};

/// One `replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:` a texture is loaded
/// with as it is made.
///
/// A compressed format states its pitches in **blocks**, as Metal takes them, while its origin and
/// size stay in texels: that difference is the whole subject of the compressed families, so the
/// case states both rather than either side computing one from the other.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Upload<'a> {
    pub level: u32,
    pub slice: u32,
    /// The region's origin in texels.
    pub origin: [u32; 3],
    /// The region's size in texels.
    pub size: [u32; 3],
    pub bytes: &'a [u8],
    /// `bytesPerRow`: bytes from one row of blocks to the next.
    pub row_pitch: u32,
    /// The rows of bytes the upload hands, which is the region's **block** rows: a side checks
    /// `bytes` holds them before the call reads, and a compressed format's block rows are not its
    /// texel rows, which is exactly the difference these families are about.
    pub rows: u32,
    /// `bytesPerImage`: bytes from one image to the next; 0 for a texture of one image.
    pub image_pitch: u32,
}

impl Upload<'_> {
    /// Level 0, slice 0, the whole of a 2D extent, `rows` rows of `row_pitch` bytes.
    pub const fn level_0(size: [u32; 2], row_pitch: u32, rows: u32, bytes: &[u8]) -> Upload<'_> {
        Upload {
            level: 0,
            slice: 0,
            origin: [0, 0, 0],
            size: [size[0], size[1], 1],
            bytes,
            row_pitch,
            rows,
            image_pitch: 0,
        }
    }

    /// The bytes the upload must hold for the call to read inside them.
    pub const fn needed(&self) -> usize {
        self.row_pitch as usize * self.rows as usize
    }

    /// The words the upload's parameters fingerprint as, beside its bytes.
    pub fn words(&self) -> Vec<u8> {
        let mut out = Vec::new();
        for word in [self.level, self.slice] {
            out.extend(word.to_le_bytes());
        }
        for axis in self.origin.iter().chain(&self.size) {
            out.extend(axis.to_le_bytes());
        }
        for word in [self.row_pitch, self.rows, self.image_pitch] {
            out.extend(word.to_le_bytes());
        }
        out.extend(self.bytes);
        out
    }
}

/// What a texture is made **over**, where it is not made of memory of its own.
///
/// Both forms are one Metal call that answers a texture, so they belong to
/// [`Side::texture`](crate::case::Side::texture) rather than to methods of their own: the
/// descriptor a case states is the same descriptor, and what changes is which object the call is
/// sent to. A request that names neither is `newTextureWithDescriptor:` as before.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Over {
    /// `newTextureWithDescriptor:offset:bytesPerRow:` of a buffer the case made: the texture's
    /// texels **are** the buffer's bytes from `offset`, its rows `bytes_per_row` apart.
    ///
    /// A texture buffer's one row is its texels, so its `bytes_per_row` is the whole run of them,
    /// which is what Metal takes and what the probes passed.
    Buffer {
        buffer: Handle,
        offset: u64,
        bytes_per_row: u64,
    },
    /// `newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:` of a texture the case
    /// made: the view takes **this request's** format, texture type, swizzle, `0..levels` and
    /// `0..slices`, replacing the parent texture's.
    ///
    /// A view holds no memory, so a request that names one loads nothing: the case loads the
    /// parent and then views it, which is the order a swizzled view's answer depends on.
    View { texture: Handle },
}

/// A texture a case makes, as `MTLTextureDescriptor` describes one, with the bytes it starts from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextureRequest<'a> {
    /// The raw `MTLPixelFormat`, so a case may name one of Apple's private formats.
    pub format: u32,
    pub texture_type: TextureType,
    /// `width`, `height` and `depth`, in texels.
    pub size: [u32; 3],
    /// `mipmapLevelCount`.
    pub levels: u32,
    /// `arrayLength`: an array type's slices, 1 otherwise. A cube's six faces are its slices and
    /// not its array length, as Metal counts them.
    pub slices: u32,
    pub usage: TextureUsage,
    /// Whether the storage is private; shared otherwise, so the host reads it back.
    pub private: bool,
    /// `allowGPUOptimizedContents`.
    pub gpu_optimized: bool,
    pub swizzle: TextureSwizzleChannels,
    /// What the texture is made over, where it is not made of its own memory ([`Over`]). `None` is
    /// `newTextureWithDescriptor:`.
    pub over: Option<Over>,
    /// The `replaceRegion:` calls the texture is loaded with, in order.
    pub uploads: &'a [Upload<'a>],
}

impl<'a> TextureRequest<'a> {
    /// A one-level 2D texture of `format`, `width` by `height`, read by a shader, in shared
    /// storage, GPU-optimized as Metal's descriptor is by default, with the default swizzle and
    /// nothing loaded.
    pub const fn read_2d(format: u32, width: u32, height: u32) -> Self {
        Self {
            format,
            texture_type: TextureType::Type2D,
            size: [width, height, 1],
            levels: 1,
            slices: 1,
            usage: TextureUsage::SHADER_READ,
            private: false,
            gpu_optimized: true,
            swizzle: TextureSwizzleChannels::DEFAULT,
            over: None,
            uploads: &[],
        }
    }

    /// A one-level texture buffer of `format` over `texels` texels of `buffer` from `offset`, read
    /// by a shader, with the default swizzle: Metal's
    /// `textureBufferDescriptorWithPixelFormat:width:resourceOptions:usage:` over a buffer.
    ///
    /// `bytes_per_row` is the whole run of texels, which is what a texture buffer's one row is.
    pub const fn texel_buffer(
        format: u32,
        texels: u32,
        buffer: Handle,
        offset: u64,
        bytes_per_row: u64,
    ) -> Self {
        Self {
            format,
            texture_type: TextureType::TextureBuffer,
            size: [texels, 1, 1],
            levels: 1,
            slices: 1,
            usage: TextureUsage::SHADER_READ,
            private: false,
            gpu_optimized: true,
            swizzle: TextureSwizzleChannels::DEFAULT,
            over: Some(Over::Buffer {
                buffer,
                offset,
                bytes_per_row,
            }),
            uploads: &[],
        }
    }

    /// The same texture as a view of `texture`: this request's format, type, levels, slices and
    /// swizzle, over the parent's memory.
    #[must_use]
    pub const fn viewing(mut self, texture: Handle) -> Self {
        self.over = Some(Over::View { texture });
        self
    }

    /// The same texture with `levels` mip levels.
    #[must_use]
    pub const fn levels(mut self, levels: u32) -> Self {
        self.levels = levels;
        self
    }

    /// The same texture in private storage, which no host call reaches.
    #[must_use]
    pub const fn private(mut self) -> Self {
        self.private = true;
        self
    }

    /// The same texture in `usage`.
    #[must_use]
    pub const fn used(mut self, usage: TextureUsage) -> Self {
        self.usage = usage;
        self
    }

    /// The same texture as a cube of `side` texels a face.
    #[must_use]
    pub const fn cube(mut self, side: u32) -> Self {
        self.texture_type = TextureType::Cube;
        self.size = [side, side, 1];
        self
    }

    /// The same texture read through `swizzle`.
    #[must_use]
    pub const fn swizzled(mut self, swizzle: TextureSwizzleChannels) -> Self {
        self.swizzle = swizzle;
        self
    }

    /// The same texture loaded with `uploads`.
    #[must_use]
    pub const fn loaded(mut self, uploads: &'a [Upload<'a>]) -> Self {
        self.uploads = uploads;
        self
    }

    /// The words the request fingerprints as: every field of the descriptor and every upload, so a
    /// texture whose shape or starting bytes move is a change to the case's inputs rather than a
    /// quiet pass against a recording of other bytes.
    ///
    /// [`TextureRequest::over`] is written **only where the case states one**, and a handle's
    /// number never is: a handle is a side's own counter, and a case states the resource underneath
    /// as an input of its own. So a request made as it was before this field existed fingerprints
    /// as it did, and every committed recording of one stays the answer to the inputs it was taken
    /// from.
    pub fn words(&self) -> Vec<u8> {
        let mut out = Vec::new();
        out.extend(self.format.to_le_bytes());
        out.extend(self.texture_type.as_raw().to_le_bytes());
        for axis in &self.size {
            out.extend(axis.to_le_bytes());
        }
        for word in [self.levels, self.slices, self.usage.as_raw()] {
            out.extend(word.to_le_bytes());
        }
        out.push(u8::from(self.private));
        out.push(u8::from(self.gpu_optimized));
        for channel in [
            self.swizzle.red,
            self.swizzle.green,
            self.swizzle.blue,
            self.swizzle.alpha,
        ] {
            out.extend(channel.as_raw().to_le_bytes());
        }
        match self.over {
            None => {}
            Some(Over::Buffer {
                offset,
                bytes_per_row,
                buffer: _,
            }) => {
                out.extend(b"over-buffer");
                out.extend(offset.to_le_bytes());
                out.extend(bytes_per_row.to_le_bytes());
            }
            Some(Over::View { texture: _ }) => out.extend(b"view-of"),
        }
        for upload in self.uploads {
            out.extend(upload.words());
        }
        out
    }
}
