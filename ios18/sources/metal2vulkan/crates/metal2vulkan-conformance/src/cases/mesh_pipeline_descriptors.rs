//! What a mesh render pipeline **descriptor** says about itself, and what `reset` puts back.
//!
//! `MTLMeshRenderPipelineDescriptor` was exercised at five of its eighteen implemented items, and
//! all five by `mesh_draws`, `mesh_barriers` and `mesh_commands` **making pipelines** with it: the
//! three stage functions, the colour-attachment array and `supportIndirectCommandBuffers`. Nothing
//! read a getter of the descriptor back, so the label, the raster sample count, the two coverage
//! flags, the rasterization switch, the depth and stencil formats, the binary archives, the three
//! per-stage linked-function objects and `reset` were reached by nothing at all.
//!
//! The runtime holds each of those defaults as a literal in
//! [`MeshPipelineDescriptor::new`](metal2vulkan::pipeline::MeshPipelineDescriptor::new), read off
//! Apple's documentation, and nothing in the tree ran Metal to check one.
//! [`crate::mesh_descriptors`] says which claim each getter is, and which nine items of Metal's
//! mesh descriptor the runtime implements no field for; this family is the three runs that hold the
//! rest.
//!
//! # The shapes, and why each is here
//!
//! - **the descriptor of a mesh and a fragment function** (`mesh-descriptor-defaults`): Metal's
//!   default for every other field, which is the one shape that can say what a program gets for
//!   free. An object function is deliberately **not** set, so `objectFunction`'s nil is recorded as
//!   the nil it is;
//! - **the descriptor with every field stated** (`mesh-descriptor-set`): each field away from its
//!   default, the colour attachments stating slots 1 and 2 with no two of their nine getters
//!   agreeing across the two ([`crate::mesh_descriptors::AWAY`]), read straight back. Slot 0 is
//!   stated by nothing, so its answer is Metal's own default for an attachment recorded beside two
//!   that were set — and it is slot 0 deliberately, which is where a side that ignored the index
//!   would have written;
//! - **the same descriptor after `reset`** (`mesh-descriptor-reset`): every field stated, then
//!   `reset`, then the stage functions set again. Its answer must be the first shape's but for the
//!   object function, which this shape sets and the first does not. It reads every field the first
//!   shape does **except** the three per-stage linked-function lists, for the measured reason
//!   [`crate::render_descriptors::SuppliedLists`] states: `reset` leaves such a list nil where
//!   `init` leaves a non-nil object, and `objc2-metal` 0.3.2 declares all three non-null and panics
//!   on the nil.
//!
//! The runtime has no mutable descriptor, so its `reset` is `MeshPipelineDescriptor::new` again
//! (the coverage map says so), and the case makes our side build the stated descriptor and then
//! replace it — rather than skip the setting, which would leave nothing for the reset to undo.
//!
//! # Exactness
//!
//! Every field is a number, a bool, a name or an ordinal. The family declares no tolerance and
//! announces nothing: no case touches a texture, a sampler, a float or the GPU.
//!
//! Four items are compared **at their default alone** — `binaryArchives` and the three per-stage
//! linked-function objects — because the runtime takes each of them somewhere other than the
//! descriptor and a count is the one shape both sides can state. The three per-stage ones are
//! compared on the first two shapes and not the third. [`crate::mesh_descriptors`] gives the
//! reasoning; nothing is widened for it.
//!
//! # Why this family is safe
//!
//! No shape compiles a pipeline, allocates a resource, makes a command buffer or reaches a queue:
//! each makes one library, two or three functions of it, and a descriptor it reads and drops. The
//! one setter of Metal's that can end a process here is `setLabel:` with nil, which no shape sends.

use crate::case::{Abort, Case, Failure, Input, MetalSource, Side};
use crate::fixture::{Fixture, METAL_TEXT, RUNTIME_TEXT};
use crate::mesh_descriptors::DescriptorSpec;
use crate::recording::ShaderRef;
use crate::render_descriptors::Shape;
use crate::transcript::{Digest, Node, Observed, Sites};

/// The site the library's make sits at.
const LIBRARY: &str = "library";

/// The site the descriptor's own answers sit at.
const FACTS: &str = "facts";

/// The fixture whose object, mesh and fragment functions `mesh_draws` draws with, so this family
/// needs no fixture of its own.
const FIXTURE: &str = "mesh_draws";
const MESH: &str = "mesh_draws_mesh";
const OBJECT: &str = "mesh_draws_object";
const FRAGMENT: &str = "mesh_draws_fragment";

/// A case over one mesh render pipeline descriptor's own facts.
pub struct MeshDescriptorCase {
    name: &'static str,
    spec: DescriptorSpec<'static>,
}

/// Every case of the family.
pub fn all() -> Vec<MeshDescriptorCase> {
    vec![
        // The floor: a descriptor carrying a mesh and a fragment function. Every other field is
        // Metal's own default, and no case before this one asked Metal for any of them. The object
        // function is left unset so its nil is recorded.
        MeshDescriptorCase {
            name: "mesh-descriptor-defaults",
            spec: DescriptorSpec::new(MESH, FRAGMENT, Shape::Defaults),
        },
        // Every field stated, each away from its default, read straight back: the round trip that
        // an enumeration numbered differently, or a getter wired to a neighbour's field, fails.
        MeshDescriptorCase {
            name: "mesh-descriptor-set",
            spec: DescriptorSpec::new(MESH, FRAGMENT, Shape::Set).with_object(OBJECT),
        },
        // The same, then `reset`, then the stage functions again.
        MeshDescriptorCase {
            name: "mesh-descriptor-reset",
            spec: DescriptorSpec::new(MESH, FRAGMENT, Shape::Reset).with_object(OBJECT),
        },
    ]
}

impl Case for MeshDescriptorCase {
    fn name(&self) -> &'static str {
        self.name
    }

    fn metal_source(&self) -> MetalSource {
        MetalSource::Run
    }

    fn shaders(&self) -> Result<Vec<ShaderRef>, String> {
        let fixture = self.loaded();
        [RUNTIME_TEXT, METAL_TEXT]
            .into_iter()
            .map(|extension| {
                Ok(ShaderRef {
                    fixture: format!("{FIXTURE}{extension}"),
                    entry: MESH.to_owned(),
                    source_sha256: crate::sha256_hex(fixture.text(extension)?.as_bytes()),
                })
            })
            .collect()
    }

    fn inputs(&self) -> Result<Vec<Input>, String> {
        let fixture = self.loaded();
        Ok(vec![
            Input {
                name: "air".to_owned(),
                digest: Digest::sha256(fixture.text(RUNTIME_TEXT)?.as_bytes()),
            },
            Input {
                name: "air-typed".to_owned(),
                digest: Digest::sha256(fixture.text(METAL_TEXT)?.as_bytes()),
            },
            // The spec is the whole of the rest of what the case feeds: the stage functions, the
            // shape, and — where the shape states anything — the stated table itself.
            Input::bytes("descriptor", &self.spec.words()),
            // The slot bound the facts are read over, so a record of three attachments compared
            // against a record of two is a different observation.
            Input::bytes(
                "slots",
                &crate::render_descriptors::READ_COLORS.to_le_bytes(),
            ),
        ])
    }

    fn run(&self, side: &mut dyn Side, sites: &mut Sites) -> Result<(), Abort> {
        let stop = |at: &str, why: String| Abort {
            at: at.to_owned(),
            failure: Failure::named("FixtureNotRead", why),
        };
        let loaded = self.loaded().loaded().map_err(|why| stop(LIBRARY, why))?;
        let library = crate::case::made(sites, LIBRARY, side.library(&loaded.source()))?;
        match side.mesh_descriptor_facts(library, &self.spec) {
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

impl MeshDescriptorCase {
    fn loaded(&self) -> Fixture {
        Fixture {
            name: FIXTURE,
            entry: MESH,
        }
    }
}

/// The stated attachments disagree field by field, and every slot the table names is inside the
/// bound the facts are read over. Those two properties are what make the set shape a measurement
/// rather than a shape that passes by coincidence.
#[cfg(test)]
mod tests {
    use super::*;
    use crate::mesh_descriptors::AWAY;

    /// Every colour attachment slot the stated table names is inside the bound the facts are read
    /// over, so a stated attachment always appears in the record.
    #[test]
    fn every_stated_slot_is_read_back() {
        for color in AWAY.colors {
            assert!(
                color.index < crate::render_descriptors::READ_COLORS,
                "colour attachment {} is stated and never read",
                color.index
            );
        }
    }

    /// No slot of the record is stated twice, and at least one is stated by nothing: a table that
    /// filled every read slot would compare no Metal default at all.
    #[test]
    fn one_read_slot_is_stated_by_nothing() {
        let stated: std::collections::BTreeSet<u32> =
            AWAY.colors.iter().map(|color| color.index).collect();
        assert_eq!(stated.len(), AWAY.colors.len(), "a slot is stated twice");
        assert!(
            stated.len() < crate::render_descriptors::READ_COLORS as usize,
            "every read slot is stated, so no Metal default for an attachment is compared"
        );
    }

    /// The two stated attachments agree on no field a getter could be crossed with, so one read out
    /// of its neighbour's slot answers a different value rather than passing.
    #[test]
    fn the_two_stated_attachments_agree_on_nothing() {
        let [first, second] = match AWAY.colors {
            [first, second] => [first, second],
            other => panic!("the table states {} attachments, not two", other.len()),
        };
        for (what, equal) in [
            ("pixel_format", first.pixel_format == second.pixel_format),
            (
                "blending_enabled",
                first.blending_enabled == second.blending_enabled,
            ),
            ("source_rgb", first.source_rgb == second.source_rgb),
            (
                "destination_rgb",
                first.destination_rgb == second.destination_rgb,
            ),
            ("rgb_operation", first.rgb_operation == second.rgb_operation),
            ("source_alpha", first.source_alpha == second.source_alpha),
            (
                "destination_alpha",
                first.destination_alpha == second.destination_alpha,
            ),
            (
                "alpha_operation",
                first.alpha_operation == second.alpha_operation,
            ),
            ("write_mask", first.write_mask == second.write_mask),
        ] {
            assert!(
                !equal,
                "the two stated attachments share their {what}, so a crossed read would pass"
            );
        }
    }

    /// Each attachment's own blend factors and operations differ within it, so a side answering the
    /// RGB side of a blend out of the alpha side reads a different ordinal.
    #[test]
    fn each_stated_attachment_states_its_own_sides_apart() {
        for color in AWAY.colors {
            // The second attachment is Metal's own `One`/`Zero`/`Add` on both sides, which is the
            // point of having one of the two unblended: it is the row that says the defaults are
            // what they are. Only a blending attachment can hold its two sides apart.
            if !color.blending_enabled {
                continue;
            }
            assert_ne!(
                color.source_rgb.as_raw(),
                color.source_alpha.as_raw(),
                "attachment {}'s two source factors are equal",
                color.index
            );
            assert_ne!(
                color.destination_rgb.as_raw(),
                color.destination_alpha.as_raw(),
                "attachment {}'s two destination factors are equal",
                color.index
            );
            assert_ne!(
                color.rgb_operation.as_raw(),
                color.alpha_operation.as_raw(),
                "attachment {}'s two blend operations are equal",
                color.index
            );
        }
    }

    /// The three shapes fingerprint differently, so no recording of one is read against another.
    #[test]
    fn the_three_shapes_have_three_fingerprints() {
        let words: std::collections::BTreeSet<Vec<u8>> =
            all().iter().map(|case| case.spec.words()).collect();
        assert_eq!(words.len(), all().len());
    }
}
