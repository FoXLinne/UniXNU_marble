//! A host shadow of a texture's texels, for the textures whose CPU reads a driver defect keeps off
//! the host (`texture/host_copy.rs`): made at the first CPU read of the texture, never as it is
//! made, and read from by every CPU read after it while it holds the image's texels.
//!
//! **Why.** A `Shared` or `Managed` texture whose format packs more than one texel into a block
//! takes no host-transfer usage on a driver that copies such an image out of its host-visible
//! memory as the memory is bound (`RuntimeDriverDefect::HostTransferImagesCopiedAtBind`,
//! MoltenVK: `driver_defects.rs` has the backtrace). Its CPU copies then go through a staging
//! buffer, and a `getBytes` of it waits for an internal submission of its own: the suite's
//! `compressed-moves-bc1-rgba` read `internal_submissions` 56 on an M3 Max, one a `getBytes`,
//! where every one of its `replaceRegion`s staged its bytes for the device's next submission and
//! waited for none (`texture/first_move.rs`).
//!
//! A `replaceRegion` cannot leave the GPU's copy of the texels unwritten, so the write keeps its
//! staged copy. A `getBytes` can: the bytes it answers are the image's, and the host may hold
//! them. So the shadow serves reads alone, and a write keeps it current rather than dropping it.
//!
//! - **Which textures** ([`Image::shadowed`]). An image the defect keeps off the host path: the
//!   device copies on the host (`host_copy::reaches_general`) and this image's format does not
//!   (`host_copy::reaches_format`), so the defect alone is what moved it, and it copies on no
//!   staging queue either. It must be an image of the runtime's own in memory of its own, not
//!   shareable (a shared texture's bytes another process may write), of one sample and one colour
//!   aspect, not a two-plane texture, and not a carrier holding another
//!   format's blocks or a 4:2:2 texture's texels (`pvrtc.rs`, `astc.rs`, `chroma_pairs.rs`), whose
//!   CPU copies name regions in the carrier's units and not the texture's. A heap's texture, a
//!   texture over a buffer, a sparse one, the caller's and a shared one are left out: another
//!   resource, a tile map or another process writes their bytes, and no commit of the program's
//!   says so. The whole image's texels must come to at most [`SHADOW_BYTES`].
//! - **Made at the first CPU read**, by the submission that read needs anyway: one staging buffer
//!   of the whole image's packed texels, one `vkCmdCopyImageToBuffer` a subresource in it, and the
//!   read served out of it (`WorkCounts::texel_shadows_made`). A texture the program never reads
//!   back has none, and pays nothing: that is the whole of why it is made here and not as the
//!   texture is made. A read past [`SHADOW_BYTES`], or one the making of a shadow fails for, takes
//!   the submission path as before.
//! - **Read from** by every CPU read of the texture while the shadow holds the image's texels,
//!   with no submission and no wait (`WorkCounts::texel_shadow_reads`), each read laying the
//!   region's rows out of the shadow exactly as it would lay them out of a staging buffer the
//!   device had just filled.
//! - **Kept current by a CPU write.** `replaceRegion` writes the region's packed bytes into the
//!   shadow as it stages them for the image: the same bytes reach both, so the shadow holds the
//!   image's texels still.
//! - **Dropped wherever anything else may write the texels** (`WorkCounts::texel_shadows_dropped`),
//!   which is what makes it exact:
//!   - **every commit of the program's** (`CommandBuffer::commit`, `commit_failed`, and a commit
//!     the device's lowering thread runs) raises the device's count of the program's GPU work
//!     (`Shared::gpu_work`), and a shadow taken at an earlier count is dropped unread. A commit is
//!     the only way any command of the program's reaches the device, so no route of its GPU work
//!     needs naming here: a blit into the texture or into a view of it, an indirect command
//!     buffer's commands, a copy back of an undeclared-usage shadow, a resource state encoder's
//!     command, a clear. The count is raised at the commit and not at the submission, so the
//!     shadow is gone before any of that work can run.
//!   - **the program's own Vulkan submissions** (`CommandQueue::with_vk_queue`) raise it too.
//!   - **the program taking the image's `VkImage`** (`Texture::vk_image`) turns the shadow off for
//!     that image for good: the caller may write through the handle whenever it likes, and no call
//!     of the runtime's sees it.
//!   - **another `VkImage`**: the shadow holds the handle it was read out of, and a shadow of
//!     another one is dropped. That covers every swap: a stand-in presented while the texture's
//!     bytes are detached (`detach.rs`, `stand_in.rs`), a rebind's new image (`rebind.rs`) and a
//!     fetched render target's storage form (`texture/storage_form.rs`).
//!   - **a detach**: a texture presenting a stand-in has no shadow, and a detach drops it.
//! - **A race is the program's**, as on the other two paths (`texture/host_copy.rs`): a `getBytes`
//!   that runs while a command buffer the program committed is in flight reads texels the command
//!   may or may not have written, and Metal's reads them the same way. The commit raised the count
//!   before the call, so such a read never comes out of the shadow.

use super::{buffer_image_copy, image_aspects, transfer_barrier, Image, Owner, Staging};
use crate::copy_plan::{copy_unit, BlitOption, CopyUnit, HostLayout, ImageRegion};
use crate::device::lock;
use crate::error::Error;
use ash::vk;
use metal2vulkan::error::RuntimeDriverDefect;
use metal2vulkan::vk::{ImageAspectFlags, ImageType, SampleCountFlags};

/// The most of an image's texels a host shadow holds: past it the texture's CPU reads take the
/// submission path as before.
///
/// The shadow is made whole at the first CPU read, by the submission that read needs anyway, so
/// this is both the host memory one texture's shadow may hold and the texels one read may move
/// beyond the ones it asked for. A `BC1_RGBA` 4096-square texture with every level comes to
/// 11 MiB and a `BC3_RGBA` 2048-square one to 5.6 MiB, so the textures a consumer uploads and
/// reads back lie inside it, and the one read that pays for the whole image pays at most this.
pub(super) const SHADOW_BYTES: u64 = 16 << 20;

/// A texture's image's texels on the host (the module docs).
#[derive(Default)]
pub(super) enum HostShadow {
    /// No CPU read has asked for one yet.
    #[default]
    Unmade,
    /// Never one: the program took the image's `VkImage` (`interop.rs`), or the making of a shadow
    /// failed.
    Off,
    /// The image's texels as of a `VkImage` and a count of the program's GPU work.
    Made(Shadowed),
}

/// An image's texels on the host.
pub(super) struct Shadowed {
    /// The `VkImage` they were read out of: the shadow holds the image's texels only while the
    /// image presents this one (the module docs).
    raw: vk::Image,
    /// The device's count of the program's GPU work when they were read, and as every CPU write
    /// since left it.
    gpu_work: u64,
    /// Every subresource's packed texels, in [`Layout`]'s order.
    texels: Vec<u8>,
}

/// Where each of an image's subresources' packed texels lie in a shadow of it: the copy unit's
/// bytes and block, the image's level-0 extent, its levels and layers, and whether its levels hold
/// depth slices of their own (a 3D image).
///
/// A subresource's texels are its level's rows of blocks, tightly packed, each row
/// `ceil(width / block.0) * bytes` long and the level holding `ceil(height / block.1)` of them per
/// depth slice: the layout a copy through a staging buffer reads and writes
/// (`copy_plan::shape`), so a region's rows lie in the shadow exactly as that buffer would hold
/// them. Levels lie outermost and each level's layers in order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Layout {
    bytes: u64,
    block: [u32; 2],
    extent: [u32; 3],
    levels: u32,
    layers: u32,
    volume: bool,
}

impl Layout {
    /// The layout of a shadow of an image of `extent`, `levels` and `layers` (`volume`: a 3D
    /// image), copied in units of `unit`.
    pub(crate) fn new(
        extent: [u32; 3],
        levels: u32,
        layers: u32,
        volume: bool,
        unit: &CopyUnit,
    ) -> Layout {
        Layout {
            bytes: u64::from(unit.bytes),
            block: unit.extent,
            extent,
            levels: levels.max(1),
            layers: layers.max(1),
            volume,
        }
    }

    /// Level `level`'s extent in texels, its depth 1 for an image whose levels hold one slice.
    fn level(&self, level: u32) -> [u32; 3] {
        let mut out = (self.extent).map(|axis| (axis >> level.min(31)).max(1));
        if !self.volume {
            out[2] = 1;
        }
        out
    }

    /// Level `level`'s packed row bytes, rows a depth slice and depth slices.
    fn shape(&self, level: u32) -> (u64, u64, u64) {
        let [w, h, d] = self.level(level);
        let row = u64::from(w.div_ceil(self.block[0])) * self.bytes;
        (row, u64::from(h.div_ceil(self.block[1])), u64::from(d))
    }

    /// The bytes one subresource of `level` holds.
    fn subresource(&self, level: u32) -> u64 {
        let (row, rows, slices) = self.shape(level);
        row * rows * slices
    }

    /// Where the texels of `layer` of `level` start.
    fn at(&self, level: u32, layer: u32) -> u64 {
        let before: u64 = (0..level.min(self.levels))
            .map(|l| self.subresource(l) * u64::from(self.layers))
            .sum();
        before + u64::from(layer) * self.subresource(level)
    }

    /// The bytes a shadow of the whole image holds.
    pub(crate) fn total(&self) -> u64 {
        self.at(self.levels, 0)
    }

    /// Every subresource as the region a copy of it names, with where its texels lie: levels
    /// outermost, each level's layers in order, a 3D level's depth slices in one region.
    pub(crate) fn regions(&self) -> Vec<(ImageRegion, u64)> {
        let mut out = Vec::with_capacity((self.levels as usize) * (self.layers as usize));
        for level in 0..self.levels {
            let [w, h, d] = self.level(level);
            for layer in 0..self.layers {
                out.push((
                    ImageRegion {
                        aspect: ImageAspectFlags::COLOR,
                        level,
                        layer,
                        layers: 1,
                        origin: [0; 3],
                        size: [w, h, d],
                    },
                    self.at(level, layer),
                ));
            }
        }
        out
    }

    /// Whether `region` lies in the image this is the layout of, in whole blocks: a region a CPU
    /// copy planned does (`copy_plan::check_region` bounds it and starts it at a block), and one
    /// that does not is never read or written here.
    fn holds(&self, region: &ImageRegion) -> bool {
        if region.aspect != ImageAspectFlags::COLOR || region.layers != 1 {
            return false;
        }
        if region.level >= self.levels || region.layer >= self.layers {
            return false;
        }
        let [w, h, d] = self.level(region.level);
        let [x, y, z] = region.origin;
        let [sx, sy, sz] = region.size;
        x % self.block[0] == 0
            && y % self.block[1] == 0
            && x + sx <= w.next_multiple_of(self.block[0])
            && y + sy <= h.next_multiple_of(self.block[1])
            && z + sz <= d
    }

    /// `region`'s rows, each in order, as a staging buffer filled from the image would hold them:
    /// its offset in a shadow's texels, its offset in the packed bytes, and its bytes. Empty for a
    /// region the layout does not hold.
    fn rows(&self, region: &ImageRegion) -> Vec<(usize, usize, usize)> {
        if !self.holds(region) {
            return Vec::new();
        }
        let (sub_row, sub_rows, _) = self.shape(region.level);
        let base = self.at(region.level, region.layer);
        let [x, y, z] = region.origin.map(u64::from);
        let [sx, sy, sz] = region.size.map(u64::from);
        let row = sx.div_ceil(u64::from(self.block[0])) * self.bytes;
        let rows = sy.div_ceil(u64::from(self.block[1]));
        let (bx, by) = (x / u64::from(self.block[0]), y / u64::from(self.block[1]));
        let mut out = Vec::with_capacity((rows * sz) as usize);
        for i in 0..sz {
            for j in 0..rows {
                let from =
                    base + (z + i) * sub_row * sub_rows + (by + j) * sub_row + bx * self.bytes;
                out.push((from as usize, ((i * rows + j) * row) as usize, row as usize));
            }
        }
        out
    }

    /// `region`'s texels out of `texels` into `packed`, which holds at least the region's packed
    /// bytes. Whether the layout holds the region.
    pub(crate) fn read(&self, region: &ImageRegion, texels: &[u8], packed: &mut [u8]) -> bool {
        let rows = self.rows(region);
        if rows.is_empty() {
            return false;
        }
        for (from, to, n) in rows {
            packed[to..to + n].copy_from_slice(&texels[from..from + n]);
        }
        true
    }

    /// `packed`, the region's packed bytes, over `region`'s texels in `texels`.
    pub(crate) fn write(&self, region: &ImageRegion, packed: &[u8], texels: &mut [u8]) {
        for (to, from, n) in self.rows(region) {
            texels[to..to + n].copy_from_slice(&packed[from..from + n]);
        }
    }
}

impl Image {
    /// The layout of a host shadow of its texels for a CPU copy in units of `unit`, where it takes
    /// one (the module docs); `None` for every other image and every copy in other units.
    pub(super) fn shadowed(&self, unit: &CopyUnit) -> Option<Layout> {
        if self.takes_host_copies() || self.takes_staging_copies() {
            return None;
        }
        // Keyed on the defect alone: the device copies images on the host, and this image's format
        // is the one thing that kept it off that path.
        let shared = self.device.shared();
        shared.host_copies.as_ref()?;
        let defects = &self.device.properties().driver_defects;
        if !defects.contains(&RuntimeDriverDefect::HostTransferImagesCopiedAtBind)
            || super::host_copy::reaches_format(self.plan().format, defects)
        {
            return None;
        }
        // An image of the runtime's own, in memory of its own, that no other resource, tile map or
        // process writes.
        match &self.owner {
            Owner::Runtime {
                shareable: false, ..
            } => {}
            _ => return None,
        }
        if self.plan().samples != SampleCountFlags::COUNT_1 {
            return None;
        }
        if image_aspects(self.plan().format) != ImageAspectFlags::COLOR
            || unit.aspect != ImageAspectFlags::COLOR
        {
            return None;
        }
        // A carrier holding another format's blocks or a 4:2:2 texture's texels, and a two-plane
        // texture, name their CPU copies' regions in units that are not their texture's.
        if crate::pvrtc::holds_blocks(self.plan())
            || crate::astc::decode_of(self.plan()).is_some()
            || crate::two_planes::holds_planes(self.plan().pixel_format)
        {
            return None;
        }
        // The copy's unit must be the image's own, so a region of it names the image's blocks.
        let own = copy_unit(
            self.plan().pixel_format,
            &self.plan().mapping,
            BlitOption::None,
        )
        .ok()?;
        if own != *unit {
            return None;
        }
        let layout = Layout::new(
            self.plan().extent,
            self.plan().levels,
            self.plan().layers,
            self.plan().image_type == ImageType::TYPE_3D,
            unit,
        );
        (layout.total() <= SHADOW_BYTES).then_some(layout)
    }

    /// Lay `region`'s texels out of its host shadow into `bytes` as `host` reads them, making the
    /// shadow where no read has yet (the module docs). Whether it did: not for an image that takes
    /// no shadow, one presenting a stand-in, one whose shadow is off, or where the making failed.
    pub(super) fn shadow_read(
        &self,
        region: &ImageRegion,
        unit: &CopyUnit,
        host: &HostLayout,
        bytes: &mut [u8],
    ) -> Result<bool, Error> {
        let Some(layout) = self.shadowed(unit) else {
            return Ok(false);
        };
        // A texture presenting a stand-in has no shadow: its bytes are detached meanwhile, and
        // the detach dropped it ([`Image::present_stand_in`]).
        let detached = lock(&self.detached).is_some();
        if detached {
            self.shadow_dropped();
            return Ok(false);
        }
        let shared = self.device.shared();
        let raw = self.raw.get();
        // Read before the shadow is taken and before it is read from, which is the conservative
        // order either way: a commit that lands after this read leaves the shadow holding the
        // count from before it, so the next read of it drops the shadow rather than keeping one
        // the commit's work may have written over. The other order would store a count the
        // commit had already reached.
        let gpu_work = shared.gpu_work();
        let mut held = lock(&self.host_shadow);
        match &*held {
            HostShadow::Off => return Ok(false),
            HostShadow::Made(shadow) if shadow.raw == raw && shadow.gpu_work == gpu_work => {}
            HostShadow::Made(_) => {
                *held = HostShadow::Unmade;
                shared.counts.texel_shadow_dropped();
            }
            HostShadow::Unmade => {}
        }
        if matches!(&*held, HostShadow::Unmade) {
            match self.take_shadow(&layout, raw, gpu_work) {
                Ok(shadow) => {
                    *held = HostShadow::Made(shadow);
                    shared.counts.texel_shadow_made(layout.total());
                }
                // The image's texels could not be read whole: the call falls back to the
                // submission path, and nothing asks for a shadow of this image again.
                Err(_) => {
                    *held = HostShadow::Off;
                    return Ok(false);
                }
            }
        }
        let HostShadow::Made(shadow) = &*held else {
            return Ok(false);
        };
        let len = host.packed_len() as usize;
        let read = match host.is_packed() {
            true => layout.read(region, &shadow.texels, &mut bytes[..len]),
            false => {
                let mut packed = vec![0; len];
                let read = layout.read(region, &shadow.texels, &mut packed);
                if read {
                    host.unpack(&packed, bytes);
                }
                read
            }
        };
        if read {
            shared.counts.texel_shadow_read();
        }
        Ok(read)
    }

    /// The image's texels whole, read out of `raw` by one internal submission (the one a CPU read
    /// of it waits for either way), as of `gpu_work`.
    fn take_shadow(
        &self,
        layout: &Layout,
        raw: vk::Image,
        gpu_work: u64,
    ) -> Result<Shadowed, Error> {
        let len = layout.total();
        let mut staging = Staging::new(&self.device, len)?;
        let buffer = staging.raw;
        let copies: Vec<vk::BufferImageCopy> = (layout.regions().iter())
            .map(|(region, at)| vk::BufferImageCopy {
                buffer_offset: *at,
                ..buffer_image_copy(region)
            })
            .collect();
        self.submit_copy(|commands, command_buffer| {
            transfer_barrier(commands, command_buffer, true);
            // SAFETY: the internal command buffer is recording; the staging buffer and the image
            // are live, the image is in the `GENERAL` layout, and the copies cover its
            // subresources once each inside a buffer of their bytes.
            unsafe {
                commands.cmd_copy_image_to_buffer(
                    command_buffer,
                    raw,
                    vk::ImageLayout::GENERAL,
                    buffer,
                    &copies,
                );
            }
            transfer_barrier(commands, command_buffer, false);
        })?;
        Ok(Shadowed {
            raw,
            gpu_work,
            texels: staging.bytes().to_vec(),
        })
    }

    /// Write `packed`, `region`'s packed bytes, into its host shadow where it holds the image's
    /// texels: a CPU write reaches the image and the shadow with the same bytes, so the shadow
    /// holds them still. A stale shadow is dropped instead.
    pub(super) fn shadow_written(&self, region: &ImageRegion, unit: &CopyUnit, packed: &[u8]) {
        let Some(layout) = self.shadowed(unit) else {
            return;
        };
        let shared = self.device.shared();
        let (raw, gpu_work) = (self.raw.get(), shared.gpu_work());
        let mut held = lock(&self.host_shadow);
        let HostShadow::Made(shadow) = &mut *held else {
            return;
        };
        if shadow.raw != raw || shadow.gpu_work != gpu_work {
            *held = HostShadow::Unmade;
            shared.counts.texel_shadow_dropped();
            return;
        }
        // The count stays as it was: a CPU write never says a shadow holds texels a commit since
        // may have written, and raising it here would hide exactly that.
        layout.write(region, packed, &mut shadow.texels);
    }

    /// Drop its host shadow where it has one (a detach, the device's count of the program's GPU
    /// work moving on).
    pub(super) fn shadow_dropped(&self) {
        let mut held = lock(&self.host_shadow);
        if matches!(&*held, HostShadow::Made(_)) {
            *held = HostShadow::Unmade;
            self.device.shared().counts.texel_shadow_dropped();
        }
    }

    /// Never shadow its texels again: the program took the image's `VkImage` and may write it
    /// whenever it likes (`interop.rs`).
    pub(super) fn shadow_off(&self) {
        let mut held = lock(&self.host_shadow);
        if matches!(&*held, HostShadow::Made(_)) {
            self.device.shared().counts.texel_shadow_dropped();
        }
        *held = HostShadow::Off;
    }
}
