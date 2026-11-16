//! A texture of Apple's private usage 0x8 alone, whose Metal texture answers no usage (`usage` 0):
//! plain Metal does everything with it (an M3 Max under macOS 27.0.1, a 4x4 `RGBA8Unorm` texture,
//! `Shared` and `Private`): the CPU's `replaceRegion:` and `getBytes:`, a render pass's clear of it
//! as a color attachment, blits to and from buffers, `generateMipmaps:`, and views of it as
//! `RGBA8Unorm`, `RGBA8Unorm_sRGB`, `BGRA8Unorm` and `R32Uint` that read its texels. Each is
//! checked here against the bytes Metal gave, the views by their bytes. (Its shader reads, samples
//! and writes are the lane's, `buffer_texture_order`.)
//!
//! A device test: compiled only under the validation crate's GPU safety cfgs (`m2v_gpu`,
//! `m2v_cpu_icd`), on the kind of device the cfg selects.

use super::blit_holds::WAIT;
use crate::{
    BlitOption, ClearColor, CommandBufferStatus, Device, DeviceKind, DeviceOptions, LoadAction,
    Origin, Region, RenderPassDescriptor, ResourceOptions, Size, StoreAction, Texture,
    TextureDescriptor,
};
use metal2vulkan::metal::{PixelFormat, TextureUsage};

/// The device the cfg selects.
fn device() -> Device {
    let options = DeviceOptions {
        kinds: match cfg!(m2v_cpu_icd) {
            true => vec![DeviceKind::Cpu],
            false => vec![
                DeviceKind::Discrete,
                DeviceKind::Integrated,
                DeviceKind::Virtual,
            ],
        },
        ..DeviceOptions::default()
    };
    Device::system_default_with(&options).unwrap()
}

/// A 4x4 `RGBA8Unorm` texture of usage 0x8 alone, of `options`, `mipmapped` or not.
fn dropped(device: &Device, options: ResourceOptions, mipmapped: bool) -> Texture {
    let mut d = TextureDescriptor::texture_2d(PixelFormat::RGBA8Unorm, 4, 4, mipmapped);
    d.usage = TextureUsage::try_from(0x8u32).unwrap();
    d.resource_options = options;
    device.new_texture(&d).unwrap()
}

/// Texel `i` is `(i, 2i, 3i, 255)`, as the Metal probe filled it.
fn pattern() -> Vec<u8> {
    (0..16u8).flat_map(|i| [i, 2 * i, 3 * i, 255]).collect()
}

fn bytes_of(texture: &Texture) -> Vec<u8> {
    let mut bytes = vec![0; 64];
    let region = Region::new_2d(0, 0, 4, 4);
    texture.get_bytes(&mut bytes, 16, 0, region, 0, 0).unwrap();
    bytes
}

#[test]
fn a_texture_of_usage_0x8_alone_does_what_metal_s_does() {
    let device = device();
    let queue = device.new_command_queue().unwrap();
    let texture = dropped(&device, ResourceOptions::shared(), false);
    assert_eq!(texture.usage(), TextureUsage::empty());
    let region = Region::new_2d(0, 0, 4, 4);
    texture
        .replace_region(region, 0, 0, &pattern(), 16, 0)
        .unwrap();
    assert_eq!(bytes_of(&texture), pattern(), "the CPU's round trip");

    // Views of each format, made, their bytes the texture's (a view's `getBytes:` copies the
    // texel bytes as they are, whatever its format names them).
    for format in [
        PixelFormat::RGBA8Unorm,
        PixelFormat::RGBA8UnormSRGB,
        PixelFormat::BGRA8Unorm,
        PixelFormat::R32Uint,
    ] {
        let view = texture.new_texture_view(format).unwrap();
        assert_eq!(bytes_of(&view), pattern(), "a {format:?} view");
    }

    // Blits out to a buffer and back in, through a private texture too.
    let out = device.new_buffer(64, ResourceOptions::shared()).unwrap();
    let private = dropped(&device, ResourceOptions::private(), false);
    let command_buffer = queue.command_buffer().unwrap();
    let mut blit = command_buffer.blit_command_encoder().unwrap();
    let whole = Size::new(4, 4, 1);
    let at = Origin::new(0, 0, 0);
    blit.copy_from_texture_to_buffer(&texture, 0, 0, at, whole, &out, 0, 16, 64, BlitOption::None)
        .unwrap();
    blit.copy_from_buffer_to_texture(&out, 0, 16, 64, whole, &private, 0, 0, at, BlitOption::None)
        .unwrap();
    blit.end_encoding().unwrap();
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    assert_eq!(command_buffer.status(), CommandBufferStatus::Completed);
    // SAFETY: the command buffer completed; nothing else writes the buffer.
    let copied = unsafe { std::slice::from_raw_parts(out.contents().unwrap().as_ptr(), 64) };
    assert_eq!(copied, pattern(), "the blit out");

    // A render pass's clear of it as a color attachment.
    let mut pass = RenderPassDescriptor::new();
    let color = &mut pass.color_attachments[0];
    color.texture = Some(texture.clone());
    color.load_action = LoadAction::Clear;
    color.store_action = StoreAction::Store;
    color.clear_color = ClearColor::new(1.0, 0.0, 1.0, 0.0);
    let command_buffer = queue.command_buffer().unwrap();
    let encoder = command_buffer.render_command_encoder(&pass).unwrap();
    encoder.end_encoding().unwrap();
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    assert_eq!(command_buffer.status(), CommandBufferStatus::Completed);
    assert!(
        bytes_of(&texture).chunks(4).all(|t| t == [255, 0, 255, 0]),
        "the clear stored: {:?}",
        bytes_of(&texture)
    );

    // `generateMipmaps:` of a mipmapped one: its 1x1 level is the average of its texels.
    let mipmapped = dropped(&device, ResourceOptions::shared(), true);
    mipmapped
        .replace_region(region, 0, 0, &[200; 64], 16, 0)
        .unwrap();
    let command_buffer = queue.command_buffer().unwrap();
    let mut blit = command_buffer.blit_command_encoder().unwrap();
    blit.generate_mipmaps(&mipmapped).unwrap();
    blit.end_encoding().unwrap();
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    assert_eq!(command_buffer.status(), CommandBufferStatus::Completed);
    let mut last = [0u8; 4];
    mipmapped
        .get_bytes(&mut last, 4, 0, Region::new_2d(0, 0, 1, 1), 2, 0)
        .unwrap();
    assert_eq!(last, [200; 4], "the last level");
}
