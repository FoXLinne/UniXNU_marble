//! A render pipeline state's creation hands its cache entries to the device's cache thread and
//! writes no file itself, and the entries it handed over are on disk once the device has gone.
//!
//! # Why this is a count
//!
//! Every entry a creation gives the directory used to be written on the creating thread: a
//! `mkdir`, a temporary file, a write and a `rename`, four journalled filesystem calls each,
//! which the caller waited for. The entries are still written, under the same names and
//! committed by the same `rename`; the creating thread no longer waits for them.
//!
//! The assertion is a count of writes, never a time: a count is a fact of the structure, so it
//! holds on any host under any load, and the figure this run would give for the time saved would
//! be a figure of this host's filesystem under this host's load. `writes` is the floor that keeps
//! a pass from being vacuous: a sweep that writes nothing at all would read zero queued writes and
//! zero writes of its own alike.
//!
//! # What a sweep gives the directory
//!
//! A sweep of states over one function pair hands over its stages' two compiled functions, and
//! no pipeline: its pipelines are stored only where re-linking them would translate, and where
//! the directory holds their stages it does not (`Sources::linked_render`). Each creation hands
//! over its variant record too, which names its stages' functions
//! (`ShaderCache::record_variant_pipeline`), a record and no translation. So the second device
//! finds the pair, links each sibling's pipeline itself, and translates nothing.
//!
//! The second test is the durability half, and it is what makes the move honest: a second device
//! over the first one's directory must find every entry the first one's creations handed over, so
//! `Worker::stop` writes what is queued before the cache thread ends. Without that drain the
//! functions would be lost whenever a device went while its thread was behind, and the second
//! device would translate the pair again rather than read it.
//!
//! Each test owns an empty cache directory, removed first and removed again when dropped, so no
//! run reads what an earlier run left.
//!
//! A device test: compiled only under the validation crate's GPU safety cfgs (`m2v_gpu`,
//! `m2v_cpu_icd`), on the kind of device the cfg selects. The functions are the owned fixture
//! `validation/fixtures/public/engine/ubo_bind_offsets.ll` (`llvm-as`).

use super::blit_holds::device;
use super::descriptor_key_once::descriptor;
use crate::{PipelineOptions, ShaderCacheStatistics};
use metal2vulkan::metal::{BlendFactor, BlendOperation};
use std::path::{Path, PathBuf};

/// The states each sweep makes.
const SWEEP: u64 = 16;

/// The compiled functions a sweep's states are all of: one vertex and one fragment. They are
/// every entry the directory takes from a sweep, so they are every entry a second device finds.
const STAGES: u64 = 2;

/// Blend factors no two of which make one pipeline: each state is its own descriptor, so each
/// writes its own entries.
const FACTORS: [BlendFactor; 8] = [
    BlendFactor::Zero,
    BlendFactor::One,
    BlendFactor::SourceColor,
    BlendFactor::OneMinusSourceColor,
    BlendFactor::SourceAlpha,
    BlendFactor::OneMinusSourceAlpha,
    BlendFactor::DestinationColor,
    BlendFactor::DestinationAlpha,
];

/// An empty directory the test owns: removed before it is made, and when dropped.
struct Fresh(PathBuf);

impl Fresh {
    fn new(leg: &str) -> Fresh {
        let path =
            std::env::temp_dir().join(format!("m2v-cache-off-thread-{leg}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        Fresh(path)
    }

    fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for Fresh {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// Make [`SWEEP`] render states of one function pair on a device over `cache`, differing only in
/// their blend factors; the device's cache statistics once they are made. The device is gone when
/// this returns, so its cache thread has written what it was handed.
fn sweep(cache: &Path) -> ShaderCacheStatistics {
    let device = device(cache);
    let base = descriptor(&device);
    let options = PipelineOptions::default();
    let mut kept = Vec::with_capacity(SWEEP as usize);
    for i in 0..SWEEP {
        let mut d = base.clone();
        let color = d.color_attachments[0]
            .as_mut()
            .expect("the fixture's target");
        color.blending_enabled = true;
        color.source_rgb_blend_factor = FACTORS[(i % 8) as usize];
        color.destination_rgb_blend_factor = FACTORS[(i / 8 % 8) as usize];
        color.rgb_blend_operation = BlendOperation::Add;
        kept.push(device.new_render_pipeline_state(&d, &options).unwrap());
    }
    let made = device.shader_cache_statistics();
    drop(kept);
    drop(device);
    made
}

/// Every cache entry a render sweep's creations hand over goes to the device's cache thread: the
/// creating thread makes no directory, no temporary file and no `rename`.
#[test]
fn a_render_sweep_writes_no_cache_file_on_its_creating_thread() {
    let fresh = Fresh::new("sweep");
    let made = sweep(fresh.path());
    assert!(
        made.writes_queued > 0,
        "the sweep must hand cache entries over at all, not {}: a sweep that writes nothing reads \
         zero writes on its own thread for the same reason",
        made.writes_queued
    );
    assert_eq!(
        (made.writes_on_thread, made.write_failures),
        (0, 0),
        "the entries the creating threads wrote themselves, and the failures: {made:?}"
    );
    println!(
        "shader cache off the creating thread: {SWEEP} render creations handed over {} entries \
         and records, {} of them translations, {} bytes written, {} written by a creating thread; \
         {} lookups hit, {} of them from the queue, {} missed",
        made.writes_queued,
        made.writes,
        made.written_bytes,
        made.writes_on_thread,
        made.hits,
        made.queued_hits,
        made.misses
    );
}

/// A second device over the first one's directory finds every artifact the first one's creations
/// handed to its cache thread: the thread writes what is queued before it ends, so a device going
/// while it is behind loses nothing.
#[test]
fn the_entries_a_device_handed_over_are_on_disk_once_it_has_gone() {
    let fresh = Fresh::new("across-devices");
    let first = sweep(fresh.path());
    assert!(
        first.writes > 0 && first.hits == 0 && first.writes_on_thread == 0,
        "the first device must hand every artifact to its cache thread and read none: {first:?}"
    );
    let second = sweep(fresh.path());
    assert_eq!(
        (second.writes, second.write_failures),
        (0, 0),
        "the second device translated something the first one had already stored: {second:?}"
    );
    assert_eq!(
        (second.hits, second.queued_hits),
        (STAGES, 0),
        "the second device's sweep must find the pair's {STAGES} compiled functions in the first \
         one's directory, read from their files and not from a queue of its own: {second:?}"
    );
    assert_eq!(
        second.misses, SWEEP,
        "each of the {SWEEP} states must link its own pipeline, which the directory holds none \
         of: {} lookups found nothing",
        second.misses
    );
}
