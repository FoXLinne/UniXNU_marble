//! What a drawing pipeline's reflection says of a fragment function's **imageblock data**.
//!
//! This family exists for one `MTLBindingType` value. `MTLBindingTypeImageblockData` was one of
//! two the suite could not reach, and the reason it stayed unreached was a wrong belief about
//! where it lives: it was written down as needing a **tile** pipeline's reflection. It does not.
//! Measured on an M3 Max under Metal 27.0.1:
//!
//! - a tile function's imageblock, implicit or explicit, is `MTLBindingTypeImageblock` (17) in
//!   `tileBindings` ([`crate::cases::tile_pipeline_reflection`] records it);
//! - a compute kernel's explicit imageblock is `MTLBindingTypeImageblock` too
//!   (`function-bindings-imageblock`);
//! - `MTLBindingTypeImageblockData` (16) comes from a **fragment** function that takes
//!   `[[imageblock_data]]`, and it arrives in a plain drawing pipeline's `fragmentBindings`.
//!
//! So the call this family needs is the one the suite already had —
//! `newRenderPipelineStateWithDescriptor:options:reflection:error:`, through
//! [`Side::render_pipeline_under_options`] — and all it was missing was a fixture whose fragment
//! function declares imageblock data. `reflection/imageblock_data` is that fixture.
//!
//! # The sweep: `MTLPipelineOption`, 8 of 8
//!
//! The eight defined values, from
//! [`crate::pipeline_option_reflection::MASKS`], the table
//! [`crate::cases::pipeline_option_reflection`] and the two new render families all read rather
//! than copy.
//!
//! **Why no value of it is vacuous.** `MTLPipelineOptionNone` answers no reflection and every mask
//! holding `1 << 0` or `1 << 1` answers one, so the eight sites of the case disagree with each
//! other at their reflection site. A side that ignored its options and always reflected, or never
//! did, fails at least one site.
//!
//! # What each site compares
//!
//! Both stage lists, in the shape [`crate::pipeline_options`] already compares: the vertex
//! function's buffer at index 0, and the fragment function's imageblock data beside its buffer at
//! index 1. The imageblock-data binding carries `MTLBinding`'s own six fields and nothing more,
//! because that is all Metal answers for it — it conforms to none of `MTLBinding`'s public
//! subprotocols, so there is no seventh field to read. Its name, its type, its access, its index
//! and its two bools are still six values an implementation can get wrong, and the index in
//! particular is Metal's own: an imageblock binds no argument-table slot and Metal answers 0 for
//! it rather than the `NSUIntegerMax` it answers for an object payload.
//!
//! The sixteen packed pixel `MTLDataType` values are **not** reachable here and the fixture
//! deliberately states none of them;
//! [`crate::cases::tile_pipeline_reflection`] records that measurement in full.
//!
//! # Why this family is safe
//!
//! One library and eight render pipeline states. Nothing is allocated for the device, no render
//! pass is encoded, no command buffer is made and nothing is drawn, so no case here can fault the
//! device, trip its watchdog or reach a queue. Every options value is one Apple's header names.
//!
//! A device without tile memory refuses the creation, and that is the venue's own gap rather than
//! a refusal of ours: imageblock data is a feature of Apple's tile-memory GPUs, and a driver that
//! has none cannot make the pipeline at all.
//!
//! # Exactness
//!
//! Every field is a number, an enumeration, a name or a bool, and the family declares no
//! tolerance. An enumeration is recorded as its Metal enumerator name beside its ordinal, so a
//! difference reads `MTLBindingTypeImageblockData` against `MTLBindingTypeImageblock` rather than
//! 16 against 17.

use crate::case::{Abort, Case, Failure, Input, MetalSource, Side};
use crate::fixture::{Fixture, METAL_TEXT, RUNTIME_TEXT};
use crate::pipeline_option_reflection::MASKS;
use crate::recording::ShaderRef;
use crate::transcript::{Digest, Node, Observed, Sites};
use metal2vulkan::metal::PixelFormat;

/// The site the library's creation sits at.
const LIBRARY: &str = "library";

/// The fixture whose fragment function takes and returns explicit imageblock data.
const FIXTURE: &str = "reflection/imageblock_data";

/// Its vertex function, which takes a `float2 *` buffer at index 0.
const VERTEX: &str = "reflection_imageblock_vertex";

/// Its fragment function, which takes the imageblock data and a `half &` buffer at index 1.
const FRAGMENT: &str = "reflection_imageblock_fragment";

/// The colour attachment the pipeline declares. `RGBA16Float`, which the fragment function's
/// `half4` output is compatible with: an integer attachment would make Metal refuse the creation
/// and every site would compare a refusal against a refusal.
const ATTACHMENT: PixelFormat = PixelFormat::RGBA16Float;

/// The one case of the family.
pub struct FragmentImageblockReflection;

/// Every case of the family.
pub fn all() -> Vec<FragmentImageblockReflection> {
    vec![FragmentImageblockReflection]
}

impl FragmentImageblockReflection {
    fn loaded() -> Fixture {
        Fixture {
            name: FIXTURE,
            entry: FRAGMENT,
        }
    }
}

impl Case for FragmentImageblockReflection {
    fn name(&self) -> &'static str {
        "fragment-imageblock-reflection"
    }

    fn metal_source(&self) -> MetalSource {
        MetalSource::Run
    }

    fn shaders(&self) -> Result<Vec<ShaderRef>, String> {
        let fixture = Self::loaded();
        [RUNTIME_TEXT, METAL_TEXT]
            .into_iter()
            .map(|extension| {
                Ok(ShaderRef {
                    fixture: format!("{FIXTURE}{extension}"),
                    entry: FRAGMENT.to_owned(),
                    source_sha256: crate::sha256_hex(fixture.text(extension)?.as_bytes()),
                })
            })
            .collect()
    }

    fn inputs(&self) -> Result<Vec<Input>, String> {
        let fixture = Self::loaded();
        // Every mask the case sweeps, so a value added to or taken out of the sweep is a change to
        // the case's inputs rather than a recording that silently covers less.
        let mut masks = Vec::new();
        for (bits, name) in MASKS {
            masks.extend(bits.to_le_bytes());
            masks.extend(name.as_bytes());
        }
        // The two stages and the attachment: the pipeline the reflection is a fact about.
        let mut descriptor = VERTEX.as_bytes().to_vec();
        descriptor.extend(FRAGMENT.as_bytes());
        descriptor.extend(ATTACHMENT.as_raw().to_le_bytes());
        Ok(vec![
            Input {
                name: "air".to_owned(),
                digest: Digest::sha256(fixture.text(RUNTIME_TEXT)?.as_bytes()),
            },
            Input {
                name: "air-typed".to_owned(),
                digest: Digest::sha256(fixture.text(METAL_TEXT)?.as_bytes()),
            },
            Input::bytes("descriptor", &descriptor),
            Input::bytes("options", &masks),
        ])
    }

    fn run(&self, side: &mut dyn Side, sites: &mut Sites) -> Result<(), Abort> {
        let stop = |at: &str, why: String| Abort {
            at: at.to_owned(),
            failure: Failure::named("FixtureNotRead", why),
        };
        let loaded = Self::loaded().loaded().map_err(|why| stop(LIBRARY, why))?;
        let library = crate::case::made(sites, LIBRARY, side.library(&loaded.source()))?;
        // Every mask is asked for, whatever an earlier one answered, for the reason
        // [`crate::cases::pipeline_option_reflection`] states.
        let mut stopped = None;
        for (bits, name) in MASKS {
            let at = format!("options-{name}");
            match side.render_pipeline_under_options(library, VERTEX, FRAGMENT, ATTACHMENT, bits) {
                Ok(found) => sites.push(found.node(at)),
                Err(failure) => {
                    sites.push(Node::leaf(at.clone(), Observed::value(failure.recorded())));
                    stopped = stopped.or(Some(Abort { at, failure }));
                }
            }
        }
        match stopped {
            Some(abort) => Err(abort),
            None => Ok(()),
        }
    }
}
