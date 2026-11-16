//! A dispatch encoded over a no-copy buffer's pages, its command buffer committed behind a
//! shared-event wait, the buffer detached and rebound before the host signals: the rebind encodes
//! the compute encoder again (`command_buffer/reencode.rs`), and this reads what the encoding
//! again recorded, not only what the submission then wrote.
//!
//! `rebind_behind_a_wait.rs` asserts the outcome of the sequence once per keep-alive leg. This
//! asserts the condition behind a wrong outcome instead, which holds far more often than the
//! wrong write shows: a dispatch encoded again must allocate and write its own set 0, never bind
//! one an earlier command of the same command buffer wrote (`binding::SetCache`,
//! `WorkCounts::set_zero_sets_found`).
//!
//! The cache is exact for commands recorded as the program encodes them: a kept set is kept only
//! once its command records, and the command then holds every object its descriptors name until
//! the command buffer completes, so no handle it holds can come to name another object. A command
//! buffer committed and not yet first submitted holds what its commands took only *weakly*
//! (`command_buffer/mod.rs`), exactly so that a rebind may let the pages go; the rebind frees the
//! import the kept set was written for and then encodes the encoder again
//! (`buffer_descriptor.rs`), inside one call. The new import's `VkBuffer` may take the freed
//! one's handle, and the cache compares and hashes a buffer descriptor by that handle with its
//! offset and range (`binding::SetWrite::same`, `binding::set_key`), so the dispatch encoded
//! again is handed the first encoding's set and reaches whatever that handle names now.
//!
//! The counts are read around the rebind alone, so nothing of the submission is in them.
//!
//! Device test: compiled only under the validation crate's GPU safety cfgs. The kernel is
//! `scale_and_offset` of the owned fixture
//! `validation/fixtures/public/examples/runtime_examples.ll`, as `dispatch_pending.rs` runs it.

use super::blit_holds::{device, Scratch, WAIT};
use super::dispatch_pending::state;
use crate::{
    Buffer, CommandBuffer, CommandBufferError, CommandBufferStatus, ComputePipelineState, Device,
    Error, EventRef, HostMemory, KeepAlive, ResourceOptions, Size,
};
use std::alloc::Layout;
use std::ptr::NonNull;

/// The buffer's bytes: one page of any host, as the consumer's are.
const BYTES: usize = 16 << 10;

/// The threads the dispatch runs, in one threadgroup, and the scale it multiplies by.
const COUNT: u64 = 64;
const SCALE: f32 = 2.0;

/// The rounds it runs. The condition it asserts held in about half the rounds measured on an M3
/// Max (105 of 200), so a few tens of rounds meet it or the tree does not have it; the wrong
/// write it leads to showed in about one round in two hundred, so no round count makes that one
/// a gate.
const ROUNDS: usize = 40;

/// One page of host memory, zeroed, freed when dropped.
struct Page(NonNull<u8>);

impl Page {
    fn layout() -> Layout {
        Layout::from_size_align(BYTES, BYTES).unwrap()
    }

    fn new() -> Page {
        // SAFETY: the layout's size is not zero.
        let pointer = unsafe { std::alloc::alloc_zeroed(Page::layout()) };
        Page(NonNull::new(pointer).unwrap())
    }

    /// Whether the kernel wrote it: its words are zero until it does.
    fn written(&self) -> bool {
        // SAFETY: `BYTES` bytes, allocated in `new` aligned past a word, which no command writes
        // any more (its command buffer completed).
        let words = unsafe { std::slice::from_raw_parts(self.0.as_ptr().cast::<u32>(), BYTES / 4) };
        words.iter().any(|&word| word != 0)
    }

    /// Its pages, for a no-copy buffer or a rebind.
    ///
    /// # Safety
    ///
    /// They outlive every buffer over them, which alone uses them while it lives.
    unsafe fn memory(&self) -> HostMemory {
        // SAFETY: the caller's contract; `BYTES` bytes, allocated in `new` page-aligned.
        unsafe { HostMemory::new(self.0) }
    }
}

impl Drop for Page {
    fn drop(&mut self) {
        // SAFETY: allocated with this layout in `Page::new`.
        unsafe { std::alloc::dealloc(self.0.as_ptr(), Page::layout()) };
    }
}

/// The dispatch of `state` writing `output`, encoded into `command_buffer`.
fn dispatch(
    command_buffer: &CommandBuffer,
    state: &ComputePipelineState,
    input: &Buffer,
    output: &Buffer,
    scale: &Buffer,
) {
    let mut encoder = command_buffer.compute_command_encoder().unwrap();
    encoder.set_compute_pipeline_state(state).unwrap();
    encoder.set_buffer(Some(input), 0, 0).unwrap();
    encoder.set_buffer(Some(output), 0, 1).unwrap();
    encoder.set_buffer(Some(scale), 0, 2).unwrap();
    encoder
        .dispatch_threads(Size::new(COUNT, 1, 1), Size::new(COUNT, 1, 1))
        .unwrap();
    encoder.end_encoding().unwrap();
}

/// What one round saw.
#[derive(Debug, Default, PartialEq, Eq)]
struct Round {
    /// The dispatch wrote the pages the buffer was made over, which it left at the first detach:
    /// a backing two rebinds behind the submission, which no command of it named.
    dummy: bool,
    /// It wrote the pages it was encoded over, which the buffer left before the submission.
    old: bool,
    /// It wrote the pages the buffer lies in at the submission, which is the only right answer
    /// for a command buffer that completed with no error.
    new: bool,
    status: Option<CommandBufferStatus>,
    error: Option<CommandBufferError>,
    /// Over the rebind alone: the encoders it encoded again, and the splices it refused.
    reencoded: u64,
    refused: u64,
    /// Over the rebind alone: the sets 0 a command of the encoding again was handed from the
    /// command buffer's cache instead of allocating and writing its own. The condition: the
    /// cache's exactness rests on a hold the rebind let go of, so every one of these binds
    /// descriptors written for an import that is freed.
    found: u64,
}

/// One round of the sequence on `device`: the pages are the caller's, so it reads all three
/// ranges back whatever the rebinds passed. `None` where the device imports no host memory.
fn round(
    device: &Device,
    state: &ComputePipelineState,
    queue: &crate::CommandQueue,
    pages: &[Page; 3],
    (input, scale): (&Buffer, &Buffer),
) -> Option<Round> {
    let keep = || Some(Box::new(()) as KeepAlive);
    // SAFETY: each page outlives every buffer over it (the caller's, dropped after the device),
    // and only the buffer's commands use it while it lives.
    let made = unsafe {
        device.new_buffer_with_bytes_no_copy(
            pages[0].memory(),
            BYTES as u64,
            ResourceOptions::shared(),
            None,
        )
    };
    let output = match made {
        Ok(buffer) => buffer,
        Err(Error::Unsupported(gap)) => {
            eprintln!("REBIND-DESCRIPTOR-SLIP: the device imports no host memory: {gap:?}");
            return None;
        }
        Err(error) => panic!("new_buffer_with_bytes_no_copy: {error:?}"),
    };
    assert_eq!(output.detach_backing(), Ok(true), "the first detach");
    // SAFETY: as above.
    unsafe { output.rebind_bytes_no_copy(pages[1].memory(), None, keep()) }.unwrap();

    // The dispatch is encoded over the second page, and cannot start: its command buffer waits
    // for an event only the host signals.
    let gate = device.new_shared_event().unwrap();
    let command_buffer = queue.command_buffer().unwrap();
    command_buffer
        .encode_wait_for_event(EventRef::Shared(&gate), 1)
        .unwrap();
    dispatch(&command_buffer, state, input, &output, scale);
    command_buffer.commit().unwrap();

    assert_eq!(
        output.detach_backing(),
        Ok(true),
        "the detach of a buffer whose only use waits for an event"
    );
    // Around the rebind alone, which encodes the encoder again in this call: nothing of the
    // submission is in these.
    let before = device.work_counts();
    // SAFETY: as above.
    unsafe { output.rebind_bytes_no_copy(pages[2].memory(), None, keep()) }.unwrap();
    let after = device.work_counts();

    gate.set_signaled_value(1);
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    Some(Round {
        dummy: pages[0].written(),
        old: pages[1].written(),
        new: pages[2].written(),
        status: Some(command_buffer.status()),
        error: command_buffer.error(),
        reencoded: after.encoders_reencoded - before.encoders_reencoded,
        refused: after.reencodes_refused - before.reencodes_refused,
        found: after.set_zero_sets_found - before.set_zero_sets_found,
    })
}

/// The sequence [`ROUNDS`] times, each round over its own pages and its own buffer: no command of
/// an encoder encoded again binds a set 0 the command buffer kept, and no dispatch reaches other
/// pages than the buffer lies in at the submission.
#[test]
fn a_dispatch_encoded_again_binds_no_set_an_earlier_command_wrote() {
    let scratch = Scratch::new("rebind-descriptor-slip");
    let device: Device = device(&scratch.0);
    let state = state(&device);
    let queue = device.new_command_queue().unwrap();
    let bytes: Vec<u8> = (0..COUNT).flat_map(|i| (i as f32).to_le_bytes()).collect();
    let input = device
        .new_buffer_with_bytes(&bytes, ResourceOptions::shared())
        .unwrap();
    let scale = device
        .new_buffer_with_bytes(&SCALE.to_le_bytes(), ResourceOptions::shared())
        .unwrap();
    // Warm, over a buffer of the runtime's own, as the consumer's does.
    let warm = device
        .new_buffer(COUNT * 4, ResourceOptions::shared())
        .unwrap();
    let command_buffer = queue.command_buffer().unwrap();
    dispatch(&command_buffer, &state, &input, &warm, &scale);
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();

    let mut rounds = Vec::new();
    for at in 0..ROUNDS {
        // Dropped at the round's end: every round takes its own pages, as the consumer's fresh
        // process does.
        let pages = [Page::new(), Page::new(), Page::new()];
        let Some(seen) = round(&device, &state, &queue, &pages, (&input, &scale)) else {
            return;
        };
        println!("rebind-descriptor-slip round {at}: {seen:?}");
        rounds.push(seen);
    }
    let found: u64 = rounds.iter().map(|round| round.found).sum();
    let wrong: Vec<&Round> = (rounds.iter())
        .filter(|round| round.dummy || round.old || !round.new)
        .collect();
    println!(
        "rebind-descriptor-slip: {found} sets 0 bound again at a rebind, {} rounds reached other \
         pages than the buffer lies in",
        wrong.len(),
    );
    // The condition. A tree that has it meets it in about half its rounds, so this reads zero or
    // it does not, with no rate to weigh; a round that reaches the wrong pages is one of these
    // whose handle came to name something else, and far rarer.
    assert_eq!(
        found, 0,
        "a command of an encoder encoded again was handed {found} sets 0 the command buffer kept, \
         written for an import the rebind freed",
    );
    assert!(
        wrong.is_empty(),
        "the dispatch reached other pages than the buffer lies in, in {} of {ROUNDS} rounds: \
         {wrong:?}",
        wrong.len(),
    );
}
