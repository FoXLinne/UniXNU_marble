//! The fences that end command buffers, where the driver needs them: the batch that ends a command
//! buffer signals one with its submission, and the completion turn finishes the command buffer
//! only once both its queue's timeline reached the batch's value and the fence is signaled
//! ([`completion`](super::completion)).
//!
//! - **The value is the completion, and Vulkan says so.** A batch's timeline signal follows the
//!   batch's commands: "The first synchronization scope of each semaphore signal operation defined
//!   by this command includes every command in the same batch that the signal operation is defined
//!   in, and all commands that occur earlier in submission order" (`vkQueueSubmit`). A host wait
//!   for the value orders the host after that signal: "signal operations happening before host
//!   operations that wait on them" system-synchronize-with them (the memory model), where
//!   `vkGetSemaphoreCounterValue` alone gives no such dependency (its value "may immediately be
//!   out of date"). So once the waiter's `vkWaitSemaphores` returned for the value, the batch's
//!   commands completed and what they used may be released, and nothing but the waiter's wait is
//!   relied on ([`completion`](super::completion), where the waits are, and what publishes a
//!   batch). The waiter waits for one semaphore, never for any of several
//!   (`RuntimeDriverDefect::WaitForAnyObjectLosesASignal`).
//! - **MoltenVK reads some of it later, and only some.** MoltenVK signals a timeline semaphore
//!   from the GPU (`MVKSemaphoreMTLEvent::encodeSignal`, `[mtlCmdBuff encodeSignalEvent:]`), so
//!   the host sees the value before the Metal command buffer's completed handlers run, and it
//!   signals a fence from the last of those handlers. What those handlers read decides whether the
//!   value alone is enough. Every `addCompletedHandler:` of 1.4.2's source, read in full (the
//!   `addScheduledHandler:` of `MVKImage.mm` is a swapchain's, which the runtime makes none of):
//!   - `MVKCommandEncoder::finishQueries` (`MVKCommandBuffer.mm`) calls `finishQueries` on every
//!     query pool the command buffer reset, began or ended, through a raw `MVKQueryPool*` it does
//!     not retain, and that call takes the pool's `_availabilityLock` and writes its
//!     `_availability`. **A Vulkan object of the command buffer's**, and the only handler that
//!     reads one the runtime may release at the value. Registered only where the encoder
//!     collected an activated query (`_pActivatedQueries`, from `addActivatedQueries`), so only
//!     for a command buffer that recorded a query command.
//!   - `MVKCommandEncoder::getTempMTLBuffer` and `MVKCmdUpdateBuffer::encode` return a
//!     `MVKMTLBufferAllocation` to the pool it came from, which lives in the `MVKCommandPool` of
//!     the `VkCommandPool` the command buffer was allocated from
//!     (`MVKCommandEncoder::getCommandEncodingPool`). The runtime calls
//!     `vkDestroyCommandPool` on a command buffer's pool only as the device is torn down, after
//!     its queues are idle, and resets a spare with `vkResetCommandPool` of no flag, which leaves
//!     `MVKCommandPool`'s encoding pool alone (`MVKCommandPool::reset` trims only for
//!     `VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT`, which the runtime never passes;
//!     `spares.rs`). So that pool outlives every handler.
//!   - `MVKImagePlane::pullFromDeviceOnCompletion` (`MVKImage.mm`) copies a texture into mapped
//!     host-coherent memory through its `MVKImagePlane*`, a `VkImage`'s. Registered only from
//!     `MVKImagePlane::applyImageMemoryBarrier`, and only where the barrier's destination names
//!     `VK_ACCESS_HOST_READ_BIT` or `VK_ACCESS_MEMORY_READ_BIT` into `VK_IMAGE_LAYOUT_GENERAL`;
//!     no image memory barrier of the runtime names either (`image_barrier.rs`, which is where
//!     all of them are made, held to it by `tests::image_barriers`), so it is registered for none
//!     of the runtime's command buffers.
//!   - `MVKQueueCommandBufferSubmission::finish` (`MVKQueue.mm`) reads the submission, its
//!     `MVKQueue`, and the signal semaphores and fence it retains for exactly this
//!     ("We therefore retain() each here to ensure they live long enough for this submission to
//!     finish using them"); `MVKQueue::handleMTLCommandBufferError` reads the `MTLCommandBuffer`
//!     and the device. Neither reads an object of the command buffer.
//!   - `MVKCommandEncoder::getStageCountersMTLFence` releases an `MTLFence` the block retains, and
//!     `MVKCmdBlitImage::encode`'s three release an `MTLTexture` the block retains. Metal objects
//!     of MoltenVK's own, no Vulkan object.
//!
//!   So the hazard is one handler and one object class: the query pools of a command buffer that
//!   recorded a query command. The device's driver defects name it
//!   (`RuntimeDriverDefect::ReadsCommandBuffersAfterTheirSignal`, `driver_defects.rs`).
//! - **The fence is kept for every command buffer there all the same, and it is the faster wait.**
//!   A command buffer that records no query command could learn its completion from the value
//!   alone, which Vulkan makes enough (above). It does not, for two measured reasons.
//!   - **The host sees the fence before it sees the value.** MoltenVK signals the fence from the
//!     last of the completed handlers, after the GPU signalled the value, but its host wait for a
//!     timeline value goes through an `MTLSharedEvent`'s listener while its fence is a condition
//!     variable the handler thread broadcasts. On an Apple M3 Max (macOS 27.0.1, MoltenVK 1.4.2,
//!     `runtime_lane::commit_costs`) one submission of a barrier alone, waited for 200 times after
//!     20: its fence returned 49.3 us after the submission (p10 44.6, p90 60.1) and its timeline
//!     value 58.6 us (p10 48.2, p90 82.6). Waiting for the value in its place would cost about 9
//!     us a command buffer at the median and 22 at the 90th percentile, not save any. For scale, a
//!     whole empty command buffer of the runtime completes 32.4 us after its commit there (p90
//!     43.1), so the handler chain is no hundreds of microseconds.
//!   - **A queue whose failures are relayed needs one for every batch** (`device/relay.rs`), and
//!     every MoltenVK device the runtime creates relays: MoltenVK reports a command buffer's
//!     failure to the instance's messenger from the first of the same completed handlers, before
//!     the fence's, and the relay gives the failure to the first submission in flight whose fence
//!     is unsignaled. A command buffer finished at its value could be finished before its failure
//!     arrived. A device the runtime adopts relays none (`Device::from_raw`: an adopted instance
//!     has no messenger of the runtime's), so there the defect above is the whole reason.
//! - **Only there**. Every other driver signals the fence with the value, so a command buffer there
//!   ends at its value alone and its commit takes no fence ([`ends_at_fence`]): NVIDIA's
//!   `vkCreateFence` on the commit path waited up to 1.9 s for other processes' work.
//! - **Pooled, and bounded.** A fence is taken when the last batch is submitted (made only when
//!   none is idle) and given back, reset, once a completion turn saw it signaled. At most
//!   [`IDLE`] wait idle; the ones beyond are released. So the fences live are the command buffers
//!   in flight plus at most [`IDLE`], whatever the number of command buffers committed.
//! - **The exit guard's doors.** Making one is the scheduler's (inside its section), waiting for
//!   and resetting them the queues' waiters' (inside their sections), and every release goes
//!   through `release.rs`; each entry point is in the guard's table and primed
//!   (`exit_guard`, `device/prime.rs`).

use crate::device::lock;
use crate::exit_guard::{self, Call};
use crate::release;
use crate::Device;
use ash::vk;
use metal2vulkan::error::RuntimeDriverDefect;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

/// The most fences kept idle.
pub(crate) const IDLE: usize = 16;

/// Whether a submission to `device`'s `VkQueue` `queue` that ends a command buffer takes a fence:
/// where the driver reads a command buffer's query pools after its signal, or the queue's
/// failures are relayed (the module docs).
pub(crate) fn ends_at_fence(device: &Device, queue: usize) -> bool {
    device.shared().flights(queue).is_some()
        || (device.properties().driver_defects)
            .contains(&RuntimeDriverDefect::ReadsCommandBuffersAfterTheirSignal)
}

/// A device's idle fences, each unsignaled and in no pending submission.
#[derive(Default)]
pub(crate) struct Fences {
    idle: Mutex<Vec<vk::Fence>>,
    /// The fences made, none idle: `WorkCounts::fences_made`.
    made: AtomicU64,
}

impl Fences {
    /// An unsignaled fence of `device`: an idle one, else a new one.
    ///
    /// # Safety
    ///
    /// The fences are `device`'s; the caller is inside a section of the exit guard.
    pub(crate) unsafe fn take(&self, device: &ash::Device) -> Result<vk::Fence, vk::Result> {
        if let Some(fence) = self.take_idle() {
            return Ok(fence);
        }
        // SAFETY: the device is live (the caller holds it).
        let made = unsafe { device.create_fence(&vk::FenceCreateInfo::default(), None) };
        exit_guard::called(Call::CreateFence);
        self.made.fetch_add(1, Ordering::Relaxed);
        made
    }

    /// How many fences [`take`](Self::take) made so far.
    pub(crate) fn made(&self) -> u64 {
        self.made.load(Ordering::Relaxed)
    }

    /// Give back `fences` of `device`, each unsignaled (reset, or never submitted) and in no
    /// pending submission: kept idle up to [`IDLE`], the rest released. `fences` is left empty,
    /// its room kept for the caller's next ones.
    ///
    /// # Safety
    ///
    /// As above.
    pub(crate) unsafe fn give(&self, device: &ash::Device, fences: &mut Vec<vk::Fence>) {
        self.keep(fences);
        for fence in fences.drain(..) {
            // SAFETY: the caller's: no pending submission uses it.
            unsafe { release::object(device, fence) };
        }
    }

    /// Release every idle fence: the device is being torn down.
    ///
    /// # Safety
    ///
    /// As above.
    pub(crate) unsafe fn release(&self, device: &ash::Device) {
        let idle: Vec<vk::Fence> = lock(&self.idle).drain(..).collect();
        for fence in idle {
            // SAFETY: the caller's.
            unsafe { release::object(device, fence) };
        }
    }

    /// An idle fence, if any.
    pub(crate) fn take_idle(&self) -> Option<vk::Fence> {
        lock(&self.idle).pop()
    }

    /// Keep `fences` idle up to [`IDLE`], the first of them first; the ones beyond stay in
    /// `fences`, for the caller to release.
    pub(crate) fn keep(&self, fences: &mut Vec<vk::Fence>) {
        let mut idle = lock(&self.idle);
        let kept = IDLE.saturating_sub(idle.len()).min(fences.len());
        idle.extend(fences.drain(..kept));
    }
}
