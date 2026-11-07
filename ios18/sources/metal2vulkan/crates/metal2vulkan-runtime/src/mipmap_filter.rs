//! `generate_mipmaps` of a format the device's blits cannot filter (`mipmap_plan.rs`'s
//! [`MipmapPlan::Filter`](super::mipmap_plan::MipmapPlan::Filter): an extended-range format, an
//! 8-bit sRGB texture held as its UNORM twin, a format without blits or linear filtering), by the
//! compute filter of `bind::mipmap_filter`: each level a bilinear (trilinear in 3D) filter of the
//! one above at the scaled texel centres, in `float`, in a 2D, 2D array, cube, cube array or 3D
//! texture whatever its levels' sizes. It is not Apple's datapath, and it is announced as the
//! blit chain is (`device-filtered-mipmaps`).
//!
//! **The command.** One blit command, between barriers from the commands before it and to the
//! ones after it ([`BlitCommandEncoder::command`]), over a scratch block the device keeps between
//! calls (`mipmap_scratch.rs`; `bind::MipmapFilterChain` lays it out): the first level of every
//! layer copied in, unless it is read in place (below), every later level zeroed where its
//! texels are narrower than a word (each dispatch ORs them in; a wider texel's writes every
//! word), each level's tap words uploaded, one dispatch per later level of its kernel
//! (`owned_kernel.rs`: one per `MipmapFilterKernel`), each reading the level the one before
//! wrote, and every later level copied back into the texture.
//!
//! **The first level read in place**: the first dispatch fetches its taps from a sampled view of
//! the first level (a 2D array of every layer, 3D for a 3D texture) and recovers each texel's bytes
//! (`bind::MipmapFilterChain::plan`'s `fetch`), where the image has sampled usage, the view's
//! format (the image's own, or the UNORM twin of an 8-bit sRGB one) is one the image views as and
//! samples, and the fetch is exact: the extended-range formats' integer carriers on every device,
//! the normalized and 16-bit float formats on a driver measured to return values whose recovery
//! reads every code back (`DriverFact::SampledFetchRecoversCodes`). Vulkan states no precision for
//! those conversions, so on any other driver the level is copied in. The
//! command buffer holds the block's lease and the dispatches' descriptor sets until it completes.
//! Where the device's limits do not fit the chain (a level past `maxStorageBufferRange`, the
//! scratch past `max_buffer_length`, a dispatch past `maxComputeWorkGroupCount`) or the scratch's
//! memory runs out, nothing is recorded and the call is refused.

use super::binding_view::{views_as, VkView};
use super::blit::BlitCommandEncoder;
use super::error::Error;
use super::image_plan::ImageFacts;
use super::image_plan::ImagePlan;
use super::owned_kernel::OwnedSet;
use super::owned_kernel::{OwnedModule, OwnedSets};
use super::sparse::emulated::Written;
use super::texture::{texture_aspects, Texture};
use super::tracker::{Access, Barrier};
use ash::vk;
use metal2vulkan::bind::MipmapFilterChain;
use metal2vulkan::device::DriverFact;
use metal2vulkan::metal::TextureType;
use metal2vulkan::vk::{
    ComponentMapping, Format, FormatFeatureFlags2, ImageAspectFlags, ImageUsageFlags, ImageViewType,
};

/// The most bytes one `vkCmdUpdateBuffer` writes.
const UPDATE_BYTES: usize = 65536;

impl BlitCommandEncoder {
    /// Record `texture`'s mipmaps by the compute filter (the module docs): `false`, recording
    /// nothing, where the device's limits do not fit its chain. The caller planned it
    /// (`mipmap_plan::plan_mipmaps`: Metal's rules, the format, the image's own format and its
    /// transfers).
    pub(super) fn filter_mipmaps(
        &mut self,
        texture: &Texture,
        facts: &ImageFacts<'_>,
        read: Access,
        write: Access,
    ) -> Result<bool, Error> {
        let record = texture.record();
        let plan = texture.image_plan()?;
        let volume = record.texture_type == TextureType::Type3D;
        let mut extent = record.extent();
        if !volume {
            extent[2] = 1;
        }
        let device = self.core().device().clone();
        let fetch = fetch_view(plan, facts, &device);
        let limits = &device.properties().description.limits;
        let Some(chain) = MipmapFilterChain::plan(
            record.pixel_format,
            extent,
            record.layer_count,
            record.level_count,
            volume,
            limits.min_storage_buffer_offset_alignment,
            limits.max_storage_buffer_range,
            [
                limits.max_compute_work_group_count[0],
                limits.max_compute_work_group_count[1],
            ],
            fetch,
        ) else {
            return Ok(false);
        };
        if chain.bytes > device.properties().max_buffer_length {
            return Ok(false);
        }
        // Each later level's kernel (levels of one kernel share it).
        let mut kernels = Vec::with_capacity(chain.levels.len() - 1);
        for level in &chain.levels[1..] {
            let Some(key) = level.kernel else {
                return Err(super::error::internal(
                    "a mipmap filter level without its kernel",
                ));
            };
            kernels.push(device.owned_kernel(OwnedModule::MipmapFilter(key))?);
        }
        // The device's scratch block, kept between calls (`mipmap_scratch.rs`).
        let Some(scratch) = device.mipmap_scratch(chain.bytes)? else {
            return Ok(false);
        };
        let (raw, at) = (scratch.raw(), 0);
        let words = |bytes: u64| bytes.next_multiple_of(4);
        let (base_level, base_layer, layers) =
            (record.base_level, record.base_layer, record.layer_count);
        // The first level's sampled view, where the chain reads it in place.
        let first_view = match chain.fetch {
            Some(format) => Some(texture.image_view(&VkView {
                view_type: if volume {
                    ImageViewType::TYPE_3D
                } else {
                    ImageViewType::TYPE_2D_ARRAY
                },
                format,
                components: ComponentMapping::IDENTITY,
                aspect: ImageAspectFlags::COLOR,
                base_level,
                level_count: 1,
                base_layer,
                layer_count: layers,
                usage: ImageUsageFlags::SAMPLED,
            })?),
            None => None,
        };
        let mut ranges = Vec::with_capacity(chain.levels.len() - 1);
        let mut uploads = Vec::new();
        let mut zeroed = Vec::with_capacity(chain.levels.len() - 1);
        let mut groups = Vec::with_capacity(chain.levels.len() - 1);
        for pair in chain.levels.windows(2) {
            let (above, level) = (&pair[0], &pair[1]);
            let Some((offset, params)) = &level.params else {
                return Err(super::error::internal(
                    "a mipmap filter level without its words",
                ));
            };
            let bytes: Vec<u8> = params.iter().flat_map(|w| w.to_le_bytes()).collect();
            let params_range = (raw, at + offset, bytes.len() as u64);
            let destination = (raw, at + level.offset, words(level.bytes));
            let rest = [destination, params_range];
            // The first dispatch's source: the first level's view where it is read in place.
            ranges.push(match first_view.filter(|_| ranges.is_empty()) {
                Some(view) => OwnedSet {
                    images: vec![view],
                    storage_images: Vec::new(),
                    buffers: rest.to_vec(),
                },
                None => OwnedSet {
                    images: Vec::new(),
                    storage_images: Vec::new(),
                    buffers: [(raw, at + above.offset, words(above.bytes))]
                        .into_iter()
                        .chain(rest)
                        .collect(),
                },
            });
            for (k, chunk) in bytes.chunks(UPDATE_BYTES).enumerate() {
                uploads.push((at + offset + (k * UPDATE_BYTES) as u64, chunk.to_vec()));
            }
            if chain.zeroed {
                zeroed.push((at + level.offset, words(level.bytes)));
            }
            groups.push(level.groups);
        }
        let sets = OwnedSets::write_each(&device, &kernels, &ranges)?;
        let first = base_level..base_level + 1;
        let later = base_level + 1..base_level + record.level_count;
        let all_layers = base_layer..base_layer + layers;
        // The first level is read by the copy in, or by the first dispatch's fetches.
        let first_read = match first_view {
            Some(_) => Access::new(
                vk::PipelineStageFlags2::COMPUTE_SHADER,
                vk::AccessFlags2::SHADER_SAMPLED_READ,
            ),
            None => read,
        };
        let uses = [
            texture.image_use(first, all_layers.clone(), first_read, false)?,
            texture.image_use(later, all_layers, write, true)?,
        ];
        let (image, aspects) = (texture.raw_image()?, texture_aspects(plan));
        let region =
            |level: u32, offset: u64, [width, height, depth]: [u32; 3]| vk::BufferImageCopy {
                buffer_offset: at + offset,
                buffer_row_length: 0,
                buffer_image_height: 0,
                image_subresource: vk::ImageSubresourceLayers {
                    aspect_mask: aspects,
                    mip_level: base_level + level,
                    base_array_layer: base_layer,
                    layer_count: layers,
                },
                image_offset: vk::Offset3D::default(),
                image_extent: vk::Extent3D {
                    width,
                    height,
                    depth,
                },
            };
        let copy_in = first_view
            .is_none()
            .then(|| region(0, chain.levels[0].offset, chain.levels[0].extent));
        let copies_back: Vec<vk::BufferImageCopy> = (1u32..)
            .zip(&chain.levels[1..])
            .map(|(level, l)| region(level, l.offset, l.extent))
            .collect();
        let descriptor_sets = sets.sets.clone();
        let owner = self.core().device().clone();
        self.command(&uses, |device, command_buffer| {
            // SAFETY: the command buffer is recording, outside a render pass; the scratch, the
            // sets and the texture (whose image keeps the first level's view) are retained until
            // it completes; the image is in the `GENERAL` layout with transfer usages and
            // features (checked above), and sampled usage and features where its view is bound
            // (`fetch_view`); every copy, fill, upload (each at most 65536 bytes, a multiple of
            // 4 at a multiple of 4) and binding lies inside the scratch
            // (`MipmapFilterChain::plan`), and each step reads what the one before it wrote, past
            // a barrier.
            unsafe {
                for (offset, bytes) in &uploads {
                    device.cmd_update_buffer(command_buffer, raw, *offset, bytes);
                }
                for &(offset, size) in &zeroed {
                    crate::word_fill::record(&owner, command_buffer, raw, offset..offset + size, 0);
                }
                if let Some(copy_in) = copy_in {
                    device.cmd_copy_image_to_buffer(
                        command_buffer,
                        image,
                        vk::ImageLayout::GENERAL,
                        raw,
                        &[copy_in],
                    );
                }
                Barrier::everything().record(device, command_buffer);
                let mut bound = vk::Pipeline::null();
                for ((set, [x, y]), kernel) in descriptor_sets.iter().zip(&groups).zip(&kernels) {
                    if kernel.pipeline != bound {
                        device.cmd_bind_pipeline(
                            command_buffer,
                            vk::PipelineBindPoint::COMPUTE,
                            kernel.pipeline,
                        );
                        bound = kernel.pipeline;
                    }
                    device.cmd_bind_descriptor_sets(
                        command_buffer,
                        vk::PipelineBindPoint::COMPUTE,
                        kernel.layout,
                        0,
                        std::slice::from_ref(set),
                        &[],
                    );
                    device.cmd_dispatch(command_buffer, *x, *y, 1);
                    Barrier::everything().record(device, command_buffer);
                }
                device.cmd_copy_buffer_to_image(
                    command_buffer,
                    raw,
                    image,
                    vk::ImageLayout::GENERAL,
                    &copies_back,
                );
            }
        })?;
        if first_view.is_some() {
            device.shared().counts.mipmap_first_level_fetched();
        }
        self.core().retain(scratch);
        self.core().retain(sets);
        self.core().retain(texture.clone());
        let written = (1u32..)
            .zip(&chain.levels[1..])
            .map(|(level, l)| {
                let written = Written {
                    level: base_level + level,
                    layers: base_layer..base_layer + layers,
                    origin: [0; 3],
                    size: l.extent,
                };
                (aspects, written)
            })
            .collect();
        self.scrub(texture, written)?;
        device.shared().counts.mipmaps_filtered();
        Ok(true)
    }
}

/// The format of the sampled view through which the first level of a texture whose image is
/// `plan` is read in place, where it may be (the module docs): the image's format or the UNORM
/// twin of its 8-bit sRGB one, which the image views as and samples with sampled usage, and
/// whose fetch is exact on `device`. `None` where the level is copied in. The chain takes the
/// view only for a format whose fetch recovers its texels (`MipmapFilterChain::plan`).
fn fetch_view(plan: &ImagePlan, facts: &ImageFacts<'_>, device: &super::Device) -> Option<Format> {
    let view = MipmapFilterChain::fetch_format(plan.format)?;
    let sampled = plan.usage.contains(ImageUsageFlags::SAMPLED)
        && views_as(plan, view, false)
        && facts
            .optimal(view)
            .contains(FormatFeatureFlags2::SAMPLED_IMAGE);
    let exact = !MipmapFilterChain::fetch_converts(view)
        || device
            .properties()
            .driver_facts
            .contains(&DriverFact::SampledFetchRecoversCodes);
    (sampled && exact).then_some(view)
}
