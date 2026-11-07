//! What a **mesh render pipeline's** reflection says of its arguments, under every
//! `MTLPipelineOption` value and with and without an object stage.
//!
//! The suite made mesh pipelines before this family — [`crate::cases::mesh_draws`],
//! [`crate::cases::mesh_commands`] and [`crate::cases::mesh_barriers`] make them and draw with
//! them — and asked none of them for a reflection. So
//! `newRenderPipelineStateWithMeshDescriptor:options:reflection:error:` was called with a nil
//! out-parameter everywhere in the tree, and `MTLRenderPipelineReflection.objectBindings` and
//! `.meshBindings` were compared against Metal nowhere.
//!
//! That omission held one whole protocol out of the suite's reach. **`MTLBindingTypeObjectPayload`
//! is answerable by this creation and no other**: a payload is an argument of an object or a mesh
//! function and of nothing else, so it cannot appear in a compute pipeline's reflection, a drawing
//! pipeline's or a tile pipeline's. With it comes `MTLObjectPayloadBinding`, the fifth and last of
//! `MTLBinding`'s subprotocols and the only one the suite had never read
//! ([`PayloadFacts`](crate::function_bindings::PayloadFacts)): `objectPayloadAlignment` and
//! `objectPayloadDataSize`, neither of which is in the source — the payload's struct is declared
//! in MSL and these two numbers are what Metal's own layout rules make of it.
//!
//! # Two sweeps
//!
//! **`MTLPipelineOption`, 8 of 8.** [`crate::pipeline_option_reflection::MASKS`] is the
//! whole of Apple's option word, and each case runs the mesh creation under every one of its eight
//! values — the same table [`crate::cases::pipeline_option_reflection`] sweeps for the compute
//! creation and [`crate::cases::tile_pipeline_reflection`] for the tile one, read rather than
//! copied.
//!
//! *Why no value of it is vacuous:* `MTLPipelineOptionNone` answers **no** reflection and every
//! mask holding `1 << 0` or `1 << 1` answers one, so the eight sites of one case disagree with
//! each other at their reflection site. A side that ignored its options and always reflected, or
//! never did, fails at least one site.
//!
//! **The object stage, 2 of 2.** One case states the pipeline with its object function and one
//! without it. Both are cases rather than one, because Metal's answer differs: measured on an M3
//! Max under Metal 27.0.1, the pipeline **with** an object stage answers two object bindings (the
//! payload, read-write, and a `float4` buffer at index 0) and two mesh bindings; the pipeline
//! **without** one answers **zero** object bindings and the same two mesh bindings. So the axis
//! moves `objectBindings` from two entries to none while leaving `meshBindings` alone, which is a
//! difference no other site of the case shows.
//!
//! That second case also measures something worth knowing on its own: Metal accepts a mesh
//! pipeline whose **mesh** function takes a payload while the pipeline has no object function to
//! write one, and still reports the payload among the mesh bindings. A side that refused that
//! creation would be refusing something plain Metal makes.
//!
//! # The payload's two numbers, and the access that differs between stages
//!
//! The payload appears in **both** stage lists of the pipeline that has an object stage, and the
//! two entries differ in exactly one field: `access` is `MTLBindingAccessReadWrite` in the object
//! function, which writes the payload, and `MTLBindingAccessReadOnly` in the mesh function, which
//! reads it. Its `index` is `4294967295` in both — `NSUIntegerMax` narrowed, which is Metal's way
//! of saying a payload binds no argument-table slot — and its alignment and size are 16 and 32 for
//! the fixture's `{ float4 tint; uint count; }`: a 20-byte C struct Metal rounds to 32 under the
//! `float4`'s own 16-byte alignment. None of those four numbers is a literal in the MSL.
//!
//! # All five stage lists, every site
//!
//! Each site records all five lists ([`StageBindings`](crate::function_bindings::StageBindings)),
//! for the reason that module states: the five together say **which stage Metal attributes each
//! argument to**, and a side that filed a mesh function's arguments under `objectBindings` holds
//! the same bindings and differs only here.
//!
//! The fragment list is empty on both sides **by construction** and is named as such rather than
//! counted as compared: the fixture's fragment function takes `[[stage_in]]` alone, and a stage
//! input binds no argument-table entry on either side. The vertex and tile lists are empty for a
//! mesh pipeline of any shape.
//!
//! # Why every case of this family is safe
//!
//! Each case makes one library and eight mesh pipeline states. Nothing is allocated for the
//! device, no render pass is encoded, no command buffer is made and **nothing is dispatched** —
//! which matters more here than elsewhere in this suite, because a mesh function that never writes
//! its mesh output has hung this device's GPU in this round. No case of this family runs a mesh
//! shader at all; the fixture's mesh function does set its primitive count, its vertices and its
//! indices, and it is still only ever compiled here. Every options value is one Apple's header
//! names, so no creation asks Metal what an undefined bit means.
//!
//! # Exactness
//!
//! Every field is a number, an enumeration, a name or a bool, and the family declares no
//! tolerance. An enumeration is recorded as its Metal enumerator name beside its ordinal, so a
//! difference reads `MTLBindingTypeObjectPayload` against `MTLBindingTypeBuffer` rather than 34
//! against 0.

use crate::case::{Abort, Case, Failure, Input, MeshReflectionSpec, MetalSource, Side};
use crate::fixture::{Fixture, METAL_TEXT, RUNTIME_TEXT};
use crate::pipeline_option_reflection::MASKS;
use crate::recording::ShaderRef;
use crate::transcript::{Digest, Node, Observed, Sites};
use metal2vulkan::metal::PixelFormat;

/// The site the library's creation sits at.
const LIBRARY: &str = "library";

/// The fixture holding the object, mesh and fragment functions.
const MESH: &str = "reflection/mesh_functions";

/// The mesh function, which takes the payload and a `float2 *` buffer at index 1.
const MESH_ENTRY: &str = "reflection_mesh";

/// The object function, which writes the payload and takes a `float4 &` buffer at index 0.
const OBJECT_ENTRY: &str = "reflection_object";

/// The fragment function, whose only argument is a stage input.
const FRAGMENT_ENTRY: &str = "reflection_mesh_fragment";

/// The one colour attachment the pipeline writes.
///
/// `RGBA8Unorm` and not [`TARGET_FORMAT`](crate::case::TARGET_FORMAT), for the reason
/// [`MeshReflectionSpec`] states: the fragment function returns `float4`, and Metal refuses a
/// pipeline that writes a `float4` into an integer attachment. A refused creation reflects
/// nothing, so the wrong format here would make every site of this family compare a refusal
/// against a refusal.
const ATTACHMENTS: [PixelFormat; 1] = [PixelFormat::RGBA8Unorm];

/// A case over one mesh pipeline's reflection, swept across every `MTLPipelineOption` value.
pub struct MeshPipelineReflectionCase {
    name: &'static str,
    /// Whether the descriptor states an object function.
    object: bool,
}

/// Every case of the family: the object-stage axis, both values.
pub fn all() -> Vec<MeshPipelineReflectionCase> {
    vec![
        // With the object stage: the payload is in both stage lists, read-write in the object
        // function and read-only in the mesh function.
        MeshPipelineReflectionCase {
            name: "mesh-pipeline-reflection-object-stage",
            object: true,
        },
        // Without it: `objectBindings` is empty and the mesh function's payload is still reported,
        // for a pipeline that has nothing to write it.
        MeshPipelineReflectionCase {
            name: "mesh-pipeline-reflection-no-object-stage",
            object: false,
        },
    ]
}

impl MeshPipelineReflectionCase {
    fn loaded(&self) -> Fixture {
        Fixture {
            name: MESH,
            entry: MESH_ENTRY,
        }
    }

    /// The pipeline the case states.
    fn spec(&self) -> MeshReflectionSpec<'static> {
        MeshReflectionSpec {
            object: self.object.then_some(OBJECT_ENTRY),
            mesh: MESH_ENTRY,
            fragment: FRAGMENT_ENTRY,
            attachments: &ATTACHMENTS,
        }
    }
}

impl Case for MeshPipelineReflectionCase {
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
                    fixture: format!("{MESH}{extension}"),
                    entry: MESH_ENTRY.to_owned(),
                    source_sha256: crate::sha256_hex(fixture.text(extension)?.as_bytes()),
                })
            })
            .collect()
    }

    fn inputs(&self) -> Result<Vec<Input>, String> {
        let fixture = self.loaded();
        // Every mask the case sweeps, so a value added to or taken out of the sweep is a change to
        // the case's inputs rather than a recording that silently covers less.
        let mut masks = Vec::new();
        for (bits, name) in MASKS {
            masks.extend(bits.to_le_bytes());
            masks.extend(name.as_bytes());
        }
        Ok(vec![
            Input {
                name: "air".to_owned(),
                digest: Digest::sha256(fixture.text(RUNTIME_TEXT)?.as_bytes()),
            },
            Input {
                name: "air-typed".to_owned(),
                digest: Digest::sha256(fixture.text(METAL_TEXT)?.as_bytes()),
            },
            Input::bytes("descriptor", &self.spec().words()),
            Input::bytes("options", &masks),
        ])
    }

    fn run(&self, side: &mut dyn Side, sites: &mut Sites) -> Result<(), Abort> {
        let stop = |at: &str, why: String| Abort {
            at: at.to_owned(),
            failure: Failure::named("FixtureNotRead", why),
        };
        let loaded = self.loaded().loaded().map_err(|why| stop(LIBRARY, why))?;
        let library = crate::case::made(sites, LIBRARY, side.library(&loaded.source()))?;
        let spec = self.spec();
        // Every mask is asked for, whatever an earlier one answered, for the reason
        // [`crate::cases::pipeline_option_reflection`] states.
        let mut stopped = None;
        for (bits, name) in MASKS {
            let at = format!("options-{name}");
            match side.mesh_pipeline_bindings(library, &spec, bits) {
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
