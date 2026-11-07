//! The texture slots of a render pipeline state's functions, and how each function accesses them
//! ([`RenderPipelineState::texture_access`](crate::RenderPipelineState::texture_access)).
//!
//! The answers come from the pipeline's slot writes
//! (`metal2vulkan::reflection::SlotWrite`): each argument's role, declared access and slot, with
//! no type description derived. The same values are in the pipeline's reflection as
//! `Binding::access`, but a reflection must also give Metal's `MTLType` for every binding and
//! refuses the whole function where Metal's answer for one argument's *type* has not been
//! measured. Reading these answers from the reflection made one such argument refuse every slot
//! of the state, read-only ones included, and a caller that cannot tell a read-only slot from a
//! writable one has to treat every bound texture as written.
//!
//! Two properties follow, and both matter to a caller that must be safe:
//!
//! - **A state that compiles answers.** The slot writes refuse nothing a compile accepted.
//! - **No refusal is remembered.** Only a successful set of answers is kept
//!   ([`TextureAccessMemo`]); a slot left undetermined is worked out again from that set on every
//!   call, and a failure to get the answers at all is not stored, so a later call tries again.
//!   The memo used to hold a `Result` and hand back the same refusal for the life of the state.
//!
//! A slot whose answer one argument alone leaves undetermined is refused as that slot, naming the
//! gap; every other slot of the state still answers.

use crate::error::Error;
use metal2vulkan::error::{DiagName, Feature, Unsupported};
use metal2vulkan::metal::{BindingAccess, BindingType, RenderStages};
use metal2vulkan::reflection::{SlotWritability, SlotWrite, SlotWrites};
use metal2vulkan::Budget;
use std::ops::Range;
use std::sync::OnceLock;

/// What one texture argument of a stage answers: the slots it covers and its declared access, or
/// the gap that left it undetermined.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Answer {
    /// The argument's declared access, as Metal answers it.
    Access(BindingAccess),
    /// Undetermined for the reason named; the slots it covers answer no access.
    Gap(metal2vulkan::reflection::SlotWriteGap),
}

/// The texture arguments a render pipeline state's functions reference. Each entry has its
/// stage, the slots it covers (each element's slot, for an array) and its answer.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct TextureAccesses(Vec<(RenderStages, Range<u64>, Answer)>);

/// A state's [`TextureAccesses`], worked out when first asked and kept while the state lives.
///
/// It holds a successful set of answers alone. A call that cannot get them stores nothing, so a
/// refusal is never handed back for the life of the state (which is what a `Result` here did).
pub(crate) type TextureAccessMemo = OnceLock<TextureAccesses>;

/// The widest span a slot can be asked about, for an argument whose own span is not stated: it may
/// cover any slot, so it must not let one read as unbound.
const EVERY_SLOT: Range<u64> = 0..u64::MAX;

impl TextureAccesses {
    /// The referenced texture arguments of `writes`'s stages.
    ///
    /// An argument of another binding type is left out, and so is one the function never
    /// references: Metal's reflection lists that one with `isUsed` false, and the function
    /// neither reads nor writes it. An argument whose role determines no binding type at all, or
    /// whose slots only a function constant states, may be a texture at any slot, so it is kept
    /// over every slot as a gap.
    fn of(writes: &SlotWrites) -> TextureAccesses {
        let stages: [(RenderStages, &[SlotWrite]); 5] = [
            (RenderStages::VERTEX, writes.vertex_slots()),
            (RenderStages::FRAGMENT, writes.fragment_slots()),
            (RenderStages::TILE, writes.tile_slots()),
            (RenderStages::OBJECT, writes.object_slots()),
            (RenderStages::MESH, writes.mesh_slots()),
        ];
        let mut accesses = Vec::new();
        for (stage, slots) in stages {
            for slot in slots {
                if slot.binding_type.is_some_and(|t| t != BindingType::Texture) {
                    continue;
                }
                let answer = match (slot.writes, slot.access) {
                    (SlotWritability::Unknown(gap), _) => Answer::Gap(gap),
                    (_, Some(access)) => Answer::Access(access),
                    // A determined writability always carries its declared access.
                    (_, None) => continue,
                };
                let span = match (slot.index, slot.count, answer) {
                    (Some(first), Some(count), _) => {
                        let first = u64::from(first);
                        first..first + u64::from(count.max(1))
                    }
                    _ => EVERY_SLOT,
                };
                if matches!(answer, Answer::Access(_)) && !slot.used {
                    continue;
                }
                accesses.push((stage, span, answer));
            }
        }
        TextureAccesses(accesses)
    }

    /// The accesses of the referenced texture arguments of `stages` that cover slot `index`,
    /// joined into one; `None` where none covers it.
    ///
    /// Refused where an argument that may cover the slot is undetermined and none that covers it
    /// answers: that slot alone, naming the gap. Where a determined argument covers it, that
    /// answer stands, so one undetermined argument does not take the rest of the state with it.
    pub(crate) fn at(
        &self,
        stages: RenderStages,
        index: u64,
    ) -> Result<Option<BindingAccess>, Error> {
        let covering = || {
            self.0
                .iter()
                .filter(move |(stage, slots, _)| stages.contains(*stage) && slots.contains(&index))
        };
        let joined = covering()
            .filter_map(|(_, _, answer)| match answer {
                Answer::Access(access) => Some(*access),
                Answer::Gap(_) => None,
            })
            .reduce(|a, b| if a == b { a } else { BindingAccess::ReadWrite });
        if let Some(access) = joined {
            return Ok(Some(access));
        }
        match covering().find_map(|(_, _, answer)| match answer {
            Answer::Gap(gap) => Some(*gap),
            Answer::Access(_) => None,
        }) {
            Some(gap) => Err(Error::Unsupported(
                Unsupported::not_implemented(Feature::Reflection)
                    .naming(DiagName::new(gap_name(gap))),
            )),
            None => Ok(None),
        }
    }

    /// `memo`'s accesses, from `slot_writes`'s answers (under the standard budget) the first time.
    /// A failure to get them is returned and not stored.
    pub(crate) fn remembered(
        memo: &TextureAccessMemo,
        slot_writes: impl FnOnce(&Budget) -> Result<SlotWrites, Error>,
    ) -> Result<&TextureAccesses, Error> {
        if let Some(accesses) = memo.get() {
            return Ok(accesses);
        }
        let answers = TextureAccesses::of(&slot_writes(&Budget::standard())?);
        // Another thread racing here computed the same answers, so either value will do.
        Ok(memo.get_or_init(|| answers))
    }
}

/// The name a refused slot's gap is reported under.
fn gap_name(gap: metal2vulkan::reflection::SlotWriteGap) -> &'static str {
    use metal2vulkan::reflection::SlotWriteGap as G;
    match gap {
        G::UnresolvedIndex => "a texture slot only a function constant states",
        G::UnmeasuredPayloadStage => "a payload in a stage Metal's access is unmeasured for",
        G::MemberRole => "an argument whose role only an argument-buffer member takes",
        _ => "an undetermined texture slot",
    }
}

#[cfg(test)]
mod tests {
    use super::{Answer, TextureAccesses};
    use metal2vulkan::metal::{BindingAccess, RenderStages};
    use metal2vulkan::reflection::SlotWriteGap;

    /// A slot's answer is the access of the referenced texture argument covering it in the
    /// stages asked, an array's at each element's slot; stages that agree keep their access,
    /// stages that differ join to `ReadWrite`; no stage covering the slot, or no stage asked, is
    /// `None`.
    #[test]
    fn a_slot_answers_the_accesses_of_the_stages_asked_joined() {
        use BindingAccess::{ReadOnly, ReadWrite, WriteOnly};
        let (vertex, fragment) = (RenderStages::VERTEX, RenderStages::FRAGMENT);
        let accesses = TextureAccesses(vec![
            (vertex, 0..1, Answer::Access(ReadOnly)),
            (vertex, 1..2, Answer::Access(ReadOnly)),
            (fragment, 0..1, Answer::Access(ReadOnly)),
            (fragment, 1..3, Answer::Access(WriteOnly)),
        ]);
        let both = vertex.union(fragment);
        for (stages, index, want) in [
            (vertex, 0, Some(ReadOnly)),
            (fragment, 2, Some(WriteOnly)),
            (fragment, 3, None),
            (both, 0, Some(ReadOnly)),
            (both, 1, Some(ReadWrite)),
            (both, 2, Some(WriteOnly)),
            (RenderStages::TILE, 0, None),
            (RenderStages::empty(), 0, None),
        ] {
            assert_eq!(
                accesses.at(stages, index).expect("answered"),
                want,
                "{stages:?} {index}"
            );
        }
    }

    /// An undetermined argument refuses the slots it may cover and no others: the slots a
    /// determined argument covers keep their answers, and so does every stage it is not in. That
    /// is the whole point of answering per slot -- one gap must not take the state with it.
    #[test]
    fn an_undetermined_argument_refuses_its_own_slots_alone() {
        let (vertex, fragment) = (RenderStages::VERTEX, RenderStages::FRAGMENT);
        let accesses = TextureAccesses(vec![
            (vertex, 0..1, Answer::Access(BindingAccess::ReadOnly)),
            (vertex, 4..5, Answer::Gap(SlotWriteGap::UnresolvedIndex)),
            (fragment, 0..2, Answer::Access(BindingAccess::WriteOnly)),
        ]);
        // The determined slots answer.
        assert_eq!(
            accesses.at(vertex, 0).expect("answered"),
            Some(BindingAccess::ReadOnly)
        );
        assert_eq!(
            accesses.at(fragment, 1).expect("answered"),
            Some(BindingAccess::WriteOnly)
        );
        // A slot nothing covers is still unbound, not refused.
        assert_eq!(accesses.at(vertex, 9).expect("answered"), None);
        // The undetermined slot is refused, and it alone.
        assert!(accesses.at(vertex, 4).is_err());
        assert!(accesses.at(fragment, 4).expect("answered").is_none());
        // A determined argument covering the same slot wins: nothing is lost to the gap.
        let both = TextureAccesses(vec![
            (vertex, 4..5, Answer::Gap(SlotWriteGap::UnresolvedIndex)),
            (vertex, 4..5, Answer::Access(BindingAccess::ReadOnly)),
        ]);
        assert_eq!(
            both.at(vertex, 4).expect("answered"),
            Some(BindingAccess::ReadOnly)
        );
    }
}
