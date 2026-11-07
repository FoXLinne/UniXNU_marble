//! A detach of a no-copy buffer's host pages, made from the completed handler of the command
//! buffer that used them.
//!
//! This is where a program that moves the pages under a buffer calls `detachBacking`: the handler
//! is the moment it learns the GPU is finished with the old pages. Metal detaches there, and a
//! refusal there is a refusal where Metal allows — a consumer's user lost a session to one, their
//! next replace raising a backing-refused error whenever the detach landed inside a slow handler.
//!
//! # What this case compares
//!
//! Two leaves ([`crate::host_detach::DetachedInHandler`]): what the detach answered from inside the
//! handler, and the status the command buffer read when that answer arrived. The second is there so
//! a `true` from a command buffer that never ran its work cannot read as the answer this case is
//! about.
//!
//! # The handler waits, and that is the whole reason the case is deterministic
//!
//! Metal releases what a command buffer used as it processes the completion, and that release races
//! the start of the completed handlers. Measured on this M3 Max under macOS 27.0.1 over 2672 runs,
//! a detach made in the first microseconds of a handler answered `false` between 1 and 43 times in
//! 50 as unrelated host load changed, with the two answers overlapping between 50 and 99 us after
//! `GPUEndTime`; every `false` was transient, all 467 of them detaching on a retry 2 us later. So
//! there is no answer there to compare, and this case does not ask for one.
//!
//! Past the retire Metal's answer stops moving: 684 of 684 detached at 20 us or more into the
//! handler, and 152 of 152 at 100 us or more with 32 command buffers queued. The handler here waits
//! [`AFTER`] — 20 ms, a thousand times that bound — so the answer it reports is the one Metal gives
//! wherever Metal's answer is a rule rather than a race.
//!
//! # Why the buffer is used first
//!
//! A detach refuses only while submitted work **holds** the buffer, and only a submission that
//! named it holds it. So the command buffer blit-fills the buffer
//! ([`crate::host_detach::FILL_BYTES`] bytes) before its handler detaches: without
//! that, the case would be asking about a buffer nothing had taken, which is a different question
//! with the same answer.
//!
//! # Why the default `record_runs` is enough
//!
//! Both leaves are deterministic in the region the case asks in, on both legs: the answer is
//! Metal's rule past the retire, and a command buffer's status reads final before its first
//! completed handler starts. So the recording's runs fold into one outcome, and they did. A case
//! whose answer moves asks for more runs and records the outcome **set** instead — the race this
//! case is written to stay out of is exactly what that is for.
//!
//! # Nothing here can fault, hang or end a process
//!
//! One blit fill of 64 bytes, one command buffer, one handler that sleeps 20 ms and returns. No
//! pass, no dispatch, no loop. Neither leg calls `waitUntilCompleted`, which Metal has no bounded
//! form of: this thread waits on the handler's own channel with a bound, and an answer that did not
//! arrive is recorded as the negative fact.

use crate::case::{made, Abort, Case, Input, MetalSource, Side};
use crate::host_detach::{AFTER, FILL_BYTE, FILL_BYTES};
use crate::host_pages::{HostBufferSpec, PAGE_MULTIPLE};
use crate::transcript::{Node, Observed, Sites};

/// The case's name.
pub const DETACH_IN_A_COMPLETED_HANDLER: &str = "detach-in-a-completed-handler";

/// The site the buffer's making sits at.
const BUFFER: &str = "host-buffer";

/// The site the detach's answers sit under.
const AT: &str = "detach";

/// The host memory the case hands the device: one page of the largest page in play, which is the
/// smallest a [`HostBufferSpec`] may be, so every venue is handed the same bytes.
///
/// Writable, not read-only: this case is about the detach, and the read-only bit is another
/// family's subject.
const SPEC: HostBufferSpec = HostBufferSpec::writable(PAGE_MULTIPLE, 0xab);

/// A case over a detach made from a command buffer's own completed handler.
pub struct DetachInHandler {
    name: &'static str,
}

/// The case.
pub fn detach_in_a_completed_handler() -> DetachInHandler {
    DetachInHandler {
        name: DETACH_IN_A_COMPLETED_HANDLER,
    }
}

impl Case for DetachInHandler {
    fn name(&self) -> &'static str {
        self.name
    }

    fn metal_source(&self) -> MetalSource {
        MetalSource::Run
    }

    fn inputs(&self) -> Result<Vec<Input>, String> {
        Ok(vec![
            Input::bytes("host-buffer", &SPEC.words()),
            // The fill is what makes the buffer something a submission named.
            Input::bytes("fill-bytes", &FILL_BYTES.to_le_bytes()),
            Input::bytes("fill-byte", &[FILL_BYTE]),
            // The wait is an input because it is what the compared answer means: "once the use has
            // retired", rather than inside the race at a handler's start.
            Input::bytes(
                "handler-wait-millis",
                &(AFTER.as_millis() as u64).to_le_bytes(),
            ),
        ])
    }

    fn run(&self, side: &mut dyn Side, sites: &mut Sites) -> Result<(), Abort> {
        let buffer = made(sites, BUFFER, side.host_buffer(&SPEC))?;
        match side.detach_in_a_completed_handler(buffer, AFTER) {
            Ok(detached) => sites.push(detached.node(AT)),
            // The whole experiment is the one call, so there is nothing after it to read.
            Err(failure) => sites.push(Node::leaf(AT, Observed::value(failure.recorded()))),
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::host_detach::DetachedInHandler;

    /// The case's inputs name the buffer, the fill and the wait, and the wait is the one past the
    /// race: a case stating a shorter one would be recording whichever side of Metal's race its
    /// own run saw.
    #[test]
    fn the_case_states_its_buffer_and_the_wait_past_the_race() {
        let case = detach_in_a_completed_handler();
        let inputs = case.inputs().expect("the inputs are stated");
        let named: Vec<&str> = inputs.iter().map(|input| input.name.as_str()).collect();
        assert_eq!(
            named,
            vec![
                "host-buffer",
                "fill-bytes",
                "fill-byte",
                "handler-wait-millis"
            ]
        );
        assert!(
            AFTER >= std::time::Duration::from_millis(1),
            "a wait of {AFTER:?} is inside the race Metal's answer moves in, where the case has \
             nothing to compare"
        );
        const {
            assert!(
                !SPEC.read_only,
                "the read-only bit is another family's subject"
            )
        };
        assert_eq!(SPEC.unmappable(), None, "every venue can map the spec");
    }

    /// Both leaves are present whatever the detach answered, and the answer the case exists for
    /// reads as its own value rather than as an absence.
    #[test]
    fn both_leaves_are_present_for_either_answer() {
        use crate::event_orders::Status;
        use crate::transcript::Value;
        for detached in [Some(true), Some(false), None] {
            let observed = DetachedInHandler {
                detached,
                status: Status::read("Completed"),
            };
            let node = observed.node(AT);
            assert_eq!(node.children.len(), 2, "{detached:?}");
            let answer = &node.children[0].observed.value;
            match detached {
                Some(answered) => assert_eq!(answer, &Value::Bool(answered)),
                None => assert_ne!(
                    answer,
                    &Value::Absent,
                    "a missing answer must not read absent"
                ),
            }
        }
    }
}
