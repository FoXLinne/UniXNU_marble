//! What one render pipeline state's creation costs end to end, over the shape a sweep of blend
//! states has: [`SWEEP`] states of one vertex-and-fragment pair, differing only in their blend
//! factors, each timed on the calling thread.
//!
//! # What this reports, and what it asserts
//!
//! The report is the creation-level total a program sees: the wall time of
//! `new_render_pipeline_state`, per creation, as a median over the sweep. It is the figure every
//! attribution of a creation's parts is a share of, and the only one that says whether removing a
//! part of a creation's work removed any of its cost. The parts themselves are the creation log's
//! (`METAL2VULKAN_LOG_PIPELINES`, `pipeline/creation_log.rs`): a run of this test with the log on
//! prints each creation's `made in`, and its split into `content`, `record`, `turn`, `infos`, the
//! driver's `pipeline` and `kept`, so the making's share of the median is read from the two runs
//! together. Nothing here measures the parts itself, so the log stays the one place that names
//! them.
//!
//! A time is a figure of the host it was taken on, under the load it was taken under, so the test
//! asserts none. What it asserts are the two counts that keep the report from being a figure of
//! something other than a creation: that every creation after the warm-up's handed its cache
//! entries to the device's cache thread (`writes_on_thread == 0`, so no creation's median holds a
//! journalled filesystem write), and that the sweep handed over entries at all
//! (`writes_queued > 0`, without which the first count would read zero for a sweep that stored
//! nothing). `shader_cache_off_thread.rs` holds that move as its own contract over its own sweep;
//! here the counts say which cost the printed median is and is not made of.
//!
//! It also prints the directory's own share of that median: the read time of the same window and
//! the round trips it is the time of (`tests::render_directory_trips` reads those counts
//! marginally, which is where a claim about them belongs; the figure here is what they cost
//! against a whole creation on this host).
//!
//! # The warm-up, and why the median is warm
//!
//! Metal's compiler service keys its compiled output on the function's AIR and keeps it across
//! processes, so the first creation of a pair in a fresh shader cache pays a real compile and
//! every later creation of the same pair reads it back -- two orders of magnitude apart. The sweep
//! therefore makes [`WARM`] states before it times any, and prints the warm-up's own times beside
//! the median so a reader can see which side of that cache the median is on. The median is warm by
//! construction. A cold figure needs a function the service has never seen, which means varying
//! something that reaches the AIR, not a comment.
//!
//! The device takes a shader cache directory of its own, empty, so no run reads a record an
//! earlier run stored.
//!
//! A device test: compiled only under the GPU safety cfgs (`m2v_gpu`, `m2v_cpu_icd`), on the kind
//! of device the cfg selects. The functions are the owned fixture
//! `validation/fixtures/public/engine/ubo_bind_offsets.ll`'s (`llvm-as`).

use super::blit_holds::{device, Scratch};
use super::descriptor_key_once::descriptor;
use crate::PipelineOptions;
use metal2vulkan::metal::{BlendFactor, BlendOperation};
use std::time::{Duration, Instant};

/// The timed creations.
const SWEEP: usize = 32;

/// The creations before the timed ones: the first compiles the pair's functions, and the second
/// is the first creation that reads that compile back, so the timed ones are every one after a
/// creation of the same shape has already run.
const WARM: usize = 2;

/// Blend factors no two of which make one pipeline: each state is its own descriptor, and so its
/// own `VkPipeline`, while the record, the cache and the translation of all of them are one.
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

/// `times` in microseconds, as its median, its smallest, its largest and its total.
fn figures(times: &[Duration]) -> (u128, u128, u128, u128) {
    let mut sorted: Vec<u128> = times.iter().map(Duration::as_micros).collect();
    sorted.sort_unstable();
    let median = sorted[sorted.len() / 2];
    let total: u128 = sorted.iter().sum();
    (median, sorted[0], sorted[sorted.len() - 1], total)
}

/// A sweep of render pipeline state creations over one function pair, differing only in blend
/// state: each creation's wall time, and the counts saying what the times are made of.
#[test]
fn what_a_render_pipeline_state_creation_costs_over_one_function_pair() {
    let scratch = Scratch::new("render-creation-cost");
    let device = device(&scratch.0);
    let base = descriptor(&device);
    let options = PipelineOptions::default();
    let sibling = |i: usize| {
        let mut d = base.clone();
        let color = d.color_attachments[0]
            .as_mut()
            .expect("the fixture's target");
        color.blending_enabled = true;
        color.source_rgb_blend_factor = FACTORS[i % 8];
        color.destination_rgb_blend_factor = FACTORS[i / 8 % 8];
        color.rgb_blend_operation = BlendOperation::Add;
        d
    };
    let mut kept = Vec::with_capacity(WARM + SWEEP);
    let mut warm_up = Vec::with_capacity(WARM);
    for i in 0..WARM {
        let at = Instant::now();
        let state = device
            .new_render_pipeline_state(&sibling(i), &options)
            .unwrap();
        warm_up.push(at.elapsed());
        kept.push(state);
    }
    let before = device.shader_cache_statistics();
    let before_directory = device.work_counts().shader_cache_read_nanos;
    let before_trips = device.shared().shader_cache.directory_trips();
    let mut times = Vec::with_capacity(SWEEP);
    for i in 0..SWEEP {
        let at = Instant::now();
        let state = device
            .new_render_pipeline_state(&sibling(WARM + i), &options)
            .unwrap();
        times.push(at.elapsed());
        kept.push(state);
    }
    let after = device.shader_cache_statistics();
    let directory = device.work_counts().shader_cache_read_nanos - before_directory;
    let trips = device.shared().shader_cache.directory_trips();
    let (median, least, most, total) = figures(&times);
    let warm_up: Vec<u128> = warm_up.iter().map(Duration::as_micros).collect();
    println!(
        "RENDER-CREATION-COST: {SWEEP} creations of one function pair, warm: median {median} us, \
         least {least} us, most {most} us, {total} us in all; the {WARM} warm-up creations took \
         {warm_up:?} us"
    );
    let queued = after.writes_queued - before.writes_queued;
    let on_thread = after.writes_on_thread - before.writes_on_thread;
    println!(
        "RENDER-CREATION-COST: over the timed creations the cache queued {queued} entries, wrote \
         {on_thread} on a creating thread, {} failed; {} lookups hit and {} missed",
        after.write_failures - before.write_failures,
        after.hits - before.hits,
        after.misses - before.misses
    );
    assert!(
        queued > 0,
        "the timed creations must hand cache entries to the device's cache thread at all, not \
         {queued}: a sweep that stored nothing would read zero writes on its own thread for the \
         same reason, and its median would say nothing about where a creation's time goes"
    );
    assert_eq!(
        on_thread, 0,
        "no timed creation may write a cache file on its own thread, so no creation's time holds \
         a journalled filesystem write: {on_thread} did"
    );
    // The directory's share of the median, which is the one part of a creation's own CPU that is
    // attributable to the call rather than to the driver: the same window's read time, over the
    // round trips it is the time of (`tests::render_directory_trips` reads the counts marginally).
    println!(
        "RENDER-CREATION-COST: the timed creations read the directory for {} us in all, {} us a \
         creation, over {} questions about an entry and {} reads of one",
        directory / 1000,
        directory / 1000 / SWEEP as u64,
        trips.0 - before_trips.0,
        trips.1 - before_trips.1
    );
}
