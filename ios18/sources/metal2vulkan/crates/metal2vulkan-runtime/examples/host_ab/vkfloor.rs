//! The driver's own cost under our calls: the Vulkan calls the runtime's creation, submission and
//! event paths make, timed on the same device the runtime's side of the A/B holds
//! (`Device::raw`), which on this Mac is MoltenVK. Metal has no equivalent call, so these rows
//! are the runtime's alone; what they measure is the share of a per-call gap that belongs to the
//! driver, which we never patch, and the rest is ours.
//!
//! Each row is a `percall` group, so it is timed, attributed and counted exactly as a Metal call
//! is, and it appears in the same table with no Metal column. A row times the Vulkan calls alone:
//! no planning, no bookkeeping and no state of ours.

use crate::gpu::Gpu;
use crate::percall::group;
use ash::vk;
use metal2vulkan_runtime as rt;
use std::ffi::c_void;

/// The buffers and images one window makes, and a buffer's and an image's size.
const BUFFERS: usize = 16;
const IMAGES: usize = 16;
const BUFFER_BYTES: u64 = 1 << 20;
const IMAGE_SIDE: u32 = 256;

/// The memory type of `bits` whose properties hold all of `wanted`, the first of them.
fn memory_type(
    properties: &vk::PhysicalDeviceMemoryProperties,
    bits: u32,
    wanted: vk::MemoryPropertyFlags,
) -> Option<u32> {
    (0..properties.memory_type_count).find(|index| {
        bits & (1 << index) != 0
            && properties.memory_types[*index as usize]
                .property_flags
                .contains(wanted)
    })
}

/// A buffer of [`BUFFER_BYTES`] as the runtime makes one for a `Shared` Metal buffer.
fn buffer_info() -> vk::BufferCreateInfo<'static> {
    vk::BufferCreateInfo::default()
        .size(BUFFER_BYTES)
        .usage(
            vk::BufferUsageFlags::STORAGE_BUFFER
                | vk::BufferUsageFlags::TRANSFER_SRC
                | vk::BufferUsageFlags::TRANSFER_DST,
        )
        .sharing_mode(vk::SharingMode::EXCLUSIVE)
}

/// An image as the runtime makes one for a 2D `BGRA8Unorm` texture of one level that draws render
/// into and shaders read.
fn image_info() -> vk::ImageCreateInfo<'static> {
    vk::ImageCreateInfo::default()
        .image_type(vk::ImageType::TYPE_2D)
        .format(vk::Format::B8G8R8A8_UNORM)
        .extent(vk::Extent3D {
            width: IMAGE_SIDE,
            height: IMAGE_SIDE,
            depth: 1,
        })
        .mip_levels(1)
        .array_layers(1)
        .samples(vk::SampleCountFlags::TYPE_1)
        .tiling(vk::ImageTiling::OPTIMAL)
        .usage(
            vk::ImageUsageFlags::SAMPLED
                | vk::ImageUsageFlags::COLOR_ATTACHMENT
                | vk::ImageUsageFlags::TRANSFER_SRC
                | vk::ImageUsageFlags::TRANSFER_DST,
        )
        .sharing_mode(vk::SharingMode::EXCLUSIVE)
        .initial_layout(vk::ImageLayout::UNDEFINED)
}

/// Time the driver's calls under the runtime's, `windows` windows each, on `device`'s own Vulkan
/// device. Nothing here submits anything or touches an object of the runtime's: the device's
/// queues are the runtime's to submit to (`CommandQueue::with_vk_queue`), so the submission path
/// has no floor row.
///
/// # Safety
///
/// `pages` is the host memory the no-copy group of [`crate::percall`] made its buffers over:
/// read-write pages of this process, at least eight megabytes, that outlive this call.
pub unsafe fn measure<G: Gpu>(g: &G, device: &rt::Device, windows: usize, pages: *mut u8) {
    // SAFETY: the handles are used only while the device lives, and only from this thread.
    let raw = unsafe { device.raw() };
    let vk = raw.device;
    // SAFETY: the physical device is the device's.
    let properties = unsafe {
        raw.instance
            .get_physical_device_memory_properties(raw.physical_device)
    };
    let host = vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT;

    // What the rows bind into and submit with, made once.
    let probe = unsafe { vk.create_buffer(&buffer_info(), None) }.expect("vkCreateBuffer");
    // SAFETY: the buffer is this device's.
    let buffer_needs = unsafe { vk.get_buffer_memory_requirements(probe) };
    let buffer_type = memory_type(&properties, buffer_needs.memory_type_bits, host)
        .expect("a host-visible memory type for a buffer");
    let image = unsafe { vk.create_image(&image_info(), None) }.expect("vkCreateImage");
    // SAFETY: the image is this device's.
    let image_needs = unsafe { vk.get_image_memory_requirements(image) };
    let image_type = memory_type(
        &properties,
        image_needs.memory_type_bits,
        vk::MemoryPropertyFlags::DEVICE_LOCAL,
    )
    .or_else(|| {
        memory_type(
            &properties,
            image_needs.memory_type_bits,
            vk::MemoryPropertyFlags::empty(),
        )
    })
    .expect("a memory type for an image");
    let stride = image_needs
        .size
        .next_multiple_of(image_needs.alignment.max(1));
    let block = |size: u64, index: u32| {
        let info = vk::MemoryAllocateInfo::default()
            .allocation_size(size)
            .memory_type_index(index);
        unsafe { vk.allocate_memory(&info, None) }.expect("vkAllocateMemory")
    };
    let buffer_block = block(BUFFER_BYTES * BUFFERS as u64, buffer_type);
    let image_block = block(stride * IMAGES as u64, image_type);
    let signalled = {
        let info = vk::FenceCreateInfo::default().flags(vk::FenceCreateFlags::SIGNALED);
        unsafe { vk.create_fence(&info, None) }.expect("vkCreateFence")
    };

    // The memory a buffer and an image take.
    group(
        g,
        "vk_create_buffer_1m",
        (BUFFERS, windows),
        || Vec::with_capacity(BUFFERS),
        |held: &mut Vec<vk::Buffer>, _| {
            let buffer = unsafe { vk.create_buffer(&buffer_info(), None) }.expect("vkCreateBuffer");
            let _ = unsafe { vk.get_buffer_memory_requirements(buffer) };
            held.push(buffer);
        },
        |held| {
            for buffer in held {
                unsafe { vk.destroy_buffer(buffer, None) };
            }
        },
    );
    group(
        g,
        "vk_allocate_memory_1m",
        (BUFFERS, windows),
        || Vec::with_capacity(BUFFERS),
        |held: &mut Vec<vk::DeviceMemory>, _| held.push(block(BUFFER_BYTES, buffer_type)),
        |held| {
            for memory in held {
                unsafe { vk.free_memory(memory, None) };
            }
        },
    );
    group(
        g,
        "vk_bind_buffer_memory",
        (BUFFERS, windows),
        || {
            (0..BUFFERS)
                .map(|_| unsafe { vk.create_buffer(&buffer_info(), None) }.expect("vkCreateBuffer"))
                .collect::<Vec<_>>()
        },
        |held: &mut Vec<vk::Buffer>, i| {
            unsafe { vk.bind_buffer_memory(held[i], buffer_block, i as u64 * BUFFER_BYTES) }
                .expect("vkBindBufferMemory")
        },
        |held| {
            for buffer in held {
                unsafe { vk.destroy_buffer(buffer, None) };
            }
        },
    );
    group(
        g,
        "vk_create_image_256",
        (IMAGES, windows),
        || Vec::with_capacity(IMAGES),
        |held: &mut Vec<vk::Image>, _| {
            let image = unsafe { vk.create_image(&image_info(), None) }.expect("vkCreateImage");
            let _ = unsafe { vk.get_image_memory_requirements(image) };
            held.push(image);
        },
        |held| {
            for image in held {
                unsafe { vk.destroy_image(image, None) };
            }
        },
    );
    group(
        g,
        "vk_bind_image_memory_256",
        (IMAGES, windows),
        || {
            (0..IMAGES)
                .map(|_| unsafe { vk.create_image(&image_info(), None) }.expect("vkCreateImage"))
                .collect::<Vec<_>>()
        },
        |held: &mut Vec<vk::Image>, i| {
            unsafe { vk.bind_image_memory(held[i], image_block, i as u64 * stride) }
                .expect("vkBindImageMemory")
        },
        |held| {
            for image in held {
                unsafe { vk.destroy_image(image, None) };
            }
        },
    );
    let view_info = |image| {
        vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(vk::Format::B8G8R8A8_UNORM)
            .subresource_range(vk::ImageSubresourceRange {
                aspect_mask: vk::ImageAspectFlags::COLOR,
                base_mip_level: 0,
                level_count: 1,
                base_array_layer: 0,
                layer_count: 1,
            })
    };
    {
        let bound = unsafe { vk.create_image(&image_info(), None) }.expect("vkCreateImage");
        unsafe { vk.bind_image_memory(bound, image_block, 0) }.expect("vkBindImageMemory");
        group(
            g,
            "vk_create_image_view_256",
            (IMAGES, windows),
            || Vec::with_capacity(IMAGES),
            |held: &mut Vec<vk::ImageView>, _| {
                held.push(
                    unsafe { vk.create_image_view(&view_info(bound), None) }
                        .expect("vkCreateImageView"),
                )
            },
            |held| {
                for view in held {
                    unsafe { vk.destroy_image_view(view, None) };
                }
            },
        );
        unsafe { vk.destroy_image(bound, None) };
    }

    // The host pages a no-copy buffer lies over, imported as the runtime imports them.
    let import = device.device_proc_addr(c"vkGetMemoryHostPointerPropertiesEXT");
    if let Some(import) = import {
        let import: vk::PFN_vkGetMemoryHostPointerPropertiesEXT =
            // SAFETY: the loader answered for exactly this entry point's name.
            unsafe { std::mem::transmute(import) };
        let types = {
            let mut found = vk::MemoryHostPointerPropertiesEXT::default();
            // SAFETY: the pages are host memory of this process, page-aligned.
            let result = unsafe {
                import(
                    vk.handle(),
                    vk::ExternalMemoryHandleTypeFlags::HOST_ALLOCATION_EXT,
                    pages.cast::<c_void>(),
                    &mut found,
                )
            };
            assert_eq!(result, vk::Result::SUCCESS, "host pointer properties");
            found.memory_type_bits
        };
        let index = memory_type(&properties, types, host).expect("a memory type for host pages");
        group(
            g,
            "vk_import_host_pointer_1m",
            (BUFFERS.min(8), windows),
            || Vec::with_capacity(8),
            |held: &mut Vec<vk::DeviceMemory>, i| {
                let mut info = vk::ImportMemoryHostPointerInfoEXT::default()
                    .handle_type(vk::ExternalMemoryHandleTypeFlags::HOST_ALLOCATION_EXT)
                    // SAFETY: the window's buffers lie in the pages the caller gave, one
                    // megabyte each ([`crate::percall`]'s no-copy group).
                    .host_pointer(unsafe { pages.add(i * BUFFER_BYTES as usize) }.cast());
                let allocate = vk::MemoryAllocateInfo::default()
                    .allocation_size(BUFFER_BYTES)
                    .memory_type_index(index)
                    .push_next(&mut info);
                held.push(
                    unsafe { vk.allocate_memory(&allocate, None) }.expect("an imported pointer"),
                );
            },
            |held| {
                for memory in held {
                    unsafe { vk.free_memory(memory, None) };
                }
            },
        );
    }

    group(
        g,
        "vk_wait_for_fences_signalled",
        (256, windows),
        || (),
        |_, _| {
            unsafe { vk.wait_for_fences(&[signalled], true, u64::MAX) }.expect("vkWaitForFences")
        },
        drop,
    );

    // A timeline semaphore made, and its value read: what a shared event is.
    group(
        g,
        "vk_create_semaphore_timeline",
        (16, windows),
        || Vec::with_capacity(16),
        |held: &mut Vec<vk::Semaphore>, _| {
            let mut timeline = vk::SemaphoreTypeCreateInfo::default()
                .semaphore_type(vk::SemaphoreType::TIMELINE)
                .initial_value(0);
            let info = vk::SemaphoreCreateInfo::default().push_next(&mut timeline);
            held.push(unsafe { vk.create_semaphore(&info, None) }.expect("vkCreateSemaphore"));
        },
        |held| {
            for semaphore in held {
                unsafe { vk.destroy_semaphore(semaphore, None) };
            }
        },
    );
    {
        let mut timeline = vk::SemaphoreTypeCreateInfo::default()
            .semaphore_type(vk::SemaphoreType::TIMELINE)
            .initial_value(0);
        let info = vk::SemaphoreCreateInfo::default().push_next(&mut timeline);
        let semaphore = unsafe { vk.create_semaphore(&info, None) }.expect("vkCreateSemaphore");
        group(
            g,
            "vk_get_semaphore_counter_value",
            (256, windows),
            || (),
            |_, _| {
                std::hint::black_box(
                    unsafe { vk.get_semaphore_counter_value(semaphore) }
                        .expect("vkGetSemaphoreCounterValue"),
                );
            },
            drop,
        );
        unsafe { vk.destroy_semaphore(semaphore, None) };
    }

    unsafe { vk.destroy_fence(signalled, None) };
    unsafe { vk.destroy_buffer(probe, None) };
    unsafe { vk.destroy_image(image, None) };
    unsafe { vk.free_memory(buffer_block, None) };
    unsafe { vk.free_memory(image_block, None) };
}
