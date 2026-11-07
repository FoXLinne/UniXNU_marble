//! What a tile render pipeline **descriptor** says about itself, and what `reset` puts back.
//!
//! `MTLTileRenderPipelineDescriptor` was exercised at four of its fifteen items, and all four by
//! `tile_threadgroups` **making pipelines** with it: the tile function, the colour-attachment
//! array, `threadgroupSizeMatchesTileSize` and `maxTotalThreadsPerThreadgroup`. Nothing read a
//! getter of the descriptor back, so the label, the raster sample count, the per-buffer mutability,
//! the call-stack depth, the binary-function support, the required threadgroup size, the lists of
//! supplied objects and `reset` were reached by nothing at all.
//!
//! The runtime holds each of those defaults as a literal in
//! [`TileRenderPipelineDescriptor::new`](metal2vulkan::pipeline::TileRenderPipelineDescriptor::new),
//! read off Apple's documentation, and nothing in the tree ran Metal to check one.
//! [`crate::tile_descriptors`] says which claim each getter is; this family is the three runs that
//! hold them.
//!
//! # The shapes, and why each is here
//!
//! - **the descriptor of a tile function alone** (`tile-descriptor-defaults`): Metal's default for
//!   every other field, which is the one shape that can say what a program gets for free;
//! - **the descriptor with every field stated** (`tile-descriptor-set`): each field away from its
//!   default and no two stated numbers equal ([`crate::tile_descriptors::AWAY`]), read straight
//!   back. The colour attachments state slots 0 and 2, so slot 1's answer is Metal's own default
//!   for a tile attachment recorded beside two that were set;
//! - **the same descriptor after `reset`** (`tile-descriptor-reset`): every field stated, then
//!   `reset`, then the tile function set again. Its answer must be the first shape's, which is the
//!   only way to say "a reset descriptor is a fresh one" with both shapes in one recording. It
//!   reads every field the first shape does **except** three, and each exclusion is itself a
//!   measurement:
//!   - the two lists of supplied objects, for the reason
//!     [`crate::render_descriptors::SuppliedLists`] states: `reset` leaves `preloadedLibraries`
//!     nil where `init` leaves a non-nil empty array, and `objc2-metal` 0.3.2 declares the
//!     property non-null and panics on the nil;
//!   - `tileBuffers`, because **Metal's `reset` corrupts it**. A descriptor whose `tileBuffers`
//!     array has been subscripted and then `reset` answers an object that is not a pipeline buffer
//!     array, and reading one of its elements' `mutability` ends the process. Nothing of this is
//!     true of `MTLComputePipelineDescriptor.buffers` or of `MTLRenderPipelineDescriptor`'s two
//!     arrays, whose own families read them on their reset shapes;
//!     [`TileDescriptorFacts::buffers`](crate::tile_descriptors::TileDescriptorFacts::buffers)
//!     holds the measurement and how it was narrowed.
//!
//! The runtime has no mutable descriptor, so its `reset` is `TileRenderPipelineDescriptor::new`
//! again (the coverage map says so), and the case makes our side build the stated descriptor and
//! then replace it — rather than skip the setting, which would leave nothing for the reset to undo.
//!
//! # Exactness
//!
//! Every field is a number, a bool, a name or an ordinal. The family declares no tolerance and
//! announces nothing: no case touches a texture, a sampler, a float or the GPU.
//!
//! Three items are compared **at their default alone** — `binaryArchives`, `preloadedLibraries`
//! and `linkedFunctions` — because the runtime takes each of them somewhere other than the
//! descriptor and a count is the one shape both sides can state. The two per-stage ones are
//! compared on the first two shapes and not the third. [`crate::tile_descriptors`] gives the
//! reasoning; nothing is widened for it.
//!
//! # Why this family is safe
//!
//! No shape compiles a pipeline, allocates a resource, makes a command buffer or reaches a queue:
//! each makes one library, one function of it, and a descriptor it reads and drops. Two hazards are
//! named and avoided: `setLabel:` with nil ends Metal's process and no shape sends it, and
//! `tileFunction` is non-null in `objc2-metal` although a fresh descriptor's is nil, so every shape
//! sets the tile function before reading and sets it again after the reset.

use crate::case::{Abort, Case, Failure, Input, MetalSource, Side};
use crate::fixture::{Fixture, METAL_TEXT, RUNTIME_TEXT};
use crate::recording::ShaderRef;
use crate::render_descriptors::Shape;
use crate::tile_descriptors::DescriptorSpec;
use crate::transcript::{Digest, Node, Observed, Sites};

/// The site the library's make sits at.
const LIBRARY: &str = "library";

/// The site the descriptor's own answers sit at.
const FACTS: &str = "facts";

/// The fixture whose tile function records which thread is which: `tile_threadgroups`' own, so
/// this family needs no fixture of its own.
const FIXTURE: &str = "k10ts_tile_order";

/// A case over one tile render pipeline descriptor's own facts.
pub struct TileDescriptorCase {
    name: &'static str,
    spec: DescriptorSpec<'static>,
}

/// Every case of the family.
pub fn all() -> Vec<TileDescriptorCase> {
    vec![
        // The floor: a descriptor carrying one tile function. Every other field is Metal's own
        // default, and no case before this one asked Metal for any of them.
        TileDescriptorCase {
            name: "tile-descriptor-defaults",
            spec: DescriptorSpec::new(FIXTURE, Shape::Defaults),
        },
        // Every field stated, each away from its default, read straight back.
        TileDescriptorCase {
            name: "tile-descriptor-set",
            spec: DescriptorSpec::new(FIXTURE, Shape::Set),
        },
        // The same, then `reset`, then the tile function again: the answer must be the first
        // case's, field for field.
        TileDescriptorCase {
            name: "tile-descriptor-reset",
            spec: DescriptorSpec::new(FIXTURE, Shape::Reset),
        },
    ]
}

impl Case for TileDescriptorCase {
    fn name(&self) -> &'static str {
        self.name
    }

    fn metal_source(&self) -> MetalSource {
        MetalSource::Run
    }

    fn shaders(&self) -> Result<Vec<ShaderRef>, String> {
        let fixture = Fixture::new(FIXTURE);
        [RUNTIME_TEXT, METAL_TEXT]
            .into_iter()
            .map(|extension| {
                Ok(ShaderRef {
                    fixture: format!("{FIXTURE}{extension}"),
                    entry: FIXTURE.to_owned(),
                    source_sha256: crate::sha256_hex(fixture.text(extension)?.as_bytes()),
                })
            })
            .collect()
    }

    fn inputs(&self) -> Result<Vec<Input>, String> {
        let fixture = Fixture::new(FIXTURE);
        Ok(vec![
            Input {
                name: "air".to_owned(),
                digest: Digest::sha256(fixture.text(RUNTIME_TEXT)?.as_bytes()),
            },
            Input {
                name: "air-typed".to_owned(),
                digest: Digest::sha256(fixture.text(METAL_TEXT)?.as_bytes()),
            },
            // The spec is the whole of the rest of what the case feeds: the tile function, the
            // shape, and — where the shape states anything — the stated table itself.
            Input::bytes("descriptor", &self.spec.words()),
            // The slot bounds the facts are read over, so a record of three attachments compared
            // against a record of two is a different observation.
            Input::bytes(
                "slots",
                &[
                    crate::render_descriptors::READ_COLORS,
                    crate::render_descriptors::READ_BUFFERS,
                ]
                .iter()
                .flat_map(|bound| bound.to_le_bytes())
                .collect::<Vec<u8>>(),
            ),
        ])
    }

    fn run(&self, side: &mut dyn Side, sites: &mut Sites) -> Result<(), Abort> {
        let stop = |at: &str, why: String| Abort {
            at: at.to_owned(),
            failure: Failure::named("FixtureNotRead", why),
        };
        let loaded = Fixture::new(FIXTURE)
            .loaded()
            .map_err(|why| stop(LIBRARY, why))?;
        let library = crate::case::made(sites, LIBRARY, side.library(&loaded.source()))?;
        match side.tile_descriptor_facts(library, &self.spec) {
            Ok(facts) => sites.push(facts.node(FACTS)),
            Err(failure) => {
                sites.push(Node::leaf(FACTS, Observed::value(failure.recorded())));
                return Err(Abort {
                    at: FACTS.to_owned(),
                    failure,
                });
            }
        }
        Ok(())
    }
}

/// The stated table holds no two equal numbers among the ones a getter could be crossed with, and
/// every slot it names is inside the bounds the facts are read over.
#[cfg(test)]
mod tests {
    use super::*;
    use crate::tile_descriptors::AWAY;

    /// Every number the set shape states is distinct, so a side answering one getter out of
    /// another's field reads a different value.
    #[test]
    fn the_stated_numbers_are_all_different() {
        let numbers = [
            AWAY.raster_sample_count,
            AWAY.max_total_threads_per_threadgroup,
            AWAY.max_call_stack_depth,
            AWAY.required_threads_per_threadgroup[0],
            AWAY.required_threads_per_threadgroup[1],
            AWAY.required_threads_per_threadgroup[2],
        ];
        let unique: std::collections::BTreeSet<u32> = numbers.iter().copied().collect();
        assert_eq!(
            unique.len(),
            numbers.len(),
            "two stated numbers are equal, so a crossed getter would pass: {numbers:?}"
        );
    }

    /// Every slot the stated table names is inside the bounds the facts are read over, so a stated
    /// value always appears in the record.
    #[test]
    fn every_stated_slot_is_read_back() {
        for (index, _) in AWAY.colors {
            assert!(
                *index < crate::render_descriptors::READ_COLORS,
                "colour attachment {index} is stated and never read"
            );
        }
        for (index, _) in AWAY.buffers {
            assert!(
                *index < crate::render_descriptors::READ_BUFFERS,
                "tile buffer {index} is stated and never read"
            );
        }
    }

    /// The stated colour formats differ from each other, so an attachment read out of its
    /// neighbour's slot answers a different format rather than passing.
    #[test]
    fn the_stated_colour_formats_are_all_different() {
        let formats: std::collections::BTreeSet<u32> = AWAY
            .colors
            .iter()
            .map(|(_, format)| format.as_raw())
            .collect();
        assert_eq!(formats.len(), AWAY.colors.len());
    }

    /// The three shapes fingerprint differently, so no recording of one is read against another.
    #[test]
    fn the_three_shapes_have_three_fingerprints() {
        let words: std::collections::BTreeSet<Vec<u8>> =
            all().iter().map(|case| case.spec.words()).collect();
        assert_eq!(words.len(), all().len());
    }
}
