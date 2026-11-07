//! A process-wide ring of the stages each command buffer passes from its commit to its completion,
//! for a host that asks where the time between them goes ([`read`]): when it was committed, left
//! to the lowering thread, run, recorded, handed to the scheduler and taken in, what held its
//! batches, when each was submitted, and when the completion waiter armed, saw it and finished it.
//! The scheduler's and the waiters' own sleeps and wakes are recorded under id 0 and the queue.
//!
//! - **Cheap.** A record is one atomic add and three stores; nothing allocates or locks. The ring
//!   keeps the newest [`RING`] records; a reader that falls behind loses the oldest.
//! - **One clock.** The host's uptime clock in nanoseconds (`CLOCK_UPTIME_RAW` on Apple
//!   platforms), the one the frontend's trace and the driver's rings use.
//! - **Off unless asked for.** Records are kept only where the host's environment has
//!   `SHENG_DIAG` ([`diagnostics`]), read once; the same switch keeps the allocation sites
//!   (`creation.rs`). Without it a record costs one relaxed load.
//! - **Ids.** A command buffer's id is the address of its shared state, alive from its commit to
//!   its completion: records of one id between a commit stage and its completion are one command
//!   buffer's.

use std::sync::atomic::{AtomicU64, AtomicU8, Ordering};

/// The records the ring keeps.
const RING: usize = 1 << 16;

/// What happened, the record's `stage`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u16)]
pub enum Stage {
    /// `commit` was called (`arg`: 0).
    CommitCall = 1,
    /// The commit was left to wait for the lowering thread's end of its last render encoder.
    CommitQueued = 2,
    /// The commit's own work began (`arg`: 0 at the call, 1 queued, 2 refused).
    CommitRun = 3,
    /// Its segments were recorded or prepared for their host steps (`arg`: segments).
    Prepared = 4,
    /// Its batches were handed to the scheduler (`arg`: batches).
    Handed = 5,
    /// The scheduler took its batches into its order (`arg`: batches).
    Taken = 6,
    /// A look of the scheduler's held one of its batches (`arg`: the reason, `HeldFor` + 1, |
    /// batch index << 8); recorded once per reason and batch.
    Held = 7,
    /// A batch's submission began (`arg`: batch index | last << 15).
    SubmitBegin = 8,
    /// A batch's submission returned (`arg`: 0 submitted, 1 grouped, 2 later, 3 after the group,
    /// 4 failed).
    SubmitEnd = 9,
    /// Its host step began recording on the scheduler's thread (`arg`: batch index).
    StepBegin = 10,
    /// Its host step ended (`arg`: batch index).
    StepEnd = 11,
    /// A completion waiter began waiting for its batch (`arg`: queue | slept first << 8).
    PollArmed = 12,
    /// The waiter's wait for its batch returned (`arg`: queue).
    PollDone = 13,
    /// A completion turn saw it complete (`arg`: queue).
    Seen = 14,
    /// Its completion ran (`arg`: 1 on the waiter, 2 posted to its queue's context).
    Finished = 15,
    /// A step inside a batch's submission ended (`arg`: 1 the scrubs and segments placed, 2 its
    /// waits and signals gathered, 3 the driver's `vkQueueSubmit` returned, 4 the submission's
    /// changes made).
    SubmitStep = 16,
    /// The scheduler's thread went to sleep (id 0, `arg`: slice in ms, | 0x8000 on the GPU).
    SchedulerSleep = 20,
    /// A memory operation began (`arg`: 1 foreground placement, 2 room keeper, 3 release).
    MemoryBegin = 40,
    /// Its pool (`arg`: memory type * 2 + 1 for images, + 0 for buffers).
    MemoryPool = 41,
    /// Its requested backing bytes in KiB.
    MemoryBytes = 42,
    /// Its pool's available contiguous range in KiB at the decision.
    MemoryAvailable = 43,
    /// Its Vulkan allocation ended (`arg`: 1 success, 0 refused).
    MemoryAllocated = 44,
    /// Existing backing was reused (`arg`: 1 pooled placement, 2 spare block).
    MemoryReused = 45,
    /// Its memory handle's low 32 bits.
    MemoryHandleLow = 46,
    /// Its memory handle's high 32 bits.
    MemoryHandleHigh = 47,
    /// Its backing was retained for reuse (`arg`: total spare bytes in KiB).
    MemoryCached = 48,
    /// Its physical release was posted to the device's free thread (`arg`: bytes in KiB).
    MemoryDeferred = 49,
    /// Its physical release began.
    MemoryFreeBegin = 50,
    /// Its physical release ended.
    MemoryFreeEnd = 51,
    /// A release ran inline because the bounded queue was full.
    MemoryQueueFull = 52,
    /// Completion processing began.
    CompleteBegin = 53,
    /// Completion state and resource ownership were collected.
    CompleteState = 54,
    /// User completion handlers returned.
    CompleteHandlers = 55,
    /// Retained objects and runtime resources were released.
    CompleteRetained = 56,
    /// Completion gate and command-pool preparation ended.
    CompleteEnd = 57,

    /// Something woke the scheduler's thread (id 0, `arg`: 0 commit, 1 completion, 2 host).
    SchedulerWake = 21,
    /// The scheduler's thread resumed after its sleep (id 0, `arg`: 1 timed out).
    SchedulerResumed = 22,
    /// The scheduler's thread began a pass (id 0).
    PassBegin = 23,
    /// The scheduler's thread ended a pass (id 0, `arg`: batches submitted, capped).
    PassEnd = 24,
}

struct Slot {
    when: AtomicU64,
    id: AtomicU64,
    /// stage | arg << 16 | sequence << 32: a reader takes the slot only where the sequence is the
    /// one it asked for.
    word: AtomicU64,
}

#[allow(clippy::declare_interior_mutable_const)]
const EMPTY: Slot = Slot {
    when: AtomicU64::new(0),
    id: AtomicU64::new(0),
    word: AtomicU64::new(0),
};
static SLOTS: [Slot; RING] = [EMPTY; RING];
static NEXT: AtomicU64 = AtomicU64::new(1);

/// The host's uptime clock in nanoseconds.
pub fn now_nanos() -> u64 {
    #[cfg(any(target_os = "macos", target_os = "ios"))]
    {
        let mut spec = libc::timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        // SAFETY: a valid clock id and an owned timespec.
        if unsafe { libc::clock_gettime(libc::CLOCK_UPTIME_RAW, &mut spec) } == 0 {
            return spec.tv_sec as u64 * 1_000_000_000 + spec.tv_nsec as u64;
        }
        0
    }
    #[cfg(not(any(target_os = "macos", target_os = "ios")))]
    {
        crate::gpu_time::host_nanos()
    }
}

/// Whether the host asked for the runtime's diagnostics: `SHENG_DIAG` in the environment, read
/// once.
pub(crate) fn diagnostics() -> bool {
    // 0 not read yet, 1 off, 2 on.
    static ON: AtomicU8 = AtomicU8::new(0);
    match ON.load(Ordering::Relaxed) {
        0 => {
            let on = std::env::var_os("SHENG_DIAG").is_some();
            ON.store(if on { 2 } else { 1 }, Ordering::Relaxed);
            on
        }
        state => state == 2,
    }
}

/// Record `stage` of `id` with `arg`, now, where the diagnostics are on ([`diagnostics`]).
pub(crate) fn mark(id: usize, stage: Stage, arg: u32) {
    // Per-command handoff and completion stages retain scheduler latency.
    // Idle scheduler turns have no command identity and add no causal link.
    if matches!(stage, Stage::SchedulerSleep | Stage::SchedulerWake
        | Stage::SchedulerResumed | Stage::PassBegin | Stage::PassEnd) {
        return;
    }
    if !diagnostics() {
        return;
    }
    if let Some(hook) = DIAGNOSTIC_HOOK.get() {
        hook(39, id as u64, stage as u64, arg as u64);
    }
    let sequence = NEXT.fetch_add(1, Ordering::Relaxed);
    let slot = &SLOTS[(sequence as usize) & (RING - 1)];
    slot.word.store(0, Ordering::Release);
    slot.when.store(now_nanos(), Ordering::Relaxed);
    slot.id.store(id as u64, Ordering::Relaxed);
    let word = stage as u64 | ((arg as u64 & 0xffff) << 16) | ((sequence & 0xffff_ffff) << 32);
    slot.word.store(word, Ordering::Release);
}

/// One record as [`read`] gives it.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct StageRecord {
    /// Host uptime nanoseconds.
    pub when: u64,
    /// The command buffer's id (0: the scheduler's thread).
    pub id: u64,
    /// Its place among all records.
    pub sequence: u32,
    /// [`Stage`] as its number.
    pub stage: u16,
    /// The stage's argument.
    pub arg: u16,
}

/// The records from sequence `from` on (the oldest kept where `from` fell out of the ring), at most
/// `out.len()`, oldest first: how many, and the sequence to ask for next.
pub fn read(from: u64, out: &mut [StageRecord]) -> (usize, u64) {
    let newest = NEXT.load(Ordering::Acquire);
    let mut sequence = from.max(1).max(newest.saturating_sub(RING as u64 - 1024));
    let mut written = 0;
    while sequence < newest && written < out.len() {
        let slot = &SLOTS[(sequence as usize) & (RING - 1)];
        let word = slot.word.load(Ordering::Acquire);
        if word >> 32 == sequence & 0xffff_ffff && word & 0xffff != 0 {
            let when = slot.when.load(Ordering::Relaxed);
            let id = slot.id.load(Ordering::Relaxed);
            if slot.word.load(Ordering::Acquire) == word {
                out[written] = StageRecord {
                    when,
                    id,
                    sequence: sequence as u32,
                    stage: (word & 0xffff) as u16,
                    arg: ((word >> 16) & 0xffff) as u16,
                };
                written += 1;
            }
        } else if word == 0 {
            // Being written: read again at the next call.
            break;
        }
        sequence += 1;
    }
    (written, sequence)
}

/// Optional host hook. The installed function must remain loaded for the process lifetime.
pub type DiagnosticHook = extern "C" fn(u32, u64, u64, u64);
static DIAGNOSTIC_HOOK: std::sync::OnceLock<DiagnosticHook> = std::sync::OnceLock::new();
/// Install once before creating command queues. It never changes rendering state.
pub fn set_diagnostic_hook(hook: DiagnosticHook) {
    let _ = DIAGNOSTIC_HOOK.set(hook);
}
/// Link a command buffer to its Vulkan work, including batches later grouped by the scheduler.
pub(crate) fn diagnostic_link(id: u64, command: u64, batch: u64) {
    if let Some(hook) = DIAGNOSTIC_HOOK.get() {
        hook(45, id, command, batch);
    }
}
