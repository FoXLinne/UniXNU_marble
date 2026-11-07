//! A heap texture's `allocatedSize` (`Texture::allocated_size`): the size the heap's size query
//! answers for its descriptor, which placed it, in an automatic heap and a placement heap alike,
//! a view's its texture's, a texture buffer's its row; none outside a heap. Metal answers so on an
//! M3 Max under macOS 27.0.1, plain and validated.
//!
//! A device test: compiled only under the validation crate's GPU safety cfgs (`m2v_gpu`,
//! `m2v_cpu_icd`), on the kind of device the cfg selects. Nothing runs on the GPU.

use crate::{
    Device, DeviceKind, DeviceOptions, HeapDescriptor, HeapType, ResourceOptions, TextureDescriptor,
};
use metal2vulkan::metal::{PixelFormat, TextureType, TextureUsage};

/// The device the cfg selects: a CPU device under `m2v_cpu_icd`, else a GPU.
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

/// The descriptors placed: plain, written, Apple's private 0x8 alone, a render target of an odd
/// extent, a mipmapped one, a small one, and a texture buffer.
fn descriptors() -> Vec<TextureDescriptor> {
    let of = |format, width, height, mipmapped, usage: u32| {
        let mut d = TextureDescriptor::texture_2d(format, width, height, mipmapped);
        d.usage = TextureUsage::try_from(usage).unwrap();
        d.resource_options = ResourceOptions::private();
        d
    };
    let mut texture_buffer = of(PixelFormat::RGBA8Unorm, 1000, 1, false, 0x1);
    texture_buffer.texture_type = TextureType::TextureBuffer;
    vec![
        of(PixelFormat::RGBA8Unorm, 64, 64, false, 0x1),
        of(PixelFormat::RGBA8Unorm, 64, 64, false, 0x2),
        of(PixelFormat::RGBA8Unorm, 64, 64, false, 0x8),
        of(PixelFormat::RGBA8Unorm, 100, 37, false, 0x4),
        of(PixelFormat::RGBA8Unorm, 256, 256, true, 0x1),
        of(PixelFormat::R32Float, 4, 4, false, 0x1),
        texture_buffer,
    ]
}

#[test]
fn a_heap_texture_answers_the_size_that_placed_it() {
    let device = device();
    for heap_type in [HeapType::Automatic, HeapType::Placement] {
        let hd = HeapDescriptor::new(heap_type, 16 << 20, ResourceOptions::private());
        let heap = device.new_heap(&hd).unwrap();
        let mut offset: u64 = 0;
        for d in descriptors() {
            let answer = device.heap_texture_size_and_align(&d).unwrap();
            let texture = match heap_type {
                HeapType::Automatic => heap.new_texture(&d),
                _ => {
                    offset = offset.next_multiple_of(answer.align);
                    let made = heap.new_texture_at_offset(&d, offset);
                    offset += answer.size;
                    made
                }
            };
            let texture = texture.unwrap();
            let what = format!("{heap_type:?} {:?} {:?}", d.texture_type, d.usage);
            assert_eq!(texture.allocated_size(), Some(answer.size), "{what}");
            let view = texture.new_texture_view(d.pixel_format).unwrap();
            assert_eq!(view.allocated_size(), Some(answer.size), "{what}'s view");
            let outside = device.new_texture(&d).unwrap();
            assert_eq!(outside.allocated_size(), None, "{what} outside a heap");
        }
    }
}
