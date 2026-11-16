//! Every `MTLPipelineOption` value against the reflection it makes a creation answer.
//!
//! [`crate::pipeline_option_reflection`] states the sweep and the two claims it is pointed at: that
//! `MTLPipelineOptionBufferTypeInfo` answers what `MTLPipelineOptionBindingInfo` answers, and that
//! `MTLPipelineOptionFailOnBinaryArchiveMiss` changes nothing where no archive is listed. Before
//! this family the suite had reached one of the enumeration's eight values.
//!
//! # The sweep: 8 of 8 values, over three kernels
//!
//! Every row of [`crate::pipeline_option_reflection::MASKS`] is taken against each of the three
//! kernels [`crate::cases::function_bindings`] reads a reflection off, because the three cover the
//! three binding kinds the reflection has fields for and the option under test is claimed to be
//! about one of them:
//!
//! - an **argument-buffer** kernel (`px_sample_argument`), whose `constant PixelArguments &`
//!   binding is the one with a `bufferStructType` — which is what `BufferTypeInfo` names;
//! - a **texture-slot** kernel (`xr_sample_runtime`), whose bindings are a texture and a sampler,
//!   so a row where the buffer bit changed a texture's facts would read wrong;
//! - a **threadgroup-memory** kernel (`ob1_threadgroup`), whose binding carries
//!   `MTLThreadgroupBinding`'s own fields.
//!
//! That is 24 rows: 8 values by 3 kernels. The pairing is deliberate rather than exhaustive over
//! everything — the question "does this option bit change this reflection" needs the option bit
//! swept whole and enough reflections for the answer not to be one shader's accident.
//!
//! # Why no row lists a binary archive
//!
//! `FailOnBinaryArchiveMiss` is swept here for what it does to a creation that lists **no**
//! archive, which is the claim in the runtime's own documentation and the one a consumer meets
//! first. What the bit does **with** an archive is a question about `MTLBinaryArchive`, which no
//! case of the suite makes yet; it is recorded as an open item rather than answered here.
//!
//! # Why this family is safe
//!
//! Each case makes one library and compiles one kernel eight times. Nothing is allocated for the
//! device, no command buffer is made and nothing is dispatched, so no case here can fault the
//! device or trip its watchdog. No undefined option bit is passed.
//!
//! # Exactness
//!
//! Every site is a bool, a number, a name or an enumeration. The family declares no tolerance and
//! announces nothing.

use crate::case::{Abort, Case, Failure, Input, MetalSource, Side};
use crate::fixture::{Fixture, METAL_TEXT, RUNTIME_TEXT};
use crate::pipeline_option_reflection::MASKS;
use crate::recording::ShaderRef;
use crate::transcript::{Digest, Node, Observed, Sites};

/// The site the library's make sits at.
const LIBRARY: &str = "library";

/// A case over one kernel's reflection under every `MTLPipelineOption` value.
pub struct PipelineOptionCase {
    name: &'static str,
    fixture: &'static str,
    entry: &'static str,
}

/// Every case of the family: the three kernels whose reflections cover the three binding kinds.
pub fn all() -> Vec<PipelineOptionCase> {
    vec![
        // The argument-buffer kernel: its `constant PixelArguments &a [[buffer(2)]]` is the
        // binding with a `bufferStructType`, which is the field `BufferTypeInfo` names.
        PipelineOptionCase {
            name: "pipeline-options-argument-buffer",
            fixture: "px_samplers",
            entry: "px_sample_argument",
        },
        // The texture-slot kernel: a texture and a sampler, so a buffer-only option bit must
        // leave these rows alone.
        PipelineOptionCase {
            name: "pipeline-options-texture-slots",
            fixture: "xr_samples",
            entry: "xr_sample_runtime",
        },
        // The threadgroup-memory kernel, where `MTLThreadgroupBinding`'s own fields are.
        PipelineOptionCase {
            name: "pipeline-options-threadgroup-memory",
            fixture: "a27_ob1_threadgroup",
            entry: "ob1_threadgroup",
        },
    ]
}

impl PipelineOptionCase {
    fn loaded(&self) -> Fixture {
        Fixture {
            name: self.fixture,
            entry: self.entry,
        }
    }
}

impl Case for PipelineOptionCase {
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
            Input::bytes("entry", self.entry.as_bytes()),
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
        // Every mask is asked for, whatever an earlier one answered: the sweep is the measurement,
        // and stopping at the first value a side cannot take would leave the rest unmeasured.
        let mut stopped = None;
        for (bits, name) in MASKS {
            let at = format!("options-{name}");
            match side.compute_pipeline_reflection_under(library, self.entry, bits) {
                Ok(under) => sites.push(under.node(at)),
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
