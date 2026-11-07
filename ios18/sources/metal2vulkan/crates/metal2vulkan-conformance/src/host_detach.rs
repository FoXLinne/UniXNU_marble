//! A detach of a no-copy buffer's host pages made from the command buffer's own completed
//! handler: the vocabulary for the one place a consumer asks for it and no other family here
//! reaches.
//!
//! A program that moves the pages under a buffer lets go of the old ones before it has the new,
//! and the moment it learns the GPU is finished with them is a completed handler of the command
//! buffer that used them. So that handler is where `detachBacking` is called in production, and it
//! is the shape this vocabulary states. Metal's call is private SPI, reached by selector and only
//! behind `respondsToSelector:` on the Metal leg.
//!
//! # Why the handler waits first, and what that buys
//!
//! Metal releases what a command buffer used as it processes the completion, and that release
//! **races** the start of the completed handlers: a detach made in the first microseconds of a
//! handler answers `false` or `true` depending on which won. Measured on this M3 Max under
//! macOS 27.0.1 over 2672 runs, the `false` rate of such a detach moved between 1 and 43 in 50 as
//! unrelated host load changed, and timed from `GPUEndTime` the two answers overlap between 50 and
//! 99 us. There is no answer to compare there, and a case that asked would record whichever side
//! of the race its own run happened to see.
//!
//! Once the use has retired Metal's answer is deterministic: it detached 684 of 684 times at 20 us
//! or more into the handler, and 152 of 152 at 100 us or more with 32 command buffers queued. So
//! the handler waits [`AFTER`] before it calls, which is a thousand times that bound, and the
//! answer in that region is what the case compares. The race itself is stated in the runtime's own
//! `detach.rs` and is pinned by nothing.
//!
//! # Why it cannot hang and holds no GPU
//!
//! The handler sleeps [`AFTER`] and returns, so it leaves its leg's handler thread whatever this
//! thread does. Neither leg calls `waitUntilCompleted`, which Metal has no bounded form of: this
//! thread waits on the handler's own channel with a bound ([`WAIT`]), and an answer that did not
//! arrive inside it is recorded as the negative fact rather than waited for again. The GPU work is
//! one blit fill of the buffer the case already made: no pass, no dispatch, no loop.

use crate::event_orders::Status;
use crate::transcript::{Negative, Node, NodeName, Observed, Value};
use std::time::Duration;

/// How long the completed handler waits before it detaches.
///
/// An input of the case, because it is what the compared answer means: "once the use has retired",
/// and 20 ms is a thousand times the 20 us past which Metal's answer stopped moving.
pub const AFTER: Duration = Duration::from_millis(20);

/// How many bytes of the buffer the command buffer's blit fills, at most.
///
/// The fill is there to make the buffer something a submission **named**, which is the only thing
/// that can hold it; nothing reads the bytes. Stated here rather than per side, so both legs
/// submit the same work.
pub const FILL_BYTES: u64 = 64;

/// The byte the fill writes. Stated for the same reason, and read by nothing.
pub const FILL_BYTE: u8 = 0x5a;

/// The longest either leg waits on its handler's channel for the detach's answer.
///
/// A bound rather than a measurement: the case records an answer that did not arrive inside it as
/// the negative fact, so neither leg can leave this thread waiting for work that will not come.
pub const WAIT: Duration = Duration::from_secs(30);

/// What a detach made in a command buffer's own completed handler observed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DetachedInHandler {
    /// What the detach answered from inside the handler, [`AFTER`] into it: `true` where the
    /// buffer was detached, `false` where the call refused because something still held it, and
    /// `None` where the handler never ran.
    pub detached: Option<bool>,
    /// The status the command buffer read once the handler's answer arrived, or that the answer
    /// never came.
    ///
    /// Recorded beside the answer, and not for its own sake: a `true` from a command buffer that
    /// never ran its work would be a detach of a buffer nothing had taken, which is a different
    /// question with the same answer. Deterministic on both legs, because a command buffer's
    /// status reads final before its first completed handler starts (Metal's within 10 us of it,
    /// measured on this M3 Max).
    pub status: Status,
}

impl DetachedInHandler {
    /// The observations as the sites a case emits under one node, so both legs emit one shape.
    pub fn node(&self, name: impl Into<NodeName>) -> Node {
        let detached = match self.detached {
            Some(answered) => Value::Bool(answered),
            None => Value::Nothing(Negative::NeverReturned),
        };
        Node::branch(name, Vec::new())
            .with(Node::leaf(
                "detach-backing/in-a-completed-handler-once-the-use-retired",
                Observed::value(detached),
            ))
            .with(Node::leaf(
                "command-buffer/status-when-the-handler-answered",
                Observed::value(self.status.value()),
            ))
    }
}
