//! `generateMipmapsForTexture:`'s blit chain on the device: each level the device's linear blit
//! (`vkCmdBlitImage`, `VK_FILTER_LINEAR`) of the one above, over the view's layers, as
//! [`mipmap_plan`](super::mipmap_plan) plans it for a format the device's blits filter. It is the
//! device's own filter, not Apple's, announced as `device-filtered-mipmaps`.

use super::mipmap_plan::MipBlit;
use ash::vk;

/// Record blit `blit` of `image` (of `aspects`), linearly filtered.
pub(super) fn record_blit(
    device: &ash::Device,
    command_buffer: vk::CommandBuffer,
    image: vk::Image,
    aspects: vk::ImageAspectFlags,
    blit: &MipBlit,
) {
    let layers = |level| vk::ImageSubresourceLayers {
        aspect_mask: aspects,
        mip_level: level,
        base_array_layer: blit.base_layer,
        layer_count: blit.layers,
    };
    let corner = |[x, y, z]: [u32; 3]| vk::Offset3D {
        x: x as i32,
        y: y as i32,
        z: z as i32,
    };
    let region = vk::ImageBlit {
        src_subresource: layers(blit.src_level),
        src_offsets: [vk::Offset3D::default(), corner(blit.src_extent)],
        dst_subresource: layers(blit.dst_level),
        dst_offsets: [vk::Offset3D::default(), corner(blit.dst_extent)],
    };
    // SAFETY: the command buffer is recording; the image is live, in the `GENERAL` layout, with
    // transfer usages and its format's blit and linear-filter features (`plan_mipmaps` checked
    // them), and the two levels are distinct.
    unsafe {
        device.cmd_blit_image(
            command_buffer,
            image,
            vk::ImageLayout::GENERAL,
            image,
            vk::ImageLayout::GENERAL,
            &[region],
            vk::Filter::LINEAR,
        );
    }
}
