//! What a run of render pipeline creations over one vertex-and-fragment pair derives **again**
//! for words it has already seen: the digest that names each stage's module, the encoding of the
//! pipeline artifact, and the weight the memo bounds itself by.
//!
//! Three counts, one creation-time cost each. All three are counts rather than times, so they
//! are facts of the structure and hold on any host under any load.
//!
//! # The stage digests
//!
//! A variant's objects name each stage's module by the SHA-256 of its exact SPIR-V
//! (`objects::words_digest`), which is also what the makings' records and the pipeline libraries'
//! parts are named by. A sweep of states over one function pair takes the same words on every
//! creation -- the compiled function's relocation slot hands back the same allocation -- so
//! hashing them once a creation hashes bytes identical to the previous sibling's, over every word
//! of every stage. The function now carries that digest beside the words
//! (`CompiledFunction::relocated_words_digest`), and the creation asks it rather than hashing.
//!
//! The first creation's own count stands beside the siblings' zero: a run that derived nothing
//! because nothing reached the digest at all would read zero for the same reason.
//!
//! # The pipeline artifact
//!
//! A creation that links a pipeline itself encodes the whole artifact -- every word of every
//! stage again -- to give the directory. Where the directory already holds the stages' compiled
//! functions, a later process that does not find the pipeline links it from those, which costs no
//! translation, so the entry spares a link and the encoding buys nothing
//! (`Sources::linked_render`). The zero is paired with a device whose directory takes no entry at
//! all, where every creation encodes its artifact as every creation once did: without that half
//! the zero would say nothing about the condition.
//!
//! # The variant record
//!
//! Beside what it links the directory takes a record of the keys a creation of this descriptor
//! learned for this variant: its pipeline's, and its stages' functions'
//! (`ShaderCache::record_variant_pipeline`). A later process that finds neither in its memo
//! looks the pipeline up by the one, else links the functions it finds by the others, so it
//! decodes and specializes no stage's AIR, which learning the keys does (most of a warm
//! creation's own time for a large fragment function). A sweep's
//! pipelines are the ones the directory does not take, so its records are what spare that
//! process the decodes: each creation writes one, and a second device over the directory links
//! the whole sweep with no stage decoded, where a directory that takes no entry leaves the second
//! device decoding the pair again.
//!
//! # The memo's weight
//!
//! The memo bounds itself by bytes, and what a creation gives it is the length of the encoding it
//! made. A creation that makes no encoding has no such length, and weighing its pipeline at the
//! zero bytes it did not encode would let 4096 entries of a few hundred kilobytes each sit in a
//! memo that believed its bound untouched. The pipeline is weighed by what it retains instead
//! (`RenderPipeline::footprint`), which is the same quantity measured on the live object.
//!
//! Each device takes a shader cache directory of its own, empty, so no run reads an entry an
//! earlier run stored.
//!
//! A device test: compiled only under the GPU safety cfgs (`m2v_gpu`, `m2v_cpu_icd`), on the kind
//! of device the cfg selects. The functions are the owned fixture
//! `validation/fixtures/public/engine/ubo_bind_offsets.ll`'s (`llvm-as`).

use super::blit_holds::Scratch;
use super::descriptor_key_once::descriptor;
use crate::pipeline::objects::words_digests_derived;
use crate::pipeline::render_artifacts_encoded;
use crate::shader_cache::variant_records_written;
use crate::{Device, DeviceKind, DeviceOptions, PipelineOptions, ShaderCacheOptions};
use metal2vulkan::metal::{BlendFactor, BlendOperation};
use metal2vulkan::pipeline::{RenderPipeline, RenderPipelineDescriptor};
use std::path::Path;

/// The siblings a run makes after its warm-up's one.
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
/// byte takes no entry at all: every write is larger than the directory's own limit, so nothing
/// is queued and nothing lands, and the creations meet a directory that holds none of their
/// functions.
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

/// A device whose shader cache directory takes no entry at all.
fn capped_device(cache: &Path) -> Device {
    device(cache, 1)
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

/// [`RUN`] sibling creations over one function pair derive **no** digest of their stages' words:
/// the first creation derived them, and every sibling's stages are the very words it derived them
/// over.
#[test]
fn a_run_of_sibling_creations_derives_its_stage_digests_once() {
    let scratch = Scratch::new("render-stage-digests-once");
    let device = device(&scratch.0, ShaderCacheOptions::default().capacity);
    let base = descriptor(&device);
    let options = PipelineOptions::default();
    let before_first = words_digests_derived();
    let _first = device
        .new_render_pipeline_state(&sibling(&base, 0), &options)
        .unwrap();
    let first = words_digests_derived() - before_first;
    assert!(
        first > 0,
        "the first creation must derive its stages' digests at all, not {first} of them: a \
         creation that derives none, because nothing of it names a module by its words, makes \
         the run's zero a zero that measures nothing"
    );
    let counts = device.work_counts();
    let mut kept = Vec::with_capacity(RUN);
    for i in 0..RUN {
        kept.push(
            device
                .new_render_pipeline_state(&sibling(&base, 1 + i), &options)
                .unwrap(),
        );
    }
    let derived = words_digests_derived() - before_first - first;
    let made = device.work_counts().variant_pipelines_made - counts.variant_pipelines_made;
    assert_eq!(
        made, RUN as u64,
        "every sibling must make a pipeline of its own, else the run is not the sweep this \
         measures: {made} pipelines made over {RUN} creations"
    );
    assert_eq!(
        derived, 0,
        "{RUN} sibling creations over one function pair derived {derived} digests of their \
         stages' words, over words the first creation's {first} digests were taken over"
    );
}

/// A sweep whose stages' functions the directory holds encodes **no** pipeline artifact, and the
/// same sweep against a directory that takes no entry encodes one per creation.
#[test]
fn a_sweep_encodes_a_pipeline_artifact_only_where_its_functions_are_absent() {
    let held = Scratch::new("render-stage-digests-held");
    let device = device(&held.0, ShaderCacheOptions::default().capacity);
    let base = descriptor(&device);
    let options = PipelineOptions::default();
    let before = render_artifacts_encoded();
    let mut kept = Vec::with_capacity(RUN);
    for i in 0..RUN {
        kept.push(
            device
                .new_render_pipeline_state(&sibling(&base, i), &options)
                .unwrap(),
        );
    }
    let encoded = render_artifacts_encoded() - before;
    assert_eq!(
        encoded, 0,
        "a sweep of {RUN} creations whose stages' functions the directory holds encoded \
         {encoded} pipeline artifacts, each of them every word of every stage again"
    );
    // The half that says the zero is a fact about the condition: a directory that takes no entry
    // holds none of the stages' functions, so each creation's artifact is what would spare a
    // later process its translations and each creation stores it.
    let none = Scratch::new("render-stage-digests-none");
    let capped = capped_device(&none.0);
    let base = descriptor(&capped);
    let before = render_artifacts_encoded();
    let mut kept = Vec::with_capacity(RUN);
    for i in 0..RUN {
        kept.push(
            capped
                .new_render_pipeline_state(&sibling(&base, i), &options)
                .unwrap(),
        );
    }
    let encoded = render_artifacts_encoded() - before;
    assert_eq!(
        encoded, RUN as u64,
        "a sweep of {RUN} creations against a directory holding none of their functions must \
         encode its artifact for each of them, not {encoded}"
    );
}

/// [`RUN`] sibling creations on `device`, kept until the device's statistics are read: its
/// work counts once they are made.
fn swept(device: &Device) -> crate::WorkCounts {
    let base = descriptor(device);
    let options = PipelineOptions::default();
    let kept: Vec<_> = (0..RUN)
        .map(|i| {
            device
                .new_render_pipeline_state(&sibling(&base, i), &options)
                .unwrap()
        })
        .collect();
    let counts = device.work_counts();
    drop(kept);
    counts
}

/// A sweep gives the directory a variant record per creation, and a second device over the same
/// directory links every sibling from the stages' functions those records name, decoding no
/// stage and translating nothing; against a directory that takes no entry, the second device
/// decodes the pair again.
#[test]
fn a_second_device_links_a_recorded_sweep_with_no_stage_decoded() {
    let held = Scratch::new("render-variant-records-held");
    let capacity = ShaderCacheOptions::default().capacity;
    let before = variant_records_written();
    let first = device(&held.0, capacity);
    let made = swept(&first);
    // Gone, so its cache thread has written what the sweep handed it.
    drop(first);
    let written = variant_records_written() - before;
    assert_eq!(
        written, RUN as u64,
        "a sweep of {RUN} creations must record each one's keys, not {written}"
    );
    assert!(
        made.stage_entries_specialized > 0,
        "the first device must decode the pair at all, not {} stages: a sweep that decodes \
         nothing makes the second device's zero a zero that measures nothing",
        made.stage_entries_specialized
    );
    let second = device(&held.0, capacity);
    let start = second.work_counts();
    let encoded_before = render_artifacts_encoded();
    let after = swept(&second);
    assert_eq!(
        (
            after.stage_entries_specialized - start.stage_entries_specialized,
            after.functions_translated - start.functions_translated,
        ),
        (0, 0),
        "the second device decoded or translated stages the first one's records name"
    );
    assert_eq!(
        after.variant_pipelines_made - start.variant_pipelines_made,
        RUN as u64,
        "every sibling must make a pipeline of its own, else the sweep is not the one this \
         measures"
    );
    assert_eq!(
        render_artifacts_encoded(),
        encoded_before,
        "the second device linked from the functions the directory holds, and stores no artifact"
    );
    drop(second);
    // The half that says the zero is the records': a directory that takes no entry holds no
    // record, and the second device decodes the pair again.
    let none = Scratch::new("render-variant-records-none");
    swept(&capped_device(&none.0));
    let second = capped_device(&none.0);
    let start = second.work_counts();
    let after = swept(&second);
    assert!(
        after.stage_entries_specialized > start.stage_entries_specialized,
        "a device over a directory that holds no record must decode the pair again"
    );
}

/// The memo weighs a pipeline whose artifact its creation did not encode by what the pipeline
/// retains, never by the zero bytes of the encoding that was not made.
#[test]
fn the_memo_weighs_a_pipeline_it_did_not_encode_by_what_it_retains() {
    let scratch = Scratch::new("render-stage-digests-weight");
    let device = device(&scratch.0, ShaderCacheOptions::default().capacity);
    let base = descriptor(&device);
    let options = PipelineOptions::default();
    let memo = || device.shared().shader_cache.memo.footprint();
    // The warm-up puts the pair's two compiled functions in the memo, each weighed at its own
    // encoded length, so the window below holds render pipelines alone and the weight it reads
    // is theirs. Its own pipeline is an entry too, which is why the count is taken after it.
    let mut kept = Vec::with_capacity(RUN + 1);
    kept.push(
        device
            .new_render_pipeline_state(&sibling(&base, 0), &options)
            .unwrap(),
    );
    let before = memo();
    let encoded_before = render_artifacts_encoded();
    for i in 0..RUN {
        kept.push(
            device
                .new_render_pipeline_state(&sibling(&base, 1 + i), &options)
                .unwrap(),
        );
    }
    let encoded = render_artifacts_encoded() - encoded_before;
    let (weight, entries) = memo();
    let (weight_before, entries_before) = before;
    let kept_entries = entries - entries_before;
    let kept_weight = weight - weight_before;
    assert_eq!(
        encoded, 0,
        "the sweep's creations must keep their pipelines without encoding them, else the memo is \
         weighing encodings and this says nothing: {encoded} artifacts encoded"
    );
    assert_eq!(
        kept_entries, RUN,
        "each of the {RUN} siblings' pipelines must become an entry of its own: {kept_entries} \
         entries kept"
    );
    // A weight of zero -- the length of the encoding that was not made -- would let the memo
    // hold its 4096 entries believing its byte bound untouched. Every one of these entries is a
    // render pipeline, and a pipeline's own storage is a floor its footprint must pass, since
    // each has stages and every stage has words.
    let floor = kept_entries as u64 * size_of::<RenderPipeline>() as u64;
    assert!(
        kept_weight > floor,
        "the memo must weigh each pipeline it kept by what the pipeline retains: {kept_weight} \
         bytes over {kept_entries} entries, not past the {floor} bytes their own storage alone is"
    );
    println!(
        "RENDER-STAGE-DIGESTS: {kept_entries} pipelines kept unencoded weigh {kept_weight} bytes \
         in the memo, {} bytes each",
        kept_weight / kept_entries as u64
    );
}
