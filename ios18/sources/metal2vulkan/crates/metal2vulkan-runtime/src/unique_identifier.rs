//! `uniqueIdentifier`, Apple's private property of pipeline states and indirect command buffers,
//! and the identifier an indirect command's `getPipelineStateUniqueIdentifier` answers.
//!
//! Metal's identifiers are addresses of its own allocations: a pipeline state's measured by the
//! consumer on an M3 Max under macOS 26.7, an indirect command buffer's by our probes
//! (an M4 under 27.0.1 and the M3 Max):
//!
//! - **A state made without `supportIndirectCommandBuffers`** answers an address Metal allocates
//!   as any memory, which differs from run to run. What a program can rely on is how they relate:
//!   each state answers one nonzero value for its life, a render and a compute state different
//!   ones, and a second state made from an equal descriptor a new one. The runtime answers a
//!   number of one process-wide count from [`PLAIN_FIRST`], above every value below.
//! - **A state made with it** answers `2^36` ([`INDIRECT_BASE`]) plus an address in Metal's own
//!   allocation of such states, the same in every run that makes and releases the same states in
//!   the same order, which it reuses once a state goes: a compute state made after one was
//!   released answers the released one's, the last released first, and a render state takes
//!   another kind of allocation. The steps are the function's (one kernel's states `0xc0` apart,
//!   the first of another `0x300` on, render states `0x200` apart) and the base what the process
//!   made before and how it was started (`0x100` apart between a test binary and the same binary
//!   from a shell). The runtime keeps the rule, the last released taken again first, at a step
//!   of the smallest measured (`0xc0` compute, `0x200` render); a compute state's from one step
//!   into the range ([`COMPUTE_FIRST`]; it once started from `0x280`, the first such compute state
//!   of one test binary on an M3 Max under macOS 26.7, which the same binary started from a shell
//!   answered `0x100` past, so the base matched one workload, not a rule), up to the indirect
//!   command buffers' range, and past their range once that is full; a render state's past them
//!   (where Metal's lie is not measured). No state's offset is Metal's: Metal's follow its
//!   allocator, its classes by function and the process's history.
//! - **An indirect command buffer** answers `2^36` plus its private `gpuAddress`, in a range of
//!   Metal's for them: the lowest gap from `0x18000` that holds its `allocatedSize` and one 16-KiB
//!   page more, at a multiple of 32 KiB (`0x18000`, `0x20000`, `0x28000` for buffers of four
//!   commands), free again as the buffer goes and no command buffer that executes it may run
//!   (the command buffer holds it until it completed). The runtime answers that rule.
//!
//! **Metal's own encoders** take pieces of that range, each once a device, kept for its life, at
//! the lowest gap that holds it, as a buffer's range is (an M3 Max, macOS 27.0.1, each sequence
//! in a process of its own, alike in three rounds): a blit encoder 256 KiB at its opening (an
//! empty one, one never committed and one made while a buffer is made inside it alike), of which
//! 192 KiB a render encoder's opening takes too, and that 384 KiB in all; and the first command
//! buffer scheduled with a compute encoder's dispatch or execution, or a blit encoder's command (a
//! copy, a fill, a reset or copy of commands, a texture copy, a fence update; not a
//! `synchronizeResource`), 32 KiB as it is scheduled, after its waits (one that waits for an event
//! not yet signaled has taken nothing; one past `waitUntilScheduled` has). A compute encoder takes
//! nothing at its opening, nor one that is empty or only makes a buffer resident. Where the
//! pieces' order inside one opening is not told apart by a measured gap, the runtime takes the
//! shared piece first. The runtime takes them so ([`Piece`]). A render command buffer's run takes
//! more, by its targets' sizes and what the device drew before (a buffer made after a draw into a
//! 64-pixel target answers `0x4e0000` from the base, a 1024-pixel one `0x508000`, a 4096-pixel one
//! `0x17c8000`, and after one of 16 pixels then one of 4096 `0x4d8000`, below the 16-pixel one's
//! alone): the runtime does not model it, nor a resource state encoder's pieces (not measured).
//!
//! **Which answers are Metal's**. An indirect command buffer made before a command buffer of its
//! device that encoded a render encoder was scheduled, and before a resource state encoder
//! opened, answers Metal's number (the rules above). No state's answer is measured to be Metal's.
//! The first answer of a program's call that is not Metal's number
//! (`ComputePipelineState::unique_identifier`, `RenderPipelineState::unique_identifier`,
//! `IndirectCommandBuffer::unique_identifier`, an indirect command's state identifier but 0)
//! announces, once a device, one of two kinds by how Metal's own answer stands: a plain state's,
//! which differs from run to run on Metal, `plain-state-unique-identifier`
//! ([`Degradation::PlainStateUniqueIdentifier`], one of Metal's answers); a state's with indirect
//! command buffer support, or a buffer's made past the model, which Metal answers alike in every
//! run of one program started alike, `unmeasured-unique-identifier`
//! ([`Degradation::UnmeasuredUniqueIdentifier`], a near-equivalent). The allocations are the
//! device's: Metal's measured space is one process's one device, which a device of the runtime
//! stands for.
//!
//! No run reads an identifier: a translated function never sees one.

use std::collections::{BTreeMap, BTreeSet};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use crate::degradation::Degradation;
use crate::device::lock;
use crate::error::{Error, MemoryKind};
use crate::resource_ids::StateKind;
use crate::used_resources::EncoderKind;
use crate::Device;

/// The base of Metal's identifiers of indirect command buffers and of states made with
/// `supportIndirectCommandBuffers`: `2^36`.
pub(crate) const INDIRECT_BASE: u64 = 1 << 36;

/// The first identifier of a state made without `supportIndirectCommandBuffers`: past every
/// identifier from [`INDIRECT_BASE`].
pub(crate) const PLAIN_FIRST: u64 = 1 << 37;

/// Where the first indirect command buffer lies in Metal's range for them (measured: a fresh
/// process's first buffer answers `0x1000018000`).
pub(crate) const BUFFER_FIRST: u64 = 0x1_8000;

/// Metal's page: a buffer's range holds one more past its `allocatedSize`.
const BUFFER_PAGE: u64 = 0x4000;

/// The alignment of a buffer's range.
const BUFFER_SLOT: u64 = 0x8000;

/// One of Metal's own allocations in the buffers' range, which its encoders take (an M3 Max,
/// macOS 27.0.1; the module docs): each once a device, kept for its life, at the lowest gap that
/// holds it, as a buffer's range is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Piece {
    /// The first blit or render encoder's, at its opening: 192 KiB.
    Shared,
    /// The first blit encoder's own, after [`Piece::Shared`]: 64 KiB.
    Blit,
    /// The first render encoder's own at its opening, after [`Piece::Shared`]: 192 KiB. Its
    /// command buffer's run takes more, by its targets' sizes and what the device drew before,
    /// which the runtime does not model ([`UniqueIdentifiers::scheduled`]).
    Render,
    /// The first command buffer scheduled with a compute encoder's dispatch or execution or a
    /// blit encoder's command, as it is scheduled: 32 KiB.
    Dispatch,
}

impl Piece {
    /// Its footprint in the range, as measured.
    fn footprint(self) -> u64 {
        match self {
            Piece::Shared | Piece::Render => 0x3_0000,
            Piece::Blit => 0x1_0000,
            Piece::Dispatch => 0x8000,
        }
    }
}

/// What a command buffer's encoders did that Metal's identifier allocations follow as it is
/// scheduled ([`UniqueIdentifiers::scheduled`]).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ScheduledWork {
    /// A compute encoder dispatched or executed commands, or a blit encoder encoded a command.
    pub(crate) dispatch: bool,
    /// A render encoder was encoded.
    pub(crate) render: bool,
}

/// The end of the buffers' range, from [`INDIRECT_BASE`], where the compute states' starts.
pub(crate) const BUFFERS_END: u64 = 1 << 34;

/// Where the first compute state with indirect command buffer support lies: one step into the
/// range, which no rule of Metal's places (the module docs). The `0x280` was one test binary's
/// first on an M3 Max, `0x100` below the same binary's from a shell.
pub(crate) const COMPUTE_FIRST: u64 = COMPUTE_STEP;

/// The compute states' step: the smallest measured.
pub(crate) const COMPUTE_STEP: u64 = 0xc0;

/// The render states' step, as measured.
pub(crate) const RENDER_STEP: u64 = 0x200;

/// The next plain identifier, of every device.
static PLAIN: AtomicU64 = AtomicU64::new(PLAIN_FIRST);

/// One kind of state's identifiers: in steps of `step` through `ranges` in order (each from its
/// first below its end), those given back taken again, the last first.
#[derive(Debug)]
struct Slots {
    step: u64,
    ranges: &'static [(u64, u64)],
    /// The range `next` lies in.
    range: usize,
    next: u64,
    free: Vec<u64>,
}

impl Slots {
    fn new(ranges: &'static [(u64, u64)], step: u64) -> Slots {
        Slots {
            step,
            ranges,
            range: 0,
            next: ranges.first().map_or(0, |&(first, _)| first),
            free: Vec::new(),
        }
    }

    fn take(&mut self) -> Option<u64> {
        if let Some(offset) = self.free.pop() {
            return Some(offset);
        }
        loop {
            let &(_, end) = self.ranges.get(self.range)?;
            if let Some(next) = self.next.checked_add(self.step).filter(|&next| next <= end) {
                let offset = self.next;
                self.next = next;
                return Some(offset);
            }
            self.range += 1;
            self.next = self.ranges.get(self.range)?.0;
        }
    }
}

/// The compute states' ranges: from Metal's first below the buffers' range, then past it.
const COMPUTE_RANGES: &[(u64, u64)] = &[
    (COMPUTE_FIRST, BUFFER_FIRST),
    (BUFFERS_END, 2 * BUFFERS_END),
];

/// The render states' range, past the compute states'.
const RENDER_RANGES: &[(u64, u64)] = &[(2 * BUFFERS_END, 4 * BUFFERS_END)];

/// One device's allocations of the identifiers from [`INDIRECT_BASE`], as offsets from it.
#[derive(Debug)]
struct Allocations {
    compute: Slots,
    render: Slots,
    /// The live indirect command buffers' ranges and Metal's own taken pieces' ([`Piece`]):
    /// start, then bytes.
    buffers: BTreeMap<u64, u64>,
    /// The pieces taken.
    pieces: BTreeSet<Piece>,
}

/// One device's identifiers: its allocations, and whether Metal's own allocations in the
/// buffers' range are past what the runtime models (the module docs).
#[derive(Debug)]
pub(crate) struct UniqueIdentifiers {
    allocations: Mutex<Allocations>,
    unmeasured: AtomicBool,
}

impl Default for UniqueIdentifiers {
    fn default() -> UniqueIdentifiers {
        UniqueIdentifiers {
            allocations: Mutex::new(Allocations {
                compute: Slots::new(COMPUTE_RANGES, COMPUTE_STEP),
                render: Slots::new(RENDER_RANGES, RENDER_STEP),
                buffers: BTreeMap::new(),
                pieces: BTreeSet::new(),
            }),
            unmeasured: AtomicBool::new(false),
        }
    }
}

impl UniqueIdentifiers {
    /// The device opened a command encoder of `kind`: the pieces Metal's takes at its opening
    /// (a blit encoder's [`Piece::Shared`] and [`Piece::Blit`], a render encoder's
    /// [`Piece::Shared`] and [`Piece::Render`], a compute encoder's none). A resource state
    /// encoder's are not measured: an indirect command buffer made from then on answers no
    /// number measured to be Metal's.
    pub(crate) fn encoder_opened(&self, kind: EncoderKind) {
        match kind {
            EncoderKind::Blit => self.take(&[Piece::Shared, Piece::Blit]),
            EncoderKind::Render => self.take(&[Piece::Shared, Piece::Render]),
            EncoderKind::Compute => {}
            EncoderKind::ResourceState => self.unmodelled(),
        }
    }

    /// A command buffer of the device that did `work` is scheduled, after its waits: the
    /// [`Piece::Dispatch`] Metal's dispatch or blit work takes then; and where it encoded a render
    /// encoder, what Metal's run of it takes, which follows its targets' sizes and what the
    /// device drew before (an M3 Max: a buffer made after a draw into a 64-pixel target answers
    /// `0x4e0000` from the base, into a 1024-pixel one `0x508000`, a 4096-pixel one `0x17c8000`,
    /// and after one of 16 pixels then one of 4096 `0x4d8000`), which the runtime does not model:
    /// a buffer made from then on answers no number measured to be Metal's.
    pub(crate) fn scheduled(&self, work: ScheduledWork) {
        if work.dispatch {
            self.take(&[Piece::Dispatch]);
        }
        if work.render {
            self.unmodelled();
        }
    }

    /// Take `pieces` not yet taken, in order; one no gap holds leaves Metal's allocations past
    /// the model.
    fn take(&self, pieces: &[Piece]) {
        let mut allocations = lock(&self.allocations);
        for &piece in pieces {
            if allocations.pieces.insert(piece)
                && allocations.take_range(piece.footprint()).is_none()
            {
                self.unmodelled();
            }
        }
    }

    /// Metal's own allocations are past what the runtime models from now on.
    fn unmodelled(&self) {
        // Relaxed: only the flag's value matters, read by the buffers made after the program's
        // call that set it.
        self.unmeasured.store(true, Ordering::Relaxed);
    }
}

/// What an identifier from [`INDIRECT_BASE`] was allocated as.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Allocation {
    State(StateKind),
    Buffer,
}

impl Allocations {
    fn slots(&mut self, kind: StateKind) -> &mut Slots {
        match kind {
            StateKind::Compute => &mut self.compute,
            StateKind::Render => &mut self.render,
        }
    }

    /// The offset of a new buffer of Metal's `allocated_size`: the lowest gap from
    /// [`BUFFER_FIRST`] that holds it and one page more, at a multiple of the slot.
    fn take_buffer(&mut self, allocated_size: u64) -> Option<u64> {
        let footprint = allocated_size
            .checked_add(BUFFER_PAGE)?
            .checked_next_multiple_of(BUFFER_SLOT)?;
        self.take_range(footprint)
    }

    /// The offset of the lowest gap from [`BUFFER_FIRST`] that holds `footprint` bytes (a
    /// multiple of the slot), taken; none where the range has none.
    fn take_range(&mut self, footprint: u64) -> Option<u64> {
        let mut offset = BUFFER_FIRST;
        for (&start, &bytes) in &self.buffers {
            if start >= offset.checked_add(footprint)? {
                break;
            }
            offset = offset.max(start + bytes);
        }
        if offset.checked_add(footprint)? > BUFFERS_END {
            return None;
        }
        self.buffers.insert(offset, footprint);
        Some(offset)
    }

    fn give(&mut self, allocation: Allocation, offset: u64) {
        match allocation {
            Allocation::State(kind) => self.slots(kind).free.push(offset),
            Allocation::Buffer => {
                self.buffers.remove(&offset);
            }
        }
    }
}

/// A `uniqueIdentifier`, held while its state or buffer lives: one from [`INDIRECT_BASE`] goes
/// back to its device's allocation as the holder goes.
#[derive(Debug)]
pub(crate) struct UniqueIdentifier {
    value: u64,
    /// The kind its answer announces where it is not measured to be Metal's number (the module
    /// docs); none where it is.
    degradation: Option<Degradation>,
    back: Option<(Arc<UniqueIdentifiers>, Allocation)>,
}

impl UniqueIdentifier {
    /// The identifier of a new pipeline state of `kind` on the device of `ids`: from the kind's
    /// allocation where it supports indirect command buffers (`indirect`), else plain. Refused
    /// `OutOfMemory` where the kind's range is full of live states.
    pub(crate) fn state(
        ids: &Arc<UniqueIdentifiers>,
        kind: StateKind,
        indirect: bool,
    ) -> Result<UniqueIdentifier, Error> {
        if !indirect {
            return Ok(UniqueIdentifier {
                value: PLAIN.fetch_add(1, Ordering::Relaxed),
                degradation: Some(Degradation::PlainStateUniqueIdentifier),
                back: None,
            });
        }
        let offset = lock(&ids.allocations)
            .slots(kind)
            .take()
            .ok_or(Error::OutOfMemory(MemoryKind::Device))?;
        Ok(UniqueIdentifier {
            value: INDIRECT_BASE + offset,
            degradation: Some(Degradation::UnmeasuredUniqueIdentifier),
            back: Some((Arc::clone(ids), Allocation::State(kind))),
        })
    }

    /// The identifier of a new indirect command buffer of Metal's `allocated_size` on the device
    /// of `ids`. Refused `OutOfMemory` where no gap of the buffers' range holds it.
    pub(crate) fn buffer(
        ids: &Arc<UniqueIdentifiers>,
        allocated_size: u64,
    ) -> Result<UniqueIdentifier, Error> {
        let offset = lock(&ids.allocations)
            .take_buffer(allocated_size)
            .ok_or(Error::OutOfMemory(MemoryKind::Device))?;
        let unmeasured = ids.unmeasured.load(Ordering::Relaxed);
        Ok(UniqueIdentifier {
            value: INDIRECT_BASE + offset,
            degradation: unmeasured.then_some(Degradation::UnmeasuredUniqueIdentifier),
            back: Some((Arc::clone(ids), Allocation::Buffer)),
        })
    }

    /// Its value.
    pub(crate) fn value(&self) -> u64 {
        self.value
    }

    /// The kind its answer announces where it is not measured to be Metal's number.
    pub(crate) fn degradation(&self) -> Option<Degradation> {
        self.degradation
    }

    /// Its value, as a program's call on `device` answers it: announced where it is not measured
    /// to be Metal's number ([`UniqueIdentifier::degradation`]), `object` naming the holder.
    pub(crate) fn answer(&self, device: &Device, object: &str) -> u64 {
        if let Some(kind) = self.degradation() {
            degrade_identifier(device, kind, object);
        }
        self.value
    }
}

/// The kind an indirect command's answer of a state's identifier `value` announces: none for 0
/// (it names no state, Metal's answer); a plain state's at [`PLAIN_FIRST`] and past it; any other
/// state's below it.
pub(crate) fn state_value_degradation(value: u64) -> Option<Degradation> {
    match value {
        0 => None,
        value if value >= PLAIN_FIRST => Some(Degradation::PlainStateUniqueIdentifier),
        _ => Some(Degradation::UnmeasuredUniqueIdentifier),
    }
}

/// Announce `kind`, once a device, for a program's call that answered an identifier of `object`
/// not measured to be Metal's number (the module docs).
pub(crate) fn degrade_identifier(device: &Device, kind: Degradation, object: &str) {
    device.degrade(kind, || {
        let reason = match kind {
            Degradation::PlainStateUniqueIdentifier => {
                "one value of the runtime's count, unique and fixed for the state's life, where \
                 Metal's is an address that differs from run to run"
            }
            _ => {
                "unique and fixed for its holder's life, and reused as Metal reuses it, but not \
                 the number Metal answers there, which follows how the process was started, \
                 what it made and Metal's own encoders' allocations"
            }
        };
        (
            format!("the uniqueIdentifier of {object}"),
            reason.to_owned(),
        )
    });
}

impl Drop for UniqueIdentifier {
    fn drop(&mut self) {
        if let Some((ids, allocation)) = &self.back {
            lock(&ids.allocations).give(*allocation, self.value - INDIRECT_BASE);
        }
    }
}
