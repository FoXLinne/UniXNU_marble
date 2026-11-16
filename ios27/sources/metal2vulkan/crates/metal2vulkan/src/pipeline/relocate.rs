//! Relocation of serialized function modules.
//!
//! `relocate` writes only the literals the serializer reserved (descriptor set and binding,
//! `Location`/`Component`, `SpecId`); a symbol the table does not assign is a `BuildError`
//! (`Internal`). The words of a module with its relocation slots zeroed are therefore the same
//! for every link.
//!
//! That invariant is what [`relocated`] rests on: the final words are a pure function of the
//! module and the value link assigned to each reserved slot, so two links that assign the same
//! values want the same words, and a compiled function's relocation slot
//! ([`LastRelocation`](crate::library::compiled::LastRelocation)) can answer the second of them
//! without touching the words at all.

use std::sync::Arc;

use crate::error::Error;
use crate::library::{CompiledFunction, FunctionArtifact};
use crate::spirv::serialize::SymbolTable;

/// The final SPIR-V words of `a` under link table `link` (a `LinkTable` or the `Linked` result of
/// the pipeline `a` was linked into).
pub fn relocate(a: &FunctionArtifact, link: &impl SymbolTable) -> Result<Vec<u32>, Error> {
    Ok(crate::spirv::serialize::relocate(&a.module, link)?)
}

/// The final SPIR-V words of `c`'s function under link table `link`: the words of `c`'s last
/// relocation where `link` assigns its slots the same values, else relocated now and kept as
/// `c`'s last.
///
/// What link assigned is read first, which costs one pass over the module's relocation list and
/// none over its words; a hit therefore neither clones the module nor rewrites a literal, and
/// gives back the very `Arc` the earlier pipeline's stage holds. A symbol `link` does not assign
/// is left to [`relocate`] to refuse, so a missing symbol is the same error from either path.
pub fn relocated(c: &CompiledFunction, link: &impl SymbolTable) -> Result<Arc<[u32]>, Error> {
    let a = c.artifact();
    let assigned: Option<Vec<u32>> = a
        .module
        .relocs
        .iter()
        .map(|(sym, _)| link.resolve(sym))
        .collect();
    // A symbol the table does not assign: no key, and `relocate` names it.
    let Some(assigned) = assigned else {
        return Ok(relocate(a, link)?.into());
    };
    if let Some(words) = c.last_relocation().words_for(&assigned) {
        return Ok(words);
    }
    note_relocated();
    let words: Arc<[u32]> = relocate(a, link)?.into();
    c.last_relocation().keep(assigned, &words);
    Ok(words)
}

/// The relocations this process performed through [`relocated`], which
/// `tests::relocated_once`'s count reads: a run of sibling pipeline creations over one function
/// must add none to what its first creation relocated.
#[cfg(test)]
static RELOCATED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// [`RELOCATED`].
#[cfg(test)]
pub(crate) fn relocations_done() -> u64 {
    RELOCATED.load(std::sync::atomic::Ordering::Relaxed)
}

/// Count a relocation the slot could not answer.
fn note_relocated() {
    #[cfg(test)]
    RELOCATED.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
}
