//! `MTLRenderPipelineFunctionsDescriptor`'s three per-stage lists, written and read back.
//!
//! The class was reached at none of its four items. [`crate::pipeline_functions`] says what a
//! case can claim about it and what it folds; this is the sweep of the one axis it has — which of
//! the three properties a program wrote, with what, and in what order.
//!
//! # The shapes, and why each is here
//!
//! - **the descriptor nobody wrote** (`untouched`): the floor, and the only case whose three
//!   answers are all empty. It is the one site in the family that compares nothing on its own,
//!   and it is here because what Metal's `init` leaves is what every other case's answer is read
//!   against;
//! - **one property written, the other two not** — three cases, one a stage: the answer is that
//!   stage's list and the other two are still empty, which is what says a setter touches **its
//!   own** property. Each stage takes a **different** list, so the three cases are also three
//!   different answers;
//! - **all three written, each a different list** (`every-stage`): the positive control. The
//!   vertex stage takes two functions, the fragment stage one, the tile stage two in the other
//!   order, and no two of the three lists are equal — so a side that answered one stage's
//!   property where another's belongs, or that lost an order, reads a different list rather than
//!   the same length;
//! - **a property written twice** (`replaced`): a list of two, then a list of one, on the vertex
//!   stage. The second is the answer, which is the fact that the property holds the last write
//!   rather than accumulating;
//! - **a property written and put back to nil** (`cleared`): a list of two, then
//!   `setVertexAdditionalBinaryFunctions:nil`. The answer is empty again, which is the one
//!   question a mutable property has that no first write can ask;
//! - **a property written with an empty array** (`emptied`): Metal's empty array rather than its
//!   nil. **Indistinguishable from `cleared` by construction** under this family's fold, and
//!   named as such: our side has no object that could hold the difference. What the case still
//!   buys is that Metal accepts the empty array and answers it as a list of none rather than
//!   refusing the setter.
//!
//! So of the family's eight cases, six have answers nothing else leaves, one is the floor and one
//! repeats another's answer for a stated reason.
//!
//! # The functions, and why these
//!
//! `visible_table_functions`' four `[[visible]]` functions — `table_add`, `table_mul`,
//! `table_sub` and `table_half`. Four distinct names, so a list is identified by what is in it,
//! and functions a program really would put in these properties.
//!
//! They are made by name, so none of them is compiled to a binary
//! (`MTLFunctionOptionCompileToBinary`), which Metal requires of a function in these lists. That
//! costs this family nothing and is deliberate: nothing here hands the descriptor to
//! `newRenderPipelineStateWithAdditionalBinaryFunctions:error:`, which is the call that needs a
//! binary archive and is the reason [`crate::pipeline_facts`] leaves every binary-function item
//! out. The setters are array stores and Metal validates neither at set time nor at get time
//! (measured on this family's oracle while recording: Metal's own API validation layer let every
//! case run).
//!
//! # Why this family is safe
//!
//! Each case makes one library, some functions of it, and a descriptor it writes, reads and
//! drops. Nothing is compiled into a pipeline, nothing is allocated for the device, no command
//! buffer is made and nothing is drawn or dispatched, so no case here can fault the device or
//! trip its watchdog. The one setter of Metal's in reach takes an array or nil and neither ends a
//! process.
//!
//! # Exactness
//!
//! Every field is a count or a name. The family declares no tolerance and announces nothing.

use crate::case::{Abort, Case, Failure, Input, MetalSource, Side};
use crate::fixture::{Fixture, METAL_TEXT, RUNTIME_TEXT};
use crate::pipeline_functions::{FunctionsDescriptorSpec, Write};
use crate::recording::ShaderRef;
use crate::transcript::{Digest, Node, Observed, Sites};

/// The site the library's make sits at.
const LIBRARY: &str = "library";

/// The site the descriptor's own answers sit at.
const FACTS: &str = "facts";

/// The fixture whose four `[[visible]]` functions the lists hold, and the kernel that gives its
/// module an entry point.
const FIXTURE: &str = "visible_table_functions";
const ENTRY: &str = "visible_table_functions";

/// The vertex stage's list in every case that writes one: two functions, in this order.
const VERTEX_LIST: &[&str] = &["table_add", "table_mul"];

/// The fragment stage's: one function, and not one of the vertex stage's, so a side that answered
/// the vertex property here reads two names where one belongs.
const FRAGMENT_LIST: &[&str] = &["table_sub"];

/// The tile stage's: two functions in the **other** order from the vertex stage's, so a side that
/// answered the vertex property here reads the same two names the wrong way round.
const TILE_LIST: &[&str] = &["table_mul", "table_add"];

/// The second write of the `replaced` case: one function, which is neither of the first write's
/// two, so the answer names what the last write put there.
const REPLACEMENT: &[&str] = &["table_half"];

/// A case over one descriptor's own answers.
pub struct PipelineFunctionsCase {
    name: &'static str,
    spec: FunctionsDescriptorSpec<'static>,
}

/// Every case of the family.
pub fn all() -> Vec<PipelineFunctionsCase> {
    vec![
        // The floor: what `init` leaves, which every other case's answer is read against.
        PipelineFunctionsCase {
            name: "pipeline-functions-untouched",
            spec: FunctionsDescriptorSpec::untouched(),
        },
        // One property a case, the other two untouched: a setter touches its own.
        PipelineFunctionsCase {
            name: "pipeline-functions-vertex-only",
            spec: FunctionsDescriptorSpec::new(&[Write::Functions(VERTEX_LIST)], &[], &[]),
        },
        PipelineFunctionsCase {
            name: "pipeline-functions-fragment-only",
            spec: FunctionsDescriptorSpec::new(&[], &[Write::Functions(FRAGMENT_LIST)], &[]),
        },
        PipelineFunctionsCase {
            name: "pipeline-functions-tile-only",
            spec: FunctionsDescriptorSpec::new(&[], &[], &[Write::Functions(TILE_LIST)]),
        },
        // All three, each a different list: the positive control.
        PipelineFunctionsCase {
            name: "pipeline-functions-every-stage",
            spec: FunctionsDescriptorSpec::new(
                &[Write::Functions(VERTEX_LIST)],
                &[Write::Functions(FRAGMENT_LIST)],
                &[Write::Functions(TILE_LIST)],
            ),
        },
        // Written twice: the property holds the last write rather than accumulating.
        PipelineFunctionsCase {
            name: "pipeline-functions-replaced",
            spec: FunctionsDescriptorSpec::new(
                &[Write::Functions(VERTEX_LIST), Write::Functions(REPLACEMENT)],
                &[],
                &[],
            ),
        },
        // Written and put back to nil.
        PipelineFunctionsCase {
            name: "pipeline-functions-cleared",
            spec: FunctionsDescriptorSpec::new(
                &[Write::Functions(VERTEX_LIST), Write::Nil],
                &[],
                &[],
            ),
        },
        // Written with Metal's empty array rather than its nil.
        PipelineFunctionsCase {
            name: "pipeline-functions-emptied",
            spec: FunctionsDescriptorSpec::new(
                &[Write::Functions(VERTEX_LIST), Write::Functions(&[])],
                &[],
                &[],
            ),
        },
    ]
}

impl Case for PipelineFunctionsCase {
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
                    entry: ENTRY.to_owned(),
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
            // Every write to every property, in order: that is the whole of what the case feeds.
            Input::bytes("writes", &self.spec.words()),
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
        match side.render_pipeline_functions_descriptor_facts(library, &self.spec) {
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
