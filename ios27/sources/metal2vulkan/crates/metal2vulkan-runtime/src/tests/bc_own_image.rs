//! A BC texture is its own BC image on every device, which its reads, samples and gathers read
//! through the device's decode: no decoded twin beside it, and no decode pass or CPU decode after
//! a write into it, whether the texture is `Shared` and written by `replaceRegion`, `Private` and
//! written by a blit, or a heap's. Its bytes read back as written. A texture of a format whose
//! decode the device's driver is not measured to make as Metal does (BC1 to BC3
//! `ColorBlockDecodeIsMetals`, BC4 and BC5 `ChannelBlockDecodeIsMetals`, BC6H
//! `Bc6hBlockDecodeIsMetals`, BC7 `Bc7BlockDecodeIsMetals`) announces `compressed-texel-decode`
//! naming its format; one whose decode it is measured to make announces nothing.
//!
//! A device test: compiled only under the validation crate's GPU safety cfgs (`m2v_gpu`,
//! `m2v_cpu_icd`), on the kind of device the cfg selects. The validation crate's
//! `runtime_lane::compressed_decode` reads the texels through a kernel against Metal's decode.

use super::blit_holds::{Scratch, WAIT};
use crate::{
    BlitOption, Degradation, DegradeNotice, Device, DeviceKind, DeviceOptions, Error,
    HeapDescriptor, HeapType, MessageSink, Origin, Region, ResourceOptions, ShaderCacheOptions,
    Size, Texture, TextureDescriptor,
};
use metal2vulkan::device::DriverFact;
use metal2vulkan::metal::{PixelFormat, TextureUsage};
use std::path::Path;
use std::sync::{Arc, Mutex};

/// The textures' side, in texels: 4 x 4 blocks.
const SIDE: u64 = 16;

/// Every BC format, with the driver fact under which the device decodes it as Metal does.
const FORMATS: [(PixelFormat, DriverFact); 14] = [
    (PixelFormat::BC1RGBA, DriverFact::ColorBlockDecodeIsMetals),
    (
        PixelFormat::BC1RGBASRGB,
        DriverFact::ColorBlockDecodeIsMetals,
    ),
    (PixelFormat::BC2RGBA, DriverFact::ColorBlockDecodeIsMetals),
    (
        PixelFormat::BC2RGBASRGB,
        DriverFact::ColorBlockDecodeIsMetals,
    ),
    (PixelFormat::BC3RGBA, DriverFact::ColorBlockDecodeIsMetals),
    (
        PixelFormat::BC3RGBASRGB,
        DriverFact::ColorBlockDecodeIsMetals,
    ),
    (
        PixelFormat::BC4RUnorm,
        DriverFact::ChannelBlockDecodeIsMetals,
    ),
    (
        PixelFormat::BC4RSnorm,
        DriverFact::ChannelBlockDecodeIsMetals,
    ),
    (
        PixelFormat::BC5RGUnorm,
        DriverFact::ChannelBlockDecodeIsMetals,
    ),
    (
        PixelFormat::BC5RGSnorm,
        DriverFact::ChannelBlockDecodeIsMetals,
    ),
    (
        PixelFormat::BC6HRGBFloat,
        DriverFact::Bc6hBlockDecodeIsMetals,
    ),
    (
        PixelFormat::BC6HRGBUfloat,
        DriverFact::Bc6hBlockDecodeIsMetals,
    ),
    (
        PixelFormat::BC7RGBAUnorm,
        DriverFact::Bc7BlockDecodeIsMetals,
    ),
    (
        PixelFormat::BC7RGBAUnormSRGB,
        DriverFact::Bc7BlockDecodeIsMetals,
    ),
];

/// What the device's sink heard of `compressed-texel-decode`.
type Heard = Arc<Mutex<Vec<DegradeNotice>>>;

/// The device the cfg selects, its shader cache in `cache`, announcing `compressed-texel-decode`
/// at every occurrence into `heard`.
fn device(cache: &Path, heard: &Heard) -> Device {
    let heard = Arc::clone(heard);
    let options = DeviceOptions {
        kinds: match cfg!(m2v_cpu_icd) {
            true => vec![DeviceKind::Cpu],
            false => vec![
                DeviceKind::Discrete,
                DeviceKind::Integrated,
                DeviceKind::Virtual,
            ],
        },
        shader_cache: ShaderCacheOptions {
            directory: Some(cache.to_owned()),
            ..ShaderCacheOptions::default()
        },
        messages: Some(MessageSink::new(move |message| {
            if let Some(notice) = &message.degradation {
                if notice.degradation == Degradation::CompressedTexelDecode {
                    heard.lock().unwrap().push(notice.clone());
                }
            }
        })),
        announce_each: [Degradation::CompressedTexelDecode].into_iter().collect(),
        ..DeviceOptions::default()
    };
    Device::system_default_with(&options).unwrap()
}

/// `count` blocks of `bytes` each that name their place and `seed`.
fn blocks(seed: u8, count: u64, bytes: u64) -> Vec<u8> {
    (0..count * bytes)
        .map(|i| (i as u8).wrapping_mul(37) ^ (i >> 8) as u8 ^ seed)
        .collect()
}

/// A `SIDE` x `SIDE` texture of `pf` of `options`, sampled.
fn descriptor(pf: PixelFormat, options: ResourceOptions) -> TextureDescriptor {
    let mut d = TextureDescriptor::texture_2d(pf, SIDE, SIDE, false);
    d.usage = TextureUsage::SHADER_READ;
    d.resource_options = options;
    d
}

/// The bytes of `texture`'s whole level 0 (`row` bytes a row of blocks), as `getBytes` reads
/// them: a blit into a `Shared` texture first, a `Private` texture's.
fn bytes_of(device: &Device, texture: &Texture, row: u64) -> Vec<u8> {
    let length = row * SIDE / 4;
    let shared = match texture.storage_mode() {
        crate::StorageMode::Private => {
            let pf = texture.pixel_format();
            let copy = device
                .new_texture(&descriptor(pf, ResourceOptions::shared()))
                .unwrap();
            blit(device, |blit| {
                let at = Origin::new(0, 0, 0);
                let side = Size::new(SIDE, SIDE, 1);
                blit.copy_from_texture_to_texture(texture, 0, 0, at, side, &copy, 0, 0, at)
            });
            copy
        }
        _ => texture.clone(),
    };
    let mut read = vec![0u8; length as usize];
    let whole = Region::new_2d(0, 0, SIDE, SIDE);
    shared.get_bytes(&mut read, row, 0, whole, 0, 0).unwrap();
    read
}

/// Run one blit encoder `encode` fills, and wait for it.
fn blit(device: &Device, encode: impl FnOnce(&mut crate::BlitCommandEncoder) -> Result<(), Error>) {
    let queue = device.new_command_queue().unwrap();
    let command_buffer = queue.command_buffer().unwrap();
    let mut blit = command_buffer.blit_command_encoder().unwrap();
    encode(&mut blit).unwrap();
    blit.end_encoding().unwrap();
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    assert_eq!(command_buffer.error(), None);
}

#[test]
fn a_bc_texture_is_its_own_image_and_announces_an_unmeasured_decode() {
    let scratch = Scratch::new("bc-own-image");
    let heard = Heard::default();
    let device = device(&scratch.0, &heard);
    let name = device.name().to_owned();
    let facts = &device.properties().driver_facts;
    let before = device.work_counts();
    let whole = Region::new_2d(0, 0, SIDE, SIDE);
    let mut wrong = Vec::new();
    for (f, (pf, fact)) in FORMATS.into_iter().enumerate() {
        let block = match pf {
            PixelFormat::BC1RGBA
            | PixelFormat::BC1RGBASRGB
            | PixelFormat::BC4RUnorm
            | PixelFormat::BC4RSnorm => 8,
            _ => 16,
        };
        let row = block * SIDE / 4;
        let written = blocks(0x40 + f as u8, (SIDE / 4) * (SIDE / 4), block);
        let made = heard.lock().unwrap().len();
        // `Shared`, written by `replaceRegion`.
        let shared = device
            .new_texture(&descriptor(pf, ResourceOptions::shared()))
            .unwrap();
        shared
            .replace_region(whole, 0, 0, &written, row, 0)
            .unwrap();
        // `Private`, written by a blit from a buffer.
        let private = device
            .new_texture(&descriptor(pf, ResourceOptions::private()))
            .unwrap();
        let source = device
            .new_buffer_with_bytes(&written, ResourceOptions::shared())
            .unwrap();
        blit(&device, |blit| {
            blit.copy_from_buffer_to_texture(
                &source,
                0,
                row,
                0,
                Size::new(SIDE, SIDE, 1),
                &private,
                0,
                0,
                Origin::new(0, 0, 0),
                BlitOption::None,
            )
        });
        // A heap's, written by a copy from the `Private` one.
        let d = descriptor(pf, ResourceOptions::private());
        let answer = device.heap_texture_size_and_align(&d).unwrap();
        let hd = HeapDescriptor::new(
            HeapType::Automatic,
            answer.size + answer.align,
            ResourceOptions::private(),
        );
        let heap = device.new_heap(&hd).unwrap();
        let placed = heap.new_texture(&d).unwrap();
        blit(&device, |blit| {
            let at = Origin::new(0, 0, 0);
            let side = Size::new(SIDE, SIDE, 1);
            blit.copy_from_texture_to_texture(&private, 0, 0, at, side, &placed, 0, 0, at)
        });
        // Each texture announces the device's decode where the driver lacks the fact.
        let notices: Vec<_> = heard.lock().unwrap()[made..].to_vec();
        let named = format!("a {pf:?} ");
        let naming = notices.iter().filter(|n| n.object.starts_with(&named));
        let expected = match facts.contains(&fact) {
            true => 0,
            false => 3,
        };
        if naming.count() != expected || notices.len() != expected {
            wrong.push(format!(
                "{pf:?} (DriverFact::{fact:?} {}): {} notices, not {expected}: {notices:?}",
                facts.contains(&fact),
                notices.len()
            ));
        }
        for (what, texture) in [
            ("Shared", &shared),
            ("Private", &private),
            ("heap", &placed),
        ] {
            if texture.has_decoded_twin() {
                wrong.push(format!("{pf:?} {what}: a decoded twin"));
            }
            if bytes_of(&device, texture, row) != written {
                wrong.push(format!(
                    "{pf:?} {what}: its bytes are not the blocks written"
                ));
            }
        }
        eprintln!(
            "BCOWN {name} {pf:?}: DriverFact::{fact:?} {}, {} compressed-texel-decode notices",
            facts.contains(&fact),
            notices.len()
        );
    }
    let after = device.work_counts();
    let decodes = [
        after.block_decode_passes - before.block_decode_passes,
        after.cpu_block_decodes - before.cpu_block_decodes,
    ];
    if decodes != [0, 0] {
        wrong.push(format!("decode passes and CPU decodes: {decodes:?}"));
    }
    assert!(wrong.is_empty(), "on {name}:\n{}", wrong.join("\n"));
}
