//! A state's draws build **no** descriptor key (`descriptor_keys_built`): the key of the
//! descriptor a compile is keyed by belongs to that descriptor, and the state holds one per
//! descriptor it compiles as.
//!
//! The creation's half of this is `descriptor_key_once.rs`, which this continues: a creation
//! builds the key of its descriptor once and the state keeps it. What was left there is the
//! draw's half -- a variant a draw of another bind shape needs, compiled under a key built at the
//! draw: the same digest of the same fixed descriptor, built again for every variant.
//!
//! # Which descriptor keys which compile
//!
//! A state can compile as more than one descriptor, and the key is a digest of every field of
//! one of them:
//!
//! * the descriptor it was made from (`Program::descriptor`), which its variants compile from;
//! * the descriptor it draws as (`GraphicsState::descriptor_key`), which for a twin of a color
//!   group holds its group's color attachments alone and clears
//!   `supportIndirectCommandBuffers` -- fields the key reads, so the twin's two descriptors are
//!   two keys.
//!
//! Each key stays with its own descriptor. Keying a compile by a descriptor it does not compile
//! would look its pipeline up among another descriptor's entries in the shader cache, which
//! holds the linked pipelines, the variants and the reflections by that key, so
//! [`a_color_group_twins_descriptors_are_keyed_apart`] pins that those descriptors are keyed
//! apart rather than trusting that one key serves them.
//!
//! # Why these are counts
//!
//! Building a key formats a dozen of the descriptor's fields in their derived `Debug` form and
//! hashes two function compile keys, and nothing of it depends on anything a draw works out. The
//! assertions are counts of builds, never times: a fact of the structure, so they hold on any
//! host under any load and depend on nothing that ran before them in the process.
//!
//! Device tests: compiled only under the validation crate's GPU safety cfgs (`m2v_gpu`,
//! `m2v_cpu_icd`), on the kind of device the cfg selects. The functions are the owned fixture's
//! (`descriptor_key_once::descriptor`).

use super::blit_holds::{device, Scratch};
use super::descriptor_key_once::{built, descriptor};
use crate::pipeline::{BufferForms, GraphicsState, RenderStateKind};
use crate::{PipelineOptions, RenderPipelineState};
use metal2vulkan::bind::BindShape;
use metal2vulkan::metal::PixelFormat;
use metal2vulkan::pipeline::ColorAttachmentDescriptor;
use std::collections::BTreeSet;
use std::sync::Arc;

/// The drawing state of `state`.
fn graphics(state: &RenderPipelineState) -> &GraphicsState {
    match state.kind() {
        RenderStateKind::Graphics(state) => state,
        RenderStateKind::Tile(_) => panic!("a tile state of a render descriptor"),
    }
}

/// The variant a draw of another bind shape needs is compiled under the key the state's creation
/// built: the draw builds none, and a later draw of the shape finds the variant and builds none
/// either. A pass without the pipeline's color attachment, whose writes Metal drops
/// (`BindShape::dropped_colors`), asks for a variant of its own whatever the functions do, and
/// the state compiles it where the key was built again.
#[test]
fn draws_of_other_variants_build_no_descriptor_key() {
    let scratch = Scratch::new("descriptor-key-draws");
    let device = device(&scratch.0);
    let descriptor = descriptor(&device);
    let state = device
        .new_render_pipeline_state(&descriptor, &PipelineOptions::default())
        .unwrap();
    let state = graphics(&state);
    // The base, which the creation compiled: no variant of its own.
    let (base, _) = state
        .settle(&BindShape::default(), BufferForms::Bound)
        .unwrap();
    let mut dropped = BindShape::default();
    dropped.dropped_colors = BTreeSet::from([0]);
    let before = built(&device);
    let (variant, _) = state
        .settle(&dropped, BufferForms::Bound)
        .expect("a pass without the pipeline's attachment settles on a variant of its own");
    let after = built(&device);
    assert!(
        !Arc::ptr_eq(&variant, &base),
        "the shape settled on a variant of its own, not on the base"
    );
    // The stage modules say the compile happened, so what is missing is the repeated digest and
    // not the work it keyed. (`variant_pipelines_made` is a draw's `VkPipeline`, made later.)
    assert!(
        after[1] > before[1],
        "stage modules the variant's compile made: {before:?} -> {after:?}"
    );
    assert_eq!(
        after[0] - before[0],
        0,
        "descriptor keys a draw of another variant built: {before:?} -> {after:?}"
    );
    // The state holds the variant: a later draw of the shape compiles nothing and builds no key.
    let (again, _) = state.settle(&dropped, BufferForms::Bound).unwrap();
    assert!(Arc::ptr_eq(&again, &variant), "the state held the variant");
    assert_eq!(
        built(&device),
        after,
        "the work a second draw of the shape did"
    );
}

/// The key a draw's compile is keyed by is the key of the descriptor the state compiles from,
/// and for a state drawn whole that is the one key its creation built: reading it builds none.
#[test]
fn a_states_compile_key_is_its_descriptors() {
    let scratch = Scratch::new("descriptor-key-compiled");
    let device = device(&scratch.0);
    let descriptor = descriptor(&device);
    let state = device
        .new_render_pipeline_state(&descriptor, &PipelineOptions::default())
        .unwrap();
    let state = graphics(&state);
    let before = built(&device);
    let compiled = *state.compiled_key();
    assert_eq!(
        built(&device)[0],
        before[0],
        "descriptor keys a read of the state's compile key built"
    );
    assert_eq!(
        compiled,
        device.compile_device().render_descriptor_key(&descriptor),
        "the state's compile key is the key of the descriptor it compiles from"
    );
    // Drawn whole: the descriptor it compiles from is the one it draws as, so its one key serves
    // its compiles and the questions asked of it.
    assert_eq!(
        compiled,
        *state.descriptor_key(),
        "a state drawn whole holds one key"
    );
}

/// The two descriptors a twin of a color group holds are keyed apart: the descriptor it was made
/// from, which its variants compile from, and the group's color attachments alone, which it draws
/// as and answers questions of. A compile keyed by the second would be looked up among the
/// entries of a descriptor it is not, so `GraphicsState::compiled_key` keeps a key of each.
///
/// Both differences are pinned: a twin clears `supportIndirectCommandBuffers`, and its drawn
/// descriptor holds its group's attachments at locations `0..`. No device here draws a pipeline
/// in color groups -- a group needs an attachment at or past the lesser of the device's
/// `maxColorAttachments` and `maxFragmentOutputAttachments`, which on every device measured is
/// Metal's attachment count -- so the twin's descriptors are keyed here, where the keys are, and
/// not through a state made of them.
#[test]
fn a_color_group_twins_descriptors_are_keyed_apart() {
    let scratch = Scratch::new("descriptor-key-grouped");
    let device = device(&scratch.0);
    let compile = device.compile_device();
    let mut made_from = descriptor(&device);
    made_from.color_attachments[1] = Some(ColorAttachmentDescriptor::new(PixelFormat::RG16Float));
    made_from.support_indirect_command_buffers = true;
    // The twin's own descriptor (`create_checked`): no twin is a state an indirect command names.
    let mut twin = made_from.clone();
    twin.support_indirect_command_buffers = false;
    // The descriptor a group's twin draws as (`Program::drawn`): attachment `colors()[l]` at
    // location `l`, here the second group of one attachment each.
    let mut drawn = twin.clone();
    drawn.color_attachments = Default::default();
    drawn.color_attachments[0] = made_from.color_attachments[1];
    let keys = [
        compile.render_descriptor_key(&made_from),
        compile.render_descriptor_key(&twin),
        compile.render_descriptor_key(&drawn),
    ];
    assert_ne!(
        keys[0], keys[1],
        "a twin's descriptor is keyed apart from the creation's"
    );
    assert_ne!(
        keys[1], keys[2],
        "a twin's drawn descriptor is keyed apart from the one it compiles from"
    );
    assert_ne!(
        keys[0], keys[2],
        "a twin's drawn descriptor is keyed apart from the creation's"
    );
}
