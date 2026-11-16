//! What this device's cache directory answered about a stage's **function**, kept under the
//! function's own key.
//!
//! A render variant's link asks, once per stage, whether the directory holds that stage's
//! compiled function, because that decides whether storing the pipeline's own artifact would
//! spare a later process a translation or only a link (`pipeline/sources.rs`'s `linked_render`).
//! The question is one `stat` of a name, single-digit microseconds on an M3 Max, and it is asked
//! on the creating thread.
//!
//! - **The answer is the function's, not the pipeline's.** The name the question is asked under is
//!   the SHA-256 of the build, the entry kind, the function's compile key and the device's profile
//!   fingerprint (`store::entry_name`), so nothing of the pipeline state reaches it. A sweep of
//!   states over one vertex-and-fragment pair therefore asks the same two questions for every
//!   sibling. This keeps the answer under exactly the two parts of that name a device does not
//!   fix: the function's digest and the profile. Keying it any more coarsely -- by the pair, by
//!   the descriptor, by the state -- would answer a question that was never asked.
//! - **Only `true` is kept.** The write that puts a function in the directory is the device's
//!   cache thread's, so a function absent at one question is often present at the next, within
//!   one creation: the link asks after the stage's translation was handed over. A `false` is
//!   therefore no answer about the key at all, and the next asker asks the directory.
//! - **A removal forgets everything.** An entry the directory removes -- evicted past the cap, or
//!   refused by its decoder -- may be one an answer here stands for. So each answer is kept
//!   beside the store's count of its removals, and a count that has moved empties the memo rather
//!   than letting one answer outlive the file it was about. A removal by another process is not
//!   seen, as it is not seen between the question and the store that follows it today.
//! - **Bounded** at [`ENTRIES`] keys, the least recently used going first, each a pair of
//!   digests and no artifact.

use crate::device::lock;
use crate::pipeline::memo::Lru;
use metal2vulkan::cache::Hash256;
use std::sync::Mutex;

/// The most function keys kept: a few thousand stage functions is more than any one process
/// links, and each entry is 64 bytes of digest.
pub(crate) const ENTRIES: usize = 4096;

/// The answers, and the store's removal count they were taken at.
struct Kept {
    /// The functions the directory answered `true` for, by digest and profile.
    keys: Lru<(Hash256, Hash256), ()>,
    /// [`Store::removals`](super::store::Store::removals) when those answers were taken.
    ///
    /// [`Store::removals`]: super::store::Store::removals
    removals: u64,
}

/// What the directory answered about the stage functions this device asked about (the module
/// docs).
pub(crate) struct HeldFunctions(Mutex<Kept>);

impl HeldFunctions {
    /// A memo that has been told nothing.
    pub(crate) fn new() -> HeldFunctions {
        HeldFunctions(Mutex::new(Kept {
            keys: Lru::counted(ENTRIES),
            removals: 0,
        }))
    }

    /// Whether the directory holds the function of `digest` for the profile of `profile`, from
    /// what it answered before, else from `ask` -- the directory's own question -- whose `true`
    /// is kept. `removals` is the store's removal count now: one that has moved since the kept
    /// answers were taken empties them first.
    pub(crate) fn holds(
        &self,
        (digest, profile): (&Hash256, &Hash256),
        removals: u64,
        ask: impl FnOnce() -> bool,
    ) -> bool {
        let at = (*digest, *profile);
        {
            let kept = &mut *lock(&self.0);
            if kept.removals == removals {
                if kept.keys.get(&at).is_some() {
                    return true;
                }
            } else {
                kept.keys = Lru::counted(ENTRIES);
                kept.removals = removals;
            }
        }
        let held = ask();
        if held {
            let kept = &mut *lock(&self.0);
            // A removal during the question makes the answer one about a directory that has
            // since changed: it answers this asker and is not kept.
            if kept.removals == removals {
                kept.keys.insert(at, (), 1);
            }
        }
        held
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    /// A memo, and how many questions it has put to the directory.
    struct Asked {
        held: HeldFunctions,
        asked: Cell<u64>,
    }

    impl Asked {
        fn new() -> Asked {
            Asked {
                held: HeldFunctions::new(),
                asked: Cell::new(0),
            }
        }

        /// Ask about the function of `digest` for profile `profile` at removal count `removals`,
        /// where the directory answers `answer`.
        fn holds(&self, digest: u8, profile: u8, removals: u64, answer: bool) -> bool {
            self.held
                .holds((&[digest; 32], &[profile; 32]), removals, || {
                    self.asked.set(self.asked.get() + 1);
                    answer
                })
        }
    }

    /// One function's `true` is kept, and nothing else is: another digest and another profile are
    /// each asked about, however many times the first was.
    #[test]
    fn a_kept_answer_answers_its_own_key_and_no_other() {
        let a = Asked::new();
        assert!(a.holds(1, 9, 0, true));
        assert_eq!(a.asked.get(), 1);
        // The same key, many times over: asked once.
        for _ in 0..16 {
            assert!(a.holds(1, 9, 0, true));
        }
        assert_eq!(
            a.asked.get(),
            1,
            "a function the directory answered for must be asked about once"
        );
        // Another function's digest, and another profile of the same function: each its own key.
        assert!(a.holds(2, 9, 0, true));
        assert_eq!(a.asked.get(), 2, "another function is another key");
        assert!(a.holds(1, 8, 0, true));
        assert_eq!(a.asked.get(), 3, "another profile is another key");
        // And each of those is now kept under its own key.
        assert!(a.holds(2, 9, 0, true));
        assert!(a.holds(1, 8, 0, true));
        assert_eq!(a.asked.get(), 3);
    }

    /// A `false` is no answer about the key: the next asker asks the directory, and gets the
    /// `true` the cache thread's write has since made of it.
    #[test]
    fn a_false_answer_is_asked_again() {
        let a = Asked::new();
        for n in 1..=4 {
            assert!(!a.holds(1, 9, 0, false));
            assert_eq!(a.asked.get(), n, "a `false` must not be kept");
        }
        assert!(a.holds(1, 9, 0, true));
        assert_eq!(a.asked.get(), 5);
        assert!(a.holds(1, 9, 0, false), "the `true` is kept from here on");
        assert_eq!(a.asked.get(), 5);
    }

    /// A removal empties the memo: an answer taken before it may stand for a file the removal
    /// took.
    #[test]
    fn a_removal_forgets_every_answer() {
        let a = Asked::new();
        assert!(a.holds(1, 9, 0, true));
        assert!(a.holds(2, 9, 0, true));
        assert_eq!(a.asked.get(), 2);
        // Neither answer outlives the removal.
        assert!(a.holds(1, 9, 1, true));
        assert!(a.holds(2, 9, 1, true));
        assert_eq!(a.asked.get(), 4, "a removal forgets both answers");
        assert!(a.holds(1, 9, 1, true));
        assert_eq!(a.asked.get(), 4, "and the answers after it are kept");
    }

    /// Past [`ENTRIES`] keys the least recently used answer goes, and is asked about again.
    #[test]
    fn the_memo_is_bounded() {
        let held = HeldFunctions::new();
        let mut asked = 0u64;
        let ask = |digest: u64, asked: &mut u64| {
            let mut key = [0u8; 32];
            key[..8].copy_from_slice(&digest.to_le_bytes());
            held.holds((&key, &[0; 32]), 0, || {
                *asked += 1;
                true
            })
        };
        for digest in 0..ENTRIES as u64 {
            assert!(ask(digest, &mut asked));
        }
        assert_eq!(asked, ENTRIES as u64);
        // The most recently used is still there, so the first is the one that went.
        assert!(ask(ENTRIES as u64 - 1, &mut asked));
        assert_eq!(asked, ENTRIES as u64);
        assert!(ask(ENTRIES as u64, &mut asked));
        assert_eq!(asked, ENTRIES as u64 + 1);
        assert!(ask(0, &mut asked));
        assert_eq!(
            asked,
            ENTRIES as u64 + 2,
            "the least recently used answer must go past the bound"
        );
    }
}
