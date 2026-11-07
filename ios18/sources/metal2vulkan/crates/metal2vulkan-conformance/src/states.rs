//! The Metal pipeline states a case creates, counted at the one door.
//!
//! # Why the suite counts this and the runtime does not
//!
//! The cost half gates on [`metal2vulkan_runtime::WorkCounts`], which counts what *our*
//! implementation did: the `VkPipeline`s it made, the modules, the layouts. It has no count of the
//! `MTLRenderPipelineState`s and `MTLComputePipelineState`s a case asked for, because that is not
//! a thing the runtime does — it is a thing the **case** does, through the vocabulary, identically
//! on both legs. So the number belongs here, beside the cases, and nothing in a published crate
//! grows a counter for the suite's benefit.
//!
//! # What it is for
//!
//! One number: the denominator of `pipelines made per Metal pipeline state`
//! ([`crate::cost::PipelinesPerState`]). That ratio is the largest cost this project has against
//! Metal and the only form of it a gate can hold, because it is a count and so judgeable on a
//! loaded host. Measured on this M3 Max over all 1,103 cases, in the 639 that create a state and
//! carry a cost record: 1.519 `VkPipeline`s per Metal pipeline state, and 624 of the 1,439 made at
//! a state's creation taken by no command at all.
//!
//! # The one door, and what keeps it the only one
//!
//! Every pipeline-state creation on our leg goes through [`making_a_state`], which answers the
//! device it was handed and counts one state on the way. A site that reached for the device
//! directly would be a state the denominator never saw — and a denominator that is too small
//! reads as a ratio that is too large, so a missed site is a gate that fails honest work.
//!
//! The door is therefore held mechanically rather than by anybody's memory:
//! `tests::every_pipeline_state_is_counted` reads the two sources that create states and fails on
//! any creation whose receiver is not this function. The same shape as
//! `tests::the_announcing_side_forwards_every_call`, and for the same reason: a trait and a side
//! that several streams extend cannot be held by a list somebody remembers to update.
//!
//! # Why a process-wide reading rather than a field on the side
//!
//! Two of the creations are in free functions that take a `&Device` and no side at all (the binary
//! archives' step replay, and the kernel a texture read runs), so a counter on
//! [`crate::runtime_side::RuntimeSide`] would have to be threaded through their signatures or
//! leave them uncounted. This is read before and after each case exactly as `WorkCounts` is, and
//! the suite runs its cases one at a time in one process, so the difference is that case's own.

use metal2vulkan_runtime::Device;
use std::sync::atomic::{AtomicU64, Ordering};

/// The Metal pipeline states this process has created on our leg.
static MADE: AtomicU64 = AtomicU64::new(0);

/// `device`, with one Metal pipeline state counted against it: the one door every creation on our
/// leg goes through.
///
/// It answers the device rather than counting beside the call, so the counting cannot be forgotten
/// at a site that compiles: `making_a_state(self.device).new_render_pipeline_state(..)` is the only
/// spelling the source guard accepts, and a site that keeps `self.device` fails that test by name.
///
/// It counts the **attempt**, not the success. A creation the runtime refuses is still a state the
/// case asked for, and the numerator counts whatever `VkPipeline`s the attempt made before it
/// refused, so counting only the successes would move the ratio for a reason the case did not
/// change.
pub fn making_a_state(device: &Device) -> &Device {
    MADE.fetch_add(1, Ordering::Relaxed);
    device
}

/// How many Metal pipeline states our leg has created in this process.
///
/// Read before and after a case; the difference is what that case created.
pub fn made() -> u64 {
    MADE.load(Ordering::Relaxed)
}

#[cfg(test)]
mod tests {
    /// The two sources that create a pipeline state on our leg. Both are read, because one of them
    /// is a free function with no side to hang a counter on.
    const SOURCES: [(&str, &str); 2] = [
        ("runtime_side.rs", include_str!("runtime_side.rs")),
        ("runtime_work.rs", include_str!("runtime_work.rs")),
    ];

    /// **Every pipeline-state creation on our leg is counted**, and this is what keeps that true.
    ///
    /// The denominator of the pipelines-per-state ratio is this count, and a creation that slipped
    /// past it makes the denominator too small, which reads as a ratio too large — so a forgotten
    /// site is not a hole in a report, it is a gate that fails honest work. Nothing in the
    /// compiler can say so: `self.device.new_render_pipeline_state(..)` is a perfectly good call.
    ///
    /// So the guard reads the sources and requires the receiver of every creation to be
    /// [`super::making_a_state`]. Whitespace is collapsed first, because `rustfmt` puts the
    /// receiver on its own line as often as not.
    #[test]
    fn every_pipeline_state_is_counted() {
        let mut uncounted: Vec<String> = Vec::new();
        let mut counted = 0;
        for (name, source) in SOURCES {
            let flat: String = source.split_whitespace().collect::<Vec<_>>().join(" ");
            let mut at = 0;
            while let Some(found) = flat[at..].find(".new_") {
                let from = at + found;
                at = from + ".new_".len();
                let rest = &flat[from + 1..];
                let Some(open) = rest.find('(') else {
                    continue;
                };
                let call = &rest[..open];
                if !call.contains("pipeline_state") || !call.starts_with("new_") {
                    continue;
                }
                // The receiver is what sits before the dot. The door's spelling is
                // `making_a_state(<anything without a bracket>)`, which covers both the side's
                // `self.device` and a free function's `device`.
                let before = flat[..from].trim_end();
                let door = before.ends_with(')')
                    && before
                        .rfind("making_a_state(")
                        .is_some_and(|open| !before[open..].contains("))"));
                if door {
                    counted += 1;
                } else {
                    let tail: String = before.chars().rev().take(60).collect();
                    uncounted.push(format!(
                        "{name}: {call} is reached through `{}`",
                        tail.chars().rev().collect::<String>()
                    ));
                }
            }
        }
        assert!(
            uncounted.is_empty(),
            "{} pipeline-state creation(s) do not go through states::making_a_state, so the \
             pipelines-per-state ratio's denominator misses them and the gate would fail honest \
             work. Write each as `making_a_state(<the device>).new_..._pipeline_state(..)`:\n  {}",
            uncounted.len(),
            uncounted.join("\n  ")
        );
        // A floor and not a bound: a guard that found nothing to check would pass for ever, and
        // this one is exactly the shape that rots that way.
        assert!(
            counted > 20,
            "only {counted} creation(s) were found at all, so this guard is reading the wrong \
             thing"
        );
    }
}
