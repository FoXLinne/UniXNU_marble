//! A host step makes what it needs (`exit_guard.rs`): inside the exit guard's section, marked as a
//! host step, a neutral not made before is made, and found again after it, the same object; and
//! the creation door (`creation.rs`) makes an image, a render pass of each entry point and a
//! framebuffer, whose entry points are among those a host step calls (`exit_guard::HOST_STEP`).
//!
//! A device test: compiled only under the validation crate's GPU safety cfgs (`m2v_gpu`,
//! `m2v_cpu_icd`), on the kind of device the cfg selects. The end-to-end cases, a host step's draw
//! of a state made after its commit and a host step's run of another shape, are the validation
//! lanes' `a_host_step_s_draw_of_a_state_made_after_the_commit_binds_a_neutral_made_before_it` and
//! `a_host_step_s_run_of_another_shape_makes_its_objects_and_draws`.

use crate::device::lock;
use crate::exit_guard::{self, Call};
use crate::memory::{self, MemoryClass};
use crate::{creation, release};
use crate::{Device, DeviceKind, DeviceOptions, ShaderCacheOptions};
use ash::vk;
use metal2vulkan::bind::NeutralResource;
use metal2vulkan::pipeline::SampledType;
use metal2vulkan::vk::{Format, ImageViewType};
use std::path::{Path, PathBuf};

/// A scratch directory for the device's shader cache, removed when dropped.
struct Scratch(PathBuf);

impl Scratch {
    fn new() -> Scratch {
        let path =
            std::env::temp_dir().join(format!("m2v-host-step-neutrals-{}", std::process::id()));
        std::fs::create_dir_all(&path).unwrap();
        Scratch(path)
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// The device the cfg selects: a CPU device under `m2v_cpu_icd`, else a GPU, its shader cache
/// in `cache`.
fn device(cache: &Path) -> Device {
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
        ..DeviceOptions::default()
    };
    Device::system_default_with(&options).unwrap()
}

/// Run `f` as a host step does: inside a section of the exit guard, marked as a host step.
fn in_host_step<T>(f: impl FnOnce() -> T) -> T {
    let busy = exit_guard::enter().expect("the process is not exiting");
    let made = exit_guard::host_step(f);
    drop(busy);
    made
}

/// Inside a host step, a neutral texel view, sampler and image view not made yet are made; after
/// it, on the program's thread and in another host step, each is found, the same object.
#[test]
fn a_host_step_makes_the_neutrals_nothing_made_before_it() {
    let scratch = Scratch::new();
    let device = device(&scratch.0);
    let image = NeutralResource::Image {
        view_type: ImageViewType::TYPE_2D,
        sampled_type: SampledType::Float,
        depth_compare: false,
        samples: 1,
        storage_format: None,
    };
    let texel = || device.neutral_texel_view(Format::R32_SFLOAT, false);
    let sampler = || device.neutral_sampler(true);
    let view = || device.neutral_view(image);
    let made = in_host_step(|| (texel().unwrap(), sampler().unwrap(), view().unwrap()));
    assert_eq!(
        (texel().unwrap(), sampler().unwrap(), view().unwrap()),
        made
    );
    let found = in_host_step(|| (texel().unwrap(), sampler().unwrap(), view().unwrap()));
    assert_eq!(found, made);
}

/// Inside a host step, the creation door makes an image, a render pass by `vkCreateRenderPass`
/// and by `vkCreateRenderPass2`, and a framebuffer over the image's view; their entry points are
/// among the host step's.
#[test]
fn a_host_step_makes_an_image_render_passes_and_a_framebuffer() {
    for call in [
        Call::CreateImage,
        Call::GetImageMemoryRequirements,
        Call::BindImageMemory,
        Call::CreateRenderPass,
        Call::CreateRenderPass2,
        Call::CreateFramebuffer,
    ] {
        assert!(exit_guard::HOST_STEP.contains(&call), "{call:?}");
    }
    let scratch = Scratch::new();
    let device = device(&scratch.0);
    let shared = device.shared();
    let raw = shared.vk();
    let format = vk::Format::R8G8B8A8_UNORM;
    let extent = vk::Extent3D {
        width: 4,
        height: 4,
        depth: 1,
    };
    in_host_step(|| {
        let info = vk::ImageCreateInfo::default()
            .image_type(vk::ImageType::TYPE_2D)
            .format(format)
            .extent(extent)
            .mip_levels(1)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::OPTIMAL)
            .usage(vk::ImageUsageFlags::COLOR_ATTACHMENT)
            .initial_layout(vk::ImageLayout::UNDEFINED);
        // SAFETY: the device is live; a 2D color attachment of this format is mandatory.
        let image = unsafe { creation::image(raw, &info) }.unwrap();
        // SAFETY: `image` is a live image of this device.
        let requirements = unsafe { raw.get_image_memory_requirements(image) };
        let placement = memory::allocate_image_while_queued(
            &shared.memory,
            raw,
            &requirements,
            MemoryClass::Private,
        )
        .unwrap();
        // SAFETY: the placement meets the image's requirements and is bound nowhere else.
        unsafe { raw.bind_image_memory(image, placement.memory, placement.offset) }.unwrap();
        let range = vk::ImageSubresourceRange {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            base_mip_level: 0,
            level_count: 1,
            base_array_layer: 0,
            layer_count: 1,
        };
        let view_info = vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(format)
            .subresource_range(range);
        // SAFETY: the image is live and bound.
        let view = unsafe { raw.create_image_view(&view_info, None) }.unwrap();
        let attachment = vk::AttachmentDescription::default()
            .format(format)
            .samples(vk::SampleCountFlags::TYPE_1)
            .load_op(vk::AttachmentLoadOp::DONT_CARE)
            .store_op(vk::AttachmentStoreOp::STORE)
            .initial_layout(vk::ImageLayout::UNDEFINED)
            .final_layout(vk::ImageLayout::GENERAL);
        let reference = vk::AttachmentReference {
            attachment: 0,
            layout: vk::ImageLayout::GENERAL,
        };
        let subpass = vk::SubpassDescription::default()
            .pipeline_bind_point(vk::PipelineBindPoint::GRAPHICS)
            .color_attachments(std::slice::from_ref(&reference));
        let pass_info = vk::RenderPassCreateInfo::default()
            .attachments(std::slice::from_ref(&attachment))
            .subpasses(std::slice::from_ref(&subpass));
        // SAFETY: the device is live; the pass is valid.
        let pass = unsafe { creation::render_pass(raw, &pass_info) }.unwrap();
        let attachment2 = vk::AttachmentDescription2::default()
            .format(format)
            .samples(vk::SampleCountFlags::TYPE_1)
            .load_op(vk::AttachmentLoadOp::DONT_CARE)
            .store_op(vk::AttachmentStoreOp::STORE)
            .initial_layout(vk::ImageLayout::UNDEFINED)
            .final_layout(vk::ImageLayout::GENERAL);
        let reference2 = vk::AttachmentReference2::default()
            .attachment(0)
            .layout(vk::ImageLayout::GENERAL)
            .aspect_mask(vk::ImageAspectFlags::COLOR);
        let subpass2 = vk::SubpassDescription2::default()
            .pipeline_bind_point(vk::PipelineBindPoint::GRAPHICS)
            .color_attachments(std::slice::from_ref(&reference2));
        let pass_info2 = vk::RenderPassCreateInfo2::default()
            .attachments(std::slice::from_ref(&attachment2))
            .subpasses(std::slice::from_ref(&subpass2));
        // SAFETY: the device is of Vulkan 1.2 or later (the runtime's floor); the pass is valid.
        let pass2 = unsafe { creation::render_pass2(raw, &pass_info2) }.unwrap();
        let framebuffer_info = vk::FramebufferCreateInfo::default()
            .render_pass(pass2)
            .attachments(std::slice::from_ref(&view))
            .width(extent.width)
            .height(extent.height)
            .layers(1);
        // SAFETY: the view is of the pass's attachment's format and the framebuffer's extent.
        let framebuffer = unsafe { creation::framebuffer(raw, &framebuffer_info) }.unwrap();
        // SAFETY: each is this device's, and nothing used them.
        unsafe {
            release::object(raw, framebuffer);
            release::object(raw, pass2);
            release::object(raw, pass);
            release::object(raw, view);
            release::object(raw, image);
        }
        lock(&shared.memory).free(raw, &placement);
    });
}
