//! How many times a warm render pipeline state's creation reaches the cache **directory**, and
//! which of those trips a sweep over one vertex-and-fragment pair pays again for every sibling.
//!
//! # Why a count, and why a marginal one
//!
//! A directory trip is one filesystem call: a `stat` of a name (`Store::holds`) or an `open` of
//! an entry's bytes (`Store::read`). On an M3 Max each is single-digit microseconds, so the trips
//! are most of what a warm creation's own CPU goes on
//! (`WorkCounts::shader_cache_read_nanos`), but a time is a figure of the host and its load where
//! a count is a fact of the structure. So these read the count, and print the time beside it.
//!
//! The count that means anything is the **marginal** one. An absolute figure over a sweep holds
//! the first creation's trips, which are the pair's and not a sibling's; it also holds the
//! device's own creations and whatever ran in the process before. So each test here runs [`RUN`]
//! siblings, then [`RUN`] more, and reads the **step** between the two windows. A trip a sibling
//! pays is in both windows; one the pair pays is in neither.
//!
//! # What a sibling must not pay for
//!
//! A sweep's siblings differ only in fixed-function state over one function pair, so a question
//! about the **directory's copy of a stage's function** cannot differ between them: the name it
//! is asked under holds the build, the entry kind, the stage's compile key and the device's
//! profile, and nothing of the pipeline state. `Sources::linked_render` asks it once per stage, to
//! decide whether storing the pipeline's artifact would spare a later process a translation or
//! only a link (`pipeline/sources.rs`), so a sweep of N siblings asked it 2N times for an answer
//! that could not differ. The device now keeps what the directory answered, under that one key
//! (`shader_cache/held.rs`, whose own tests hold the keying: another function and another profile
//! are each asked about, a `false` is never kept, and a removal forgets everything).
//!
//! What a sibling does still pay is every trip whose key really is its own: its linked-use
//! answers, its variant record and its pipeline artifact are keyed by its own descriptor, and
//! each of those keys is one no other sibling has. Those are the equal step the first test reads
//! and does not assert a figure for.
//!
//! # The controls
//!
//! - **A directory that holds nothing is asked every time.** The second test gives a device a
//!   directory that takes no entry at all, so every question is answered `false` and no creation
//!   may keep one: a function absent now is often present a moment later, since the write that
//!   puts it there is the device cache thread's and a creation's link asks after handing its
//!   stage over. Without that half the first test's zero would say nothing about the condition,
//!   and keeping a `false` would make a creation store no artifact for a function the directory
//!   has never held -- the one case where the artifact spares a later process a translation. Its
//!   figure is one question a creation rather than two: `linked_render` asks `all` over the stage
//!   keys, so the first `false` ends the question for that creation.
//! - **The answer itself is held elsewhere.** That the kept answer is still the right one, in
//!   both directions, is `tests::render_stage_digests`: a sweep whose functions the directory
//!   holds encodes no pipeline artifact, and the same sweep against a directory that takes no
//!   entry encodes one per creation.
//!
//! A device test: compiled only under the GPU safety cfgs (`m2v_gpu`, `m2v_cpu_icd`), on the kind
//! of device the cfg selects. The functions are the owned fixture
//! `validation/fixtures/public/engine/ubo_bind_offsets.ll`'s (`llvm-as`).

use super::blit_holds::Scratch;
use super::descriptor_key_once::descriptor;
use crate::{Device, DeviceKind, DeviceOptions, PipelineOptions, ShaderCacheOptions};
use metal2vulkan::metal::{BlendFactor, BlendOperation};
use metal2vulkan::pipeline::RenderPipelineDescriptor;
use std::path::Path;

/// The siblings in each of the two windows.
const RUN: usize = 16;

/// Blend factors no two of which make one pipeline: each sibling is its own descriptor, and so
/// its own `VkPipeline`, while the stages of all of them are one pair.
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

/// A device whose shader cache directory is `cache`, capped at `capacity` bytes. A cap of one
/// byte takes no entry at all, as `tests::render_stage_digests` uses it.
fn device(cache: &Path, capacity: u64) -> Device {
    let options = DeviceOptions {
        kinds: match cfg!(m2v_cpu_icd) {
            true => vec![DeviceKind::Cpu],
            false => vec![
                DeviceKind::Discrete,
                DeviceKind::Integrated,
                DeviceKind::Virtual,
            ],
        },
        shader_cache: ShaderCacheOptions {
            directory: Some(cache.to_owned()),
            capacity,
            ..ShaderCacheOptions::default()
        },
        ..DeviceOptions::default()
    };
    Device::system_default_with(&options).unwrap()
}

/// Sibling `i` of `base`: its own blend state over the same function pair.
fn sibling(base: &RenderPipelineDescriptor, i: usize) -> RenderPipelineDescriptor {
    let mut d = base.clone();
    let color = d.color_attachments[0]
        .as_mut()
        .expect("the fixture's target");
    color.blending_enabled = true;
    color.source_rgb_blend_factor = FACTORS[i % 8];
    color.destination_rgb_blend_factor = FACTORS[i / 8 % 8];
    color.rgb_blend_operation = BlendOperation::Add;
    d
}

/// One window of creations: the questions about an entry, the reads of one, the read time the
/// same calls are counted in, and the pipelines made.
#[derive(Clone, Copy, Debug)]
struct Window {
    questions: u64,
    reads: u64,
    nanos: u64,
    pipelines: u64,
}

impl Window {
    /// The counts as they stand.
    fn of(device: &Device) -> Window {
        let (questions, reads) = device.shared().shader_cache.directory_trips();
        let counts = device.work_counts();
        Window {
            questions,
            reads,
            nanos: counts.shader_cache_read_nanos,
            pipelines: counts.variant_pipelines_made,
        }
    }

    /// What happened between `self` and the earlier `before`.
    fn since(self, before: Window) -> Window {
        Window {
            questions: self.questions - before.questions,
            reads: self.reads - before.reads,
            nanos: self.nanos - before.nanos,
            pipelines: self.pipelines - before.pipelines,
        }
    }
}

/// `count` creations of the descriptors `each` gives, as one window; the states are returned so
/// none is released while a later window runs.
fn window(
    device: &Device,
    count: usize,
    each: impl Fn(usize) -> RenderPipelineDescriptor,
) -> (Window, Vec<crate::RenderPipelineState>) {
    let options = PipelineOptions::default();
    let before = Window::of(device);
    let mut kept = Vec::with_capacity(count);
    for i in 0..count {
        kept.push(
            device
                .new_render_pipeline_state(&each(i), &options)
                .unwrap(),
        );
    }
    (Window::of(device).since(before), kept)
}

/// A sweep over one function pair asks the directory **nothing** about its stages' functions, and
/// the trips it does make are the ones keyed by its own descriptors, the same number in each
/// window.
#[test]
fn a_sweep_over_one_function_pair_asks_about_its_stages_once() {
    let scratch = Scratch::new("render-directory-trips-pair");
    let device = device(&scratch.0, ShaderCacheOptions::default().capacity);
    let base = descriptor(&device);
    let options = PipelineOptions::default();
    // The first creation is the one that translates the pair and hands it to the directory. Its
    // own questions are not a figure of anything: the link asks after the handover, so the write
    // queue may answer them without a filesystem call at all (`Store::holds`).
    let before_first = Window::of(&device);
    let _first = device
        .new_render_pipeline_state(&sibling(&base, 0), &options)
        .unwrap();
    let first = Window::of(&device).since(before_first);
    let (one, kept_one) = window(&device, RUN, |i| sibling(&base, 1 + i));
    let (two, kept_two) = window(&device, RUN, |i| sibling(&base, 1 + RUN + i));
    println!(
        "RENDER-DIRECTORY-TRIPS: the pair's first creation asked {} questions and read {} entries \
         in {} us; two windows of {RUN} siblings: questions {} then {}, entry reads {} then {}, \
         read time {} then {} us, pipelines {} then {}",
        first.questions,
        first.reads,
        first.nanos / 1000,
        one.questions,
        two.questions,
        one.reads,
        two.reads,
        one.nanos / 1000,
        two.nanos / 1000,
        one.pipelines,
        two.pipelines
    );
    assert_eq!(
        (one.pipelines, two.pipelines),
        (RUN as u64, RUN as u64),
        "every sibling must make a pipeline of its own, else the run is not the sweep this \
         measures"
    );
    assert_eq!(
        (one.questions, two.questions),
        (0, 0),
        "{RUN} siblings over one function pair asked the directory about their stages' functions \
         {} and {} times, over the two stage keys every one of them links under",
        one.questions,
        two.questions
    );
    // The floor under that zero: these creations do reach the directory, for the entries their
    // own descriptors name, so the zero is a question not asked and not a directory out of reach.
    assert!(
        one.reads > 0 && two.reads > 0,
        "a sibling must still read the directory for the entries keyed by its own descriptor: {} \
         and {} reads over {RUN} creations each. A sweep that reached the directory for nothing \
         at all would read zero questions for that reason instead",
        one.reads,
        two.reads
    );
    assert_eq!(
        one.reads, two.reads,
        "two windows of {RUN} siblings of one pair read the directory {} and {} times: a \
         sibling's own trips are a function of its own descriptor alone, so the step between N \
         and 2N siblings is the per-sibling cost and must not move",
        one.reads, two.reads
    );
    drop((kept_one, kept_two));
}

/// A directory that holds nothing is asked about every creation's stages: a `false` is no answer
/// about the key, since the write that would make it `true` is the device cache thread's.
#[test]
fn a_directory_that_holds_nothing_is_asked_again() {
    let scratch = Scratch::new("render-directory-trips-none");
    // A cap of one byte takes no entry at all: every write is larger than the directory's own
    // limit, so nothing lands and every question about a function is answered `false`.
    let device = device(&scratch.0, 1);
    let base = descriptor(&device);
    let (one, kept_one) = window(&device, RUN, |i| sibling(&base, i));
    let (two, kept_two) = window(&device, RUN, |i| sibling(&base, RUN + i));
    println!(
        "RENDER-DIRECTORY-TRIPS: {RUN} siblings against a directory that takes no entry: \
         questions {} then {}, entry reads {} then {}, read time {} then {} us",
        one.questions,
        two.questions,
        one.reads,
        two.reads,
        one.nanos / 1000,
        two.nanos / 1000
    );
    assert!(
        one.questions >= RUN as u64 && two.questions >= RUN as u64,
        "every creation against a directory that holds none of its functions must ask about them \
         again: {} and {} questions over {RUN} creations each. Keeping a `false` would make a \
         creation store no pipeline artifact for a function the directory has never held, which \
         is the one case the artifact spares a later process a translation",
        one.questions,
        two.questions
    );
    drop((kept_one, kept_two));
}
