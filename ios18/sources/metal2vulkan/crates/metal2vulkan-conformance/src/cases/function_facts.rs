//! What a function says about **itself**: `MTLFunction`'s own getters, and the attributes and
//! function constants two of them answer with.
//!
//! [`crate::cases::function_bindings`] compares what a function's reflection says its arguments
//! are. This is the other side of the same object, and four containers the suite reached almost
//! nowhere: `MTLFunction` at one item of twelve — the argument encoder a function makes — and
//! `MTLAttribute`, `MTLVertexAttribute` and `MTLFunctionConstant` at none of theirs.
//!
//! The facts are not decoration. `functionType` is what Metal refuses a mismatched pipeline
//! descriptor on; `patchType` and `patchControlPointCount` are what a tessellated draw's control
//! points are grouped by; the stage-input attributes are the contract a vertex descriptor is
//! written against; and a function constant's `index` and `required` are what a specialization has
//! to supply. Our side answers every one of them from the AIR, by rules
//! [`metal2vulkan::reflection::FunctionProperties`] states were measured on an Apple M4 over the
//! validation crate's reflection fixtures. This family is what holds those rules to Metal on the
//! oracle, case by case, through the same recording path as every other case.
//!
//! **One flag has one value throughout, and that is a hole rather than coverage.** `isActive`
//! reads true for every attribute of every case here, the control-point field no instruction of
//! `tess_triangle_points` reads included (measured on the oracle while recording). So these cases
//! compare the getter and pin nothing about when it is false; the case that would needs a fixture
//! whose stage input holds an attribute Metal reports inactive, and nobody has one.
//!
//! # The shapes, and why each is here
//!
//! A function's two attribute lists and its constants dictionary answer differently for each kind
//! of entry point, so one case cannot say much. Between them these do:
//!
//! - **a kernel with nothing** (`ob1_threadgroup`): both lists nil, no constant, the floor every
//!   other case is read against;
//! - **a vertex and a fragment function without a stage input** (`rt6_resolve_full`,
//!   `rt6_resolve_white`): the two function types a pipeline pairs, both answering nil;
//! - **a vertex function with a stage input** (`ob3_vertex_input`): the one shape where
//!   `vertexAttributes` **and** `stageInputAttributes` both answer, which is how
//!   `MTLVertexAttribute` is reached at all;
//! - **a kernel with a stage input** (`ob3_stage_input`): `stageInputAttributes` alone, which is
//!   the fact that `vertexAttributes` is a vertex function's and not every function's;
//! - **a post-tessellation vertex function** (`tess_triangle_points`): a triangle patch of three
//!   control points, whose attributes are three control-point fields of three different data types
//!   and one per-patch input at index 5. The only shape that reaches `patchType`,
//!   `patchControlPointCount`, `isPatchData` and `isPatchControlPointData` with anything but their
//!   defaults, and the only one where the two flags disagree between two attributes of one
//!   function;
//! - **two constants a function reads** (`fc_vertex_gated_outputs`): `MTLFunctionConstant`'s four
//!   facts, at two indices;
//! - **a constant asked about rather than read** (`fc_fragment_unobserved_input`): `required` is
//!   false exactly there, so the flag is a question with two answers in this family rather than a
//!   field that reads true everywhere;
//! - **a gated attribute, unspecialized and specialized** (`fc_vertex_input_to_output`): the
//!   constant is in the dictionary without a value and the dictionary is **empty** once a value is
//!   supplied, which is what Metal answers for a specialized function. Measured on the oracle
//!   while recording: the gated attribute is in both attribute lists either way, so what the pair
//!   pins is the dictionary rather than the attribute — a gate turned on does not add an
//!   attribute Metal was already listing.
//!
//! # `MTLFunctionConstantValues`, all three setters
//!
//! The specializing cases reach that class's whole surface, one case a setter, because a value
//! that arrives by name and a value that arrives by index are two Metal calls and the suite had
//! neither: `setConstantValue:type:atIndex:`, `setConstantValue:type:withName:` and
//! `setConstantValues:type:withRange:`. Each case sets the **same** constant through its own
//! setter over the same function, so the three answers are directly comparable and a setter wired
//! to the wrong index shows up as a different attribute list rather than as nothing.
//!
//! # Why this family is safe
//!
//! Each case makes one library and one function and reads its getters. Nothing is compiled into a
//! pipeline, nothing is allocated for the device, no command buffer is made and nothing is
//! dispatched, so no case here can fault the device or trip its watchdog.
//!
//! # Exactness
//!
//! Every field is a number, an enumeration, a name or a bool, and the family declares no
//! tolerance and announces nothing: no case touches a texture, a sampler or a float. An
//! enumeration is recorded as its Metal enumerator name beside its ordinal, so a difference reads
//! `MTLDataTypeFloat4` against `MTLDataTypeHalf2` rather than 6 against 54.

use crate::case::{Abort, Case, Failure, Input, MetalSource, Side};
use crate::fixture::{Fixture, METAL_TEXT, RUNTIME_TEXT};
use crate::function_facts::ConstantValue;
use crate::recording::ShaderRef;
use crate::transcript::{Digest, Node, Observed, Sites};

/// The site the library's make sits at.
const LIBRARY: &str = "library";

/// The site the function's own answers sit at.
const FACTS: &str = "facts";

/// A case over one function's own facts.
pub struct FunctionFactsCase {
    name: &'static str,
    /// The fixture and the entry point whose getters the case reads.
    fixture: &'static str,
    entry: &'static str,
    /// The constant values the function is made with; empty makes it by name alone.
    constants: fn() -> Vec<ConstantValue>,
}

/// No constant value: `newFunctionWithName:`.
fn plain() -> Vec<ConstantValue> {
    Vec::new()
}

/// Every case of the family.
pub fn all() -> Vec<FunctionFactsCase> {
    vec![
        // The floor: a kernel with no stage input and no constant. Both attribute lists are nil
        // and the dictionary is empty, which is what every other case's answer is read against.
        FunctionFactsCase {
            name: "function-facts-kernel",
            fixture: "a27_ob1_threadgroup",
            entry: "ob1_threadgroup",
            constants: plain,
        },
        // A vertex function that computes its own position from `[[vertex_id]]`: the function type
        // a pipeline's vertex stage takes, with nothing to fetch.
        FunctionFactsCase {
            name: "function-facts-vertex",
            fixture: "rt6_resolves",
            entry: "rt6_resolve_full",
            constants: plain,
        },
        // A fragment function, which is the other half of that pipeline and answers nil for both
        // lists however its vertex stage answers.
        FunctionFactsCase {
            name: "function-facts-fragment",
            fixture: "rt6_resolves",
            entry: "rt6_resolve_white",
            constants: plain,
        },
        // A visible function, which a pipeline places in a visible-function table rather than
        // running as a stage. `functionType` is again the subject, and the shape is the third kind
        // of entry point this family had no row for: not a stage, not a mesh stage, a callee.
        FunctionFactsCase {
            name: "function-facts-visible",
            fixture: "visible_table_functions",
            entry: "table_add",
            constants: plain,
        },
        // An object function of a mesh pipeline. `functionType` is the subject: `MTLFunctionType`
        // has seven values and the three above were every one the suite reached, so the two a mesh
        // pipeline's own stages carry were compared nowhere. An object function also answers nil
        // for both attribute lists although it takes a payload, which is the fact that a payload is
        // not a stage input.
        FunctionFactsCase {
            name: "function-facts-object",
            fixture: "mesh_draws",
            entry: "mesh_draws_object",
            constants: plain,
        },
        // The mesh function the object function above dispatches, which is the other of the two.
        // Its `[[mesh]]` output is a writable object rather than a stage input, so both attribute
        // lists are nil here too and `functionType` is what separates it from the object function.
        FunctionFactsCase {
            name: "function-facts-mesh",
            fixture: "mesh_draws",
            entry: "mesh_draws_mesh",
            constants: plain,
        },
        // A vertex function with one `[[stage_in]]` attribute: the one shape where both lists
        // answer, and so the only way to `MTLVertexAttribute`'s own getters.
        FunctionFactsCase {
            name: "function-facts-vertex-attributes",
            fixture: "ob3_vertex_input",
            entry: "ob3_vertex_input",
            constants: plain,
        },
        // A kernel with one `[[stage_in]]` attribute: `stageInputAttributes` answers and
        // `vertexAttributes` is nil, which is the fact that the one is a vertex function's.
        FunctionFactsCase {
            name: "function-facts-stage-in-kernel",
            fixture: "ob3_stage_input",
            entry: "ob3_stage_input",
            constants: plain,
        },
        // A post-tessellation vertex function of a triangle patch of three control points: three
        // control-point fields of three different data types, one of them read by no instruction,
        // and one per-patch attribute at index 5.
        FunctionFactsCase {
            name: "function-facts-patch-attributes",
            fixture: "tessellation",
            entry: "tess_triangle_points",
            constants: plain,
        },
        // Two function constants the vertex function reads, at two indices: `MTLFunctionConstant`
        // stating a name, a type, an index and `required`.
        FunctionFactsCase {
            name: "function-facts-constants",
            fixture: "function_constant_interface",
            entry: "fc_vertex_gated_outputs",
            constants: plain,
        },
        // A constant the function asks `is_function_constant_defined` about rather than reads,
        // which is the one shape whose `required` is false.
        FunctionFactsCase {
            name: "function-facts-optional-constant",
            fixture: "function_constant_interface",
            entry: "fc_fragment_unobserved_input",
            constants: plain,
        },
        // A gated attribute with no value supplied: the attribute leaves both lists and the
        // constant that gates it is in the dictionary.
        FunctionFactsCase {
            name: "function-facts-gated-attribute",
            fixture: "function_constant_interface",
            entry: "fc_vertex_input_to_output",
            constants: plain,
        },
        // The same function specialized through `setConstantValue:type:atIndex:`: the gated
        // attribute is present and the dictionary is empty, as Metal answers for a specialized
        // function.
        FunctionFactsCase {
            name: "function-facts-specialized-by-index",
            fixture: "function_constant_interface",
            entry: "fc_vertex_input_to_output",
            // `has_uv` is `[[function_constant(5)]]`, which gates the `uv` attribute at index 1.
            constants: || vec![ConstantValue::bool_at(5, true)],
        },
        // The same constant of the same function through `setConstantValue:type:withName:`, so
        // the by-name setter's answer is directly comparable with the by-index one's.
        FunctionFactsCase {
            name: "function-facts-specialized-by-name",
            fixture: "function_constant_interface",
            entry: "fc_vertex_input_to_output",
            constants: || vec![ConstantValue::bool_named("has_uv", true)],
        },
        // `setConstantValues:type:withRange:` over the two constants of the gated-output vertex
        // function, which is the one setter that supplies several at once: both gated outputs are
        // turned on by one call.
        FunctionFactsCase {
            name: "function-facts-specialized-by-range",
            fixture: "function_constant_interface",
            entry: "fc_vertex_gated_outputs",
            constants: || vec![ConstantValue::bools_from(0, &[true, true])],
        },
    ]
}

impl FunctionFactsCase {
    fn loaded(&self) -> Fixture {
        Fixture {
            name: self.fixture,
            entry: self.entry,
        }
    }
}

impl Case for FunctionFactsCase {
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
                    fixture: format!("{}{extension}", self.fixture),
                    entry: self.entry.to_owned(),
                    source_sha256: crate::sha256_hex(fixture.text(extension)?.as_bytes()),
                })
            })
            .collect()
    }

    fn inputs(&self) -> Result<Vec<Input>, String> {
        let fixture = self.loaded();
        let mut constants = Vec::new();
        for value in (self.constants)() {
            constants.extend(value.words());
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
            // The entry point and the constant values are the whole of the rest of what the case
            // feeds: every fact it reads is a fact about one function of the library, made with
            // these values and no others.
            Input::bytes("entry", self.entry.as_bytes()),
            Input::bytes("constants", &constants),
        ])
    }

    fn run(&self, side: &mut dyn Side, sites: &mut Sites) -> Result<(), Abort> {
        let stop = |at: &str, why: String| Abort {
            at: at.to_owned(),
            failure: Failure::named("FixtureNotRead", why),
        };
        let loaded = self.loaded().loaded().map_err(|why| stop(LIBRARY, why))?;
        let library = crate::case::made(sites, LIBRARY, side.library(&loaded.source()))?;
        match side.function_facts(library, self.entry, &(self.constants)()) {
            Ok(facts) => {
                sites.push(facts.node(FACTS));
                Ok(())
            }
            Err(failure) => {
                sites.push(Node::leaf(FACTS, Observed::value(failure.recorded())));
                Err(Abort {
                    at: FACTS.to_owned(),
                    failure,
                })
            }
        }
    }
}
