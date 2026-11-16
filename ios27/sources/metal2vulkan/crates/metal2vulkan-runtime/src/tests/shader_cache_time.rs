//! What the shader cache's directory costs in time, and which thread pays it
//! (`WorkCounts::shader_cache_read_nanos`, `shader_cache_write_nanos`,
//! `shader_cache_off_thread_nanos`).
//!
//! # Why the three are apart
//!
//! A pipeline state's creation reaches the directory on its own thread, and the device's cache
//! thread reaches it beside the creation. A single figure over both could not say whether a
//! creation that took 300 microseconds waited for the directory or merely ran next to a thread
//! that did, which is the whole question a consumer asks of these counters. So the reads and the
//! writes a thread that asked made itself are counted apart from everything the cache thread did.
//!
//! # What is in each window
//!
//! The directory's calls alone: the window opens just before the filesystem call and closes just
//! after it, so a lookup the write queue answered, which reads no file, and the encode and decode
//! of an entry's bytes are in none of the three. Each test here holds that by moving one window
//! and asserting the others stand still, so a window drawn around the wrong work fails rather
//! than passing with a larger number.
//!
//! The entries are the driver's pipeline cache frames, as `tests::shader_cache`'s are, and each
//! test owns an empty directory it removes.

use super::shader_cache::{store, store_with_queue, Scratch, BUILD};
use crate::shader_cache::store::{entry_name, Lookup, Store};
use crate::shader_cache::worker::{drain, Asked, QUEUED_WRITES};
use crate::shader_cache::EntryKind;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

const NO_PROFILE: [u8; 32] = [0; 32];

/// How many lookups or writes each leg makes: enough that a window that times the calls reads
/// well past the clock's own resolution, few enough that none of this is a benchmark.
const ROUND: u32 = 32;

/// The entry name of the made-up key `n`.
fn name(n: u32) -> String {
    entry_name(&BUILD, EntryKind::PipelineData, &[n as u8; 32], &NO_PROFILE)
}

/// The three figures: the reads, the writes a thread that asked made itself, and the cache
/// thread's.
fn nanos(store: &Store) -> (u64, u64, u64) {
    let read = |counter: &std::sync::atomic::AtomicU64| counter.load(Ordering::Relaxed);
    (
        read(&store.counters.read_nanos),
        read(&store.counters.write_nanos),
        read(&store.counters.off_thread_nanos),
    )
}

/// Look the entry named `name` up, taking whatever bytes are there.
fn look(store: &Store, name: &str) -> Lookup<Vec<u8>> {
    store.load(name, |read: &[u8]| Ok::<Vec<u8>, ()>(read.to_vec()))
}

/// A lookup that reaches the directory is counted in the reads and in nothing else, whether it
/// finds the entry or not; a write the asking thread makes itself is counted in the writes and in
/// nothing else. The cache thread's figure stays zero throughout: this store has no cache thread,
/// so every call here is the asking thread's.
#[test]
fn a_lookup_is_read_time_and_a_creation_s_own_write_is_write_time() {
    let scratch = Scratch::new();
    let store = store(scratch.path(), BUILD, 1 << 30);
    assert_eq!(nanos(&store), (0, 0, 0), "a store that has done nothing");
    // Absent entries: the `open` of each is a read and nothing else.
    for n in 0..ROUND {
        assert_eq!(look(&store, &name(n)), Lookup::Absent);
    }
    let (absent, written, thread) = nanos(&store);
    assert!(absent > 0, "{ROUND} lookups of absent entries read 0 ns");
    assert_eq!(
        (written, thread),
        (0, 0),
        "a lookup is not a write and not the cache thread's"
    );
    // Writes on this thread: the store has no cache thread, so each is this thread's four calls.
    for n in 0..ROUND {
        store.write_soon(&name(n), vec![n as u8; 256]).unwrap();
    }
    let (after_writes, written, thread) = nanos(&store);
    assert!(written > 0, "{ROUND} writes on this thread took 0 ns");
    assert_eq!(
        (after_writes, thread),
        (absent, 0),
        "a write is not a read and not the cache thread's"
    );
    assert_eq!(
        store.counters.writes_on_thread.load(Ordering::Relaxed),
        u64::from(ROUND),
        "every write was this thread's"
    );
    // Hits: the `open`, the `read` and the access-time refresh of each are reads.
    for n in 0..ROUND {
        assert!(matches!(look(&store, &name(n)), Lookup::Hit(_)));
    }
    let (hits, write_after, thread) = nanos(&store);
    assert!(hits > after_writes, "{ROUND} hits read no time");
    assert_eq!(
        (write_after, thread),
        (written, 0),
        "a hit is not a write and not the cache thread's"
    );
    // A question about an entry is a `stat`, counted in the reads like any other.
    for n in 0..ROUND {
        assert!(store.holds(&name(n)));
    }
    assert!(
        nanos(&store).0 > hits,
        "{ROUND} questions about entries read no time"
    );
}

/// A write the device's cache thread took is the cache thread's time, and a write the asking
/// thread made because the queue was at its count bound is the asking thread's: the counter that
/// partitions creations by which one wrote (`writes_on_thread`) and the two time figures agree.
/// A lookup the queue answered reads no directory at all.
#[test]
fn a_queued_write_is_the_cache_thread_s_time_and_a_bounded_one_the_asker_s() {
    let scratch = Scratch::new();
    let asked = Arc::new(Asked::new());
    let store = store_with_queue(scratch.path(), BUILD, 1 << 30, Arc::clone(&asked));
    // Filling the queue to its count bound: each of these is queued and written by nobody yet.
    for n in 0..QUEUED_WRITES as u32 {
        store.write_soon(&name(n), vec![n as u8; 256]).unwrap();
    }
    assert_eq!(
        nanos(&store),
        (0, 0, 0),
        "a queued write touches the directory on no thread"
    );
    assert_eq!(store.counters.writes_on_thread.load(Ordering::Relaxed), 0);
    // A lookup the queue answers reads no file.
    assert!(matches!(look(&store, &name(0)), Lookup::Hit(_)));
    assert_eq!(store.counters.queued_hits.load(Ordering::Relaxed), 1);
    assert_eq!(
        nanos(&store),
        (0, 0, 0),
        "a lookup the queue answered reads no directory"
    );
    // One more: the queue is at its bound, so this creation writes its own entry, and pays for it.
    let over = name(QUEUED_WRITES as u32);
    store.write_soon(&over, vec![9; 256]).unwrap();
    let (read, written, thread) = nanos(&store);
    assert_eq!(
        store.counters.writes_on_thread.load(Ordering::Relaxed),
        1,
        "the creation past the bound wrote its own entry"
    );
    assert!(written > 0, "the creation's own write took 0 ns");
    assert_eq!(
        (read, thread),
        (0, 0),
        "the creation's own write is neither a read nor the cache thread's"
    );
    // The drain is the cache thread's work: it writes every queued entry on its own time.
    drain(&asked, &store);
    let (read_after, write_after, thread) = nanos(&store);
    assert!(thread > 0, "the drain of the queue took 0 ns");
    assert_eq!(
        (read_after, write_after),
        (read, written),
        "the cache thread's writes are neither reads nor the asking threads' writes"
    );
    assert_eq!(
        store.counters.writes_queued.load(Ordering::Relaxed),
        QUEUED_WRITES as u64
    );
}

/// The scan that bounds the directory is the cache thread's time, never a creation's: it runs at
/// device creation and at every eviction, on the thread, and reads the whole directory.
#[test]
fn the_scan_that_bounds_the_directory_is_the_cache_thread_s_time() {
    let scratch = Scratch::new();
    let store = store(scratch.path(), BUILD, 1 << 30);
    for n in 0..ROUND {
        store.write_soon(&name(n), vec![n as u8; 256]).unwrap();
    }
    let (read, written, thread) = nanos(&store);
    assert_eq!(thread, 0, "nothing has scanned yet");
    store.sweep(&AtomicBool::new(false));
    let (read_after, write_after, thread) = nanos(&store);
    assert!(thread > 0, "the sweep of the directory took 0 ns");
    assert_eq!(
        (read_after, write_after),
        (read, written),
        "the scan is neither a lookup's read nor an asking thread's write"
    );
}
