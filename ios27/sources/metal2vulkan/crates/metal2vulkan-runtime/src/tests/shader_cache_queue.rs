//! The shader cache's queue of writes (`shader_cache/worker.rs`): what a creation hands to the
//! device's cache thread, what the queue answers meanwhile, and what happens at its bounds. The
//! store here has the queue but no thread, so the test drains it itself and nothing depends on
//! when a thread would have run.
//!
//! The entries are the driver's pipeline cache frames, as `tests::shader_cache`'s are, and each
//! test owns an empty directory it removes.

use super::shader_cache::{store_with_queue, Scratch, BUILD};
use crate::shader_cache::store::{entry_name, Lookup, Store};
use crate::shader_cache::worker::{drain, run, Asked, QUEUED_BYTES, QUEUED_WRITES};
use crate::shader_cache::EntryKind;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::sync::Arc;

const NO_PROFILE: [u8; 32] = [0; 32];

/// The entry name of the made-up key `n`.
fn name(n: u32) -> String {
    entry_name(&BUILD, EntryKind::PipelineData, &[n as u8; 32], &NO_PROFILE)
}

/// `length` bytes whose first says which entry they are.
fn bytes(n: u32, length: usize) -> Vec<u8> {
    let mut made = vec![0x5a; length];
    made[0] = n as u8;
    made
}

/// The entry named `name` under `root` for [`BUILD`].
fn path(root: &Path, name: &str) -> PathBuf {
    root.join("v1")
        .join(super::shader_cache::hex_build())
        .join(&name[..2])
        .join(name)
}

/// The store of `root` with a queue of its own, and the queue.
fn queued(root: &Path) -> (Store, Arc<Asked>) {
    let asked = Arc::new(Asked::new());
    (
        store_with_queue(root, BUILD, 1 << 30, Arc::clone(&asked)),
        asked,
    )
}

/// Every byte of the entry named `name`, read back as the store reads it.
fn read(store: &Store, name: &str) -> Option<Vec<u8>> {
    match store.load(name, |read: &[u8]| Ok::<Vec<u8>, ()>(read.to_vec())) {
        Lookup::Hit(found) => Some(found),
        Lookup::Absent | Lookup::Unreadable | Lookup::Rejected => None,
    }
}

/// A queued entry is not on disk until the queue is drained, and the store's own lookup finds it
/// from the queue meanwhile: a creation whose entry is still queued never compiles it again. The
/// drain leaves the file and no temporary beside it.
#[test]
fn a_queued_entry_answers_lookups_before_its_file_exists() {
    let scratch = Scratch::new();
    let (store, asked) = queued(scratch.path());
    let name = name(1);
    let wanted = bytes(1, 512);
    assert_eq!(store.write_soon(&name, wanted.clone()).unwrap(), 512);
    let at = path(scratch.path(), &name);
    assert!(!at.exists(), "the queued entry's file is not written yet");
    assert_eq!(
        read(&store, &name).as_deref(),
        Some(wanted.as_slice()),
        "the queue answers the lookup"
    );
    assert_eq!(store.counters.queued_hits.load(Ordering::Relaxed), 1);
    assert_eq!(store.counters.writes_queued.load(Ordering::Relaxed), 1);
    assert_eq!(store.counters.written_bytes.load(Ordering::Relaxed), 0);
    drain(&asked, &store);
    assert_eq!(fs::read(&at).unwrap(), wanted, "the drain wrote the entry");
    assert_eq!(store.counters.written_bytes.load(Ordering::Relaxed), 512);
    let group: Vec<String> = fs::read_dir(at.parent().unwrap())
        .unwrap()
        .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    assert_eq!(
        group,
        std::slice::from_ref(&name),
        "the rename left no temporary file"
    );
    // Drained, the lookup reads the file: no second queued hit.
    assert_eq!(read(&store, &name).as_deref(), Some(wanted.as_slice()));
    assert_eq!(store.counters.queued_hits.load(Ordering::Relaxed), 1);
}

/// Two writes under one name leave one queued write, the later bytes: one drain writes the second
/// and the queue is then empty, where two queued writes would have left the first behind. Two
/// creations racing on one key therefore leave one file and one `rename`.
#[test]
fn a_second_write_under_one_name_replaces_the_queued_one() {
    let scratch = Scratch::new();
    let (store, asked) = queued(scratch.path());
    let name = name(2);
    store.write_soon(&name, bytes(2, 64)).unwrap();
    let second = bytes(3, 96);
    store.write_soon(&name, second.clone()).unwrap();
    assert_eq!(
        read(&store, &name).as_deref(),
        Some(second.as_slice()),
        "the queue holds the later bytes"
    );
    drain(&asked, &store);
    let at = path(scratch.path(), &name);
    assert_eq!(fs::read(&at).unwrap(), second);
    assert_eq!(
        store.counters.written_bytes.load(Ordering::Relaxed),
        96,
        "one write of the later bytes, not two"
    );
}

/// Past the queue's count bound a creation writes its entry itself, at once, and so does one
/// whose entry is larger than the whole bound: the bound costs a creation its own write, never
/// the entry.
#[test]
fn a_creation_past_the_bound_writes_its_entry_itself() {
    let scratch = Scratch::new();
    let (store, asked) = queued(scratch.path());
    for n in 0..QUEUED_WRITES as u32 {
        let name = name(n);
        store.write_soon(&name, bytes(n, 32)).unwrap();
        assert!(!path(scratch.path(), &name).exists(), "entry {n} is queued");
    }
    let over = name(QUEUED_WRITES as u32);
    store.write_soon(&over, bytes(7, 32)).unwrap();
    assert!(
        path(scratch.path(), &over).exists(),
        "the entry past the count bound is written by its own caller"
    );
    assert_eq!(
        store.counters.writes_queued.load(Ordering::Relaxed),
        QUEUED_WRITES as u64
    );
    // A single entry larger than the whole byte bound is never queued either.
    let (store, _asked) = queued(scratch.path());
    let large = name(200);
    let length = usize::try_from(QUEUED_BYTES).unwrap() + 1;
    store.write_soon(&large, bytes(200, length)).unwrap();
    assert!(
        path(scratch.path(), &large).exists(),
        "an entry larger than the queue's whole bound is written by its own caller"
    );
    assert_eq!(store.counters.writes_queued.load(Ordering::Relaxed), 0);
    drop(asked);
}

/// A decoder that refuses a queued entry's bytes drops the queued write rather than letting the
/// cache thread put them in place: the drain writes nothing, and the lookup is a refusal.
#[test]
fn a_refused_queued_entry_is_never_written() {
    let scratch = Scratch::new();
    let (store, asked) = queued(scratch.path());
    let name = name(4);
    store.write_soon(&name, bytes(4, 128)).unwrap();
    let refused = store.load(&name, |_: &[u8]| Err::<(), ()>(()));
    assert_eq!(refused, Lookup::Rejected);
    drain(&asked, &store);
    assert!(
        !path(scratch.path(), &name).exists(),
        "the refused bytes were never written"
    );
    assert_eq!(store.counters.written_bytes.load(Ordering::Relaxed), 0);
}

/// The cache thread writes what is queued before it ends, so an entry a creation handed over is on
/// disk once the device's drop has stopped the thread. Asked to stop before its loop ever waits,
/// the thread still writes every queued entry and leaves no temporary file.
///
/// The stop is asked for before the loop runs, so nothing here depends on a thread keeping up or
/// falling behind: a loop that did not write at the stop would leave the files absent on every
/// host. (A device test cannot hold this: its cache thread drains the queue long before the device
/// goes, so the files are there whether or not the stop writes them.)
#[test]
fn the_thread_writes_what_is_queued_before_it_ends() {
    let scratch = Scratch::new();
    let (store, asked) = queued(scratch.path());
    let names: Vec<String> = (10..14).map(name).collect();
    for (n, name) in names.iter().enumerate() {
        store.write_soon(name, bytes(n as u32, 256)).unwrap();
        assert!(!path(scratch.path(), name).exists(), "{name} is queued");
    }
    asked.stopping();
    run(&asked, &store);
    for (n, name) in names.iter().enumerate() {
        let at = path(scratch.path(), name);
        assert_eq!(
            fs::read(&at).ok().as_deref(),
            Some(bytes(n as u32, 256).as_slice()),
            "the stop wrote {name}"
        );
        let group: Vec<String> = fs::read_dir(at.parent().unwrap())
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        assert!(
            group.iter().all(|found| !found.starts_with('.')),
            "the stop left a temporary file: {group:?}"
        );
    }
    assert_eq!(
        store.counters.written_bytes.load(Ordering::Relaxed),
        256 * names.len() as u64
    );
}
