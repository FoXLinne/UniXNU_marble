//! The runtime's one door to `vkCreateImage`, `vkCreateRenderPass`, `vkCreateRenderPass2` and
//! `vkCreateFramebuffer`, so their entry points stay in the exit guard's tables; and to the other
//! makings a driver charges for — `vkCreateBuffer`, `vkCreateImageView`, `vkAllocateMemory`,
//! `vkBindImageMemory` and `vkBindBufferMemory` — so each counts itself where it is called.
//!
//! Each is made where it is first needed: on the program's thread, in a section of the device's
//! lowering thread, which lowers render encoders' calls (`render/lowering_worker.rs`), or in a host
//! step (`exit_guard::host_step`) on the steps' thread (`scheduler/steps.rs`), an object no earlier
//! settle made (a
//! state made after its command buffer's commit, whose run needs an attachment's stand-in, a
//! shadow, or a pass and framebuffer of its own shape). So the four, and an image's memory
//! requirements and binding, are in the guard's table (`exit_guard::Call`) and among the calls a
//! host step makes (`exit_guard::HOST_STEP`). The device's priming (`device/prime.rs`) makes its
//! own, on the thread that makes the device. No other file calls these entry points: a source test
//! fails when one does (`tests/exit_guard.rs`).
//!
//! **Each making is counted at its caller's place** (`churn_counts.rs`, as the releases are), so
//! what one call of the API made the driver do is a count of that call's own work rather than an
//! inference from a separately timed driver floor: a 256x256 texture's `vkCreateImage` and its
//! binding, a buffer's `vkCreateBuffer` and whether its memory came from a pool or a fresh
//! `vkAllocateMemory`. A count is deterministic, so a test can hold a call to it where a time
//! cannot.

use crate::allocations;
use crate::churn_counts::{vk_made, VkMaking};
use crate::error::{vulkan, Error};
use ash::prelude::VkResult;
use ash::vk;
use std::panic::Location;

/// A `VkImage` of `info`, made by `device`.
///
/// # Safety
///
/// As `vkCreateImage`'s: `device` is live, and `info` and its chain are valid for it.
#[track_caller]
pub(crate) unsafe fn image(
    device: &ash::Device,
    info: &vk::ImageCreateInfo<'_>,
) -> Result<vk::Image, Error> {
    vk_made(VkMaking::Image, Location::caller());
    // SAFETY: the caller's.
    unsafe { device.create_image(info, None) }.map_err(vulkan)
}

/// A `VkImage` of `info`, made by `device`, or the driver's refusal of it: an image whose making
/// the driver may refuse for its parameters alone (one of a DRM format modifier's explicit
/// layout, `dma_buf.rs`), whose caller takes another way then. A lost device is
/// `Error::DeviceLost`.
///
/// # Safety
///
/// As `vkCreateImage`'s: `device` is live, and `info` and its chain are valid for it.
#[track_caller]
pub(crate) unsafe fn image_or_refusal(
    device: &ash::Device,
    info: &vk::ImageCreateInfo<'_>,
) -> Result<Result<vk::Image, vk::Result>, Error> {
    vk_made(VkMaking::Image, Location::caller());
    // SAFETY: the caller's.
    match unsafe { device.create_image(info, None) } {
        Ok(raw) => Ok(Ok(raw)),
        Err(vk::Result::ERROR_DEVICE_LOST) => Err(Error::DeviceLost),
        Err(refusal) => Ok(Err(refusal)),
    }
}

/// A `VkBuffer` of `info`, made by `device`, with the driver's result as it gave it: the callers
/// read a refusal differently (a staging buffer's caller takes another way, a program's buffer
/// turns it into `Error`).
///
/// # Safety
///
/// As `vkCreateBuffer`'s: `device` is live, and `info` and its chain are valid for it.
#[track_caller]
pub(crate) unsafe fn buffer(
    device: &ash::Device,
    info: &vk::BufferCreateInfo<'_>,
) -> VkResult<vk::Buffer> {
    vk_made(VkMaking::Buffer, Location::caller());
    // SAFETY: the caller's.
    unsafe { device.create_buffer(info, None) }
}

/// A `VkImageView` of `info`, made by `device`.
///
/// # Safety
///
/// As `vkCreateImageView`'s: `device` is live, and `info` and its chain are valid for it.
#[track_caller]
pub(crate) unsafe fn image_view(
    device: &ash::Device,
    info: &vk::ImageViewCreateInfo<'_>,
) -> VkResult<vk::ImageView> {
    vk_made(VkMaking::ImageView, Location::caller());
    // SAFETY: the caller's.
    unsafe { device.create_image_view(info, None) }
}

/// `VkDeviceMemory` of `info`, allocated by `device`: one call of `vkAllocateMemory`, counted
/// whether or not the driver gave it, because a caller that tries memory types in turn pays for
/// each try. Refused (`ERROR_TOO_MANY_OBJECTS`) with no call where the device has
/// `maxMemoryAllocationCount` allocations live (`allocations.rs`).
///
/// # Safety
///
/// As `vkAllocateMemory`'s: `device` is live, and `info` and its chain are valid for it and live
/// for the call.
#[track_caller]
pub(crate) unsafe fn allocate_memory(
    device: &ash::Device,
    info: &vk::MemoryAllocateInfo<'_>,
) -> VkResult<vk::DeviceMemory> {
    let Some(admitted) = allocations::before_allocation(device.handle()) else {
        return Err(vk::Result::ERROR_TOO_MANY_OBJECTS);
    };
    vk_made(VkMaking::Memory, Location::caller());
    note_allocation_site(info.allocation_size);
    // SAFETY: the caller's.
    let memory = unsafe { device.allocate_memory(info, None) };
    allocations::allocated(admitted, memory.is_ok());
    memory
}

/// The call stacks that allocated device memory of at least [`SITE_BYTES`], by the bytes they
/// allocated: what a process's GPU memory went to, without a debugger on the device. A stack is
/// its first [`SITE_FRAMES`] return addresses past this function's; at most [`SITE_COUNT`]
/// stacks are kept, a new one past them counted under the last. Kept only with the diagnostics on
/// (`stage_trace::diagnostics`).
const SITE_BYTES: u64 = 4 << 20;
const SITE_FRAMES: usize = 10;
const SITE_COUNT: usize = 32;
static SITES: std::sync::Mutex<Vec<AllocationSite>> = std::sync::Mutex::new(Vec::new());

/// One call stack's allocations ([`allocation_sites`]).
#[derive(Clone, Copy, Debug, Default)]
pub struct AllocationSite {
    /// Bytes allocated so far.
    pub bytes: u64,
    /// Allocations so far.
    pub count: u64,
    /// Return addresses, innermost first; 0 past the stack's end.
    pub frames: [usize; SITE_FRAMES],
}

fn note_allocation_site(bytes: u64) {
    if bytes < SITE_BYTES || !crate::stage_trace::diagnostics() {
        return;
    }
    let mut raw = [std::ptr::null_mut::<libc::c_void>(); SITE_FRAMES + 2];
    // SAFETY: `raw` holds the count passed.
    #[cfg(target_vendor = "apple")]
    let depth = unsafe { libc::backtrace(raw.as_mut_ptr(), raw.len() as libc::c_int) }.max(0) as usize;
    #[cfg(not(target_vendor = "apple"))]
    let depth = 0usize;
    let mut frames = [0usize; SITE_FRAMES];
    for (frame, address) in frames.iter_mut().zip(raw.iter().take(depth).skip(2)) {
        // The pointer authentication bits of a return address (arm64e) are not part of it.
        *frame = (*address as usize) & ((1usize << 47) - 1);
    }
    let Ok(mut sites) = SITES.lock() else { return };
    let at = match sites.iter().position(|site| site.frames == frames) {
        Some(at) => at,
        None if sites.len() < SITE_COUNT => {
            sites.push(AllocationSite { frames, ..Default::default() });
            sites.len() - 1
        }
        None => sites.len() - 1,
    };
    sites[at].bytes += bytes;
    sites[at].count += 1;
}

/// The call stacks that allocated device memory of 4 MiB or more, most bytes first.
pub fn allocation_sites() -> Vec<AllocationSite> {
    let mut sites = SITES.lock().map(|sites| sites.clone()).unwrap_or_default();
    sites.sort_by_key(|site| std::cmp::Reverse(site.bytes));
    sites
}

/// `image` bound to `memory` at `offset`.
///
/// # Safety
///
/// As `vkBindImageMemory`'s: `device` is live, `image` is its unbound image, and `memory` is its
/// memory of a type the image takes, long enough for it from `offset`.
#[track_caller]
pub(crate) unsafe fn bind_image(
    device: &ash::Device,
    image: vk::Image,
    memory: vk::DeviceMemory,
    offset: vk::DeviceSize,
) -> VkResult<()> {
    vk_made(VkMaking::ImageBinding, Location::caller());
    // SAFETY: the caller's.
    unsafe { device.bind_image_memory(image, memory, offset) }
}

/// `buffer` bound to `memory` at `offset`.
///
/// # Safety
///
/// As `vkBindBufferMemory`'s: `device` is live, `buffer` is its unbound buffer, and `memory` is
/// its memory of a type the buffer takes, long enough for it from `offset`.
#[track_caller]
pub(crate) unsafe fn bind_buffer(
    device: &ash::Device,
    buffer: vk::Buffer,
    memory: vk::DeviceMemory,
    offset: vk::DeviceSize,
) -> VkResult<()> {
    vk_made(VkMaking::BufferBinding, Location::caller());
    // SAFETY: the caller's.
    unsafe { device.bind_buffer_memory(buffer, memory, offset) }
}

/// A `VkRenderPass` of `info`, made by `device`.
///
/// # Safety
///
/// As `vkCreateRenderPass`'s: `device` is live, and `info` and its chain are valid for it.
pub(crate) unsafe fn render_pass(
    device: &ash::Device,
    info: &vk::RenderPassCreateInfo<'_>,
) -> Result<vk::RenderPass, Error> {
    // SAFETY: the caller's.
    unsafe { device.create_render_pass(info, None) }.map_err(vulkan)
}

/// A `VkRenderPass` of `info` (`vkCreateRenderPass2`), made by `device`.
///
/// # Safety
///
/// As `vkCreateRenderPass2`'s: `device` is live and of Vulkan 1.2 or later, and `info` and its
/// chain are valid for it.
pub(crate) unsafe fn render_pass2(
    device: &ash::Device,
    info: &vk::RenderPassCreateInfo2<'_>,
) -> Result<vk::RenderPass, Error> {
    // SAFETY: the caller's.
    unsafe { device.create_render_pass2(info, None) }.map_err(vulkan)
}

/// A `VkFramebuffer` of `info`, made by `device`.
///
/// # Safety
///
/// As `vkCreateFramebuffer`'s: `device` is live, and `info` and its chain are valid for it.
pub(crate) unsafe fn framebuffer(
    device: &ash::Device,
    info: &vk::FramebufferCreateInfo<'_>,
) -> Result<vk::Framebuffer, Error> {
    // SAFETY: the caller's.
    unsafe { device.create_framebuffer(info, None) }.map_err(vulkan)
}
