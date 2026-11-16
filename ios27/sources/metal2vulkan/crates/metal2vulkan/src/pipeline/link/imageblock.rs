//! A render pipeline's fragment imageblock: `GraphicsState::fragment_imageblock` from the fragment
//! stage's `RuntimeFact::FragmentImageblock`.
//!
//! The fragment function reads and writes the pass's imageblock storage, one plane of cells per
//! pixel, and per sample in a multisampled pass (`ImageblockMechanism::StorageImageInterlock`),
//! and writes it inside its body. Its depth and stencil tests run before its write: its module
//! declares early tests, or each draw's plan checks them (`RuntimeFact::TestedStorageWrites`,
//! `lower::fragment::imageblock`). Link refuses:
//!
//! - a function compiled for another sample count than the pipeline's (`NeedsInput(Multisampled)`):
//!   one compiled for a multisampled pipeline reaches its own sample's cell, one compiled for one
//!   sample the pixel's;
//! - as not implemented, a pipeline whose alpha to coverage can drop a fragment after the function
//!   wrote its imageblock: Metal drops the write with the fragment (measured on the M3 Max, at one
//!   sample and at four), and the storage already holds it.

use crate::error::{Error, Feature, RuntimeInput, Unsupported};
use crate::library::FunctionArtifact;
use crate::pipeline::facts::RuntimeFact;
use crate::pipeline::graphics::{FragmentImageblock, ImageblockMechanism};
use crate::pipeline::link::RenderLinkInputs;

/// The fragment imageblock of render inputs `r` whose fragment stage is `fragment` (see the
/// module docs).
pub fn fragment_imageblock(
    r: &RenderLinkInputs,
    fragment: Option<&FunctionArtifact>,
) -> Result<Option<FragmentImageblock>, Error> {
    let Some(fragment) = fragment else {
        return Ok(None);
    };
    let Some((words, writes, master, data_size, alignment)) =
        fragment.facts.iter().find_map(|fact| match *fact {
            RuntimeFact::FragmentImageblock {
                words,
                writes,
                master,
                data_size,
                alignment,
            } => Some((words, writes, master, data_size, alignment)),
            _ => None,
        })
    else {
        return Ok(None);
    };
    if fragment.interface.io.multisampled != (r.raster_sample_count > 1) {
        return Err(Error::NeedsInput(RuntimeInput::Multisampled));
    }
    if writes && r.alpha_to_coverage {
        return Err(Unsupported::not_implemented(Feature::Imageblocks).into());
    }
    Ok(Some(FragmentImageblock {
        mechanism: ImageblockMechanism::StorageImageInterlock,
        words,
        master,
        data_size,
        alignment,
    }))
}
