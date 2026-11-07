//! What a pipeline state's creation answers under each `MTLPipelineOption`: whether a reflection
//! comes back at all, and what it holds when it does.
//!
//! # Why the option is the parameter
//!
//! `MTLPipelineOption` is the one argument of
//! `newComputePipelineStateWithFunction:options:reflection:error:` and
//! `newRenderPipelineStateWithDescriptor:options:reflection:error:` that decides whether the
//! creation answers a reflection, and the suite passed **two** of its values anywhere:
//! `MTLPipelineOptionNone`, at six call sites that want no reflection, and
//! `MTLPipelineOptionBindingInfo`, at the two that do. No case compared what the option itself
//! does, so every claim the runtime makes about the option space rested on prose:
//!
//! - **`BINDING_INFO` and `BUFFER_TYPE_INFO` answer alike**, struct and pointer types included, so
//!   `BUFFER_TYPE_INFO` adds nothing over `BINDING_INFO` and either alone gets the whole
//!   reflection;
//! - **neither gets none**, so a creation that asks gets an object rather than Metal's nil;
//! - **every bit is kept and only the defined ones act**, so an undefined bit — the runtime's own
//!   documentation names 8, 16, `1 << 10`, `1 << 31` and `1 << 40` — neither refuses the creation
//!   nor changes its reflection;
//! - **`FAIL_ON_BINARY_ARCHIVE_MISS` is about the archives**, so with no archive listed it changes
//!   nothing.
//!
//! Each of those is a statement about a value nothing ran Metal for. This vocabulary is what a
//! case states one in, and the family [`crate::cases::pipeline_options`] is the sweep.
//!
//! # What one outcome holds
//!
//! One bool and one binding list a stage. The bool is the whole of the first claim: Metal's
//! `reflection:` out-parameter either came back with an object or stayed nil, and a side that
//! answered the other way is a difference of one leaf rather than a shape mismatch. The binding
//! lists are [`crate::function_bindings`]' own [`BindingFacts`], so a reflection's content reads
//! here exactly as it reads in that family and the two are directly comparable.
//!
//! The list is an `Option`: `None` where the creation answered no reflection, which keeps **one**
//! shape across the whole sweep. A side that answered a reflection where the other answered none
//! then differs at the `reflected` leaf and at each stage's, rather than emitting a tree the other
//! side has no counterpart for.

use crate::function_bindings::{bindings_node, BindingFacts};
use crate::transcript::{Node, NodeName, Observed, Value};

/// The site a compute pipeline's one binding list sits at: `MTLComputePipelineReflection.bindings`.
pub const KERNEL_STAGE: &str = "kernel_bindings";

/// The site a render pipeline's `vertexBindings` sits at.
pub const VERTEX_STAGE: &str = "vertex_bindings";

/// The site a render pipeline's `fragmentBindings` sits at.
pub const FRAGMENT_STAGE: &str = "fragment_bindings";

/// The raw `MTLPipelineOption` value a case passes, with the name its site takes.
///
/// A raw `u64` rather than a typed set: the point of the sweep is the bits Metal does **not**
/// define as much as the three it does, and a type that only spells the three could not state
/// them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OptionValue {
    /// Metal's raw value.
    pub raw: u64,
    /// What the value is, as the case's name and its prose call it.
    pub spelling: &'static str,
}

impl OptionValue {
    /// The option of raw value `raw`, called `spelling`.
    pub const fn new(raw: u64, spelling: &'static str) -> Self {
        Self { raw, spelling }
    }

    /// The words a case's input fingerprint covers.
    pub fn words(&self) -> Vec<u8> {
        let mut out = self.raw.to_le_bytes().to_vec();
        out.extend(self.spelling.as_bytes());
        out
    }
}

/// What one creation under one option value answered.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OptionOutcome {
    /// Whether the creation's `reflection:` out-parameter came back with an object.
    pub reflected: bool,
    /// One entry a stage the case asks about, in the order it asks; `None` where the creation
    /// answered no reflection at all.
    pub stages: Vec<(&'static str, Option<Vec<BindingFacts>>)>,
}

impl OptionOutcome {
    /// The outcome of a creation that answered no reflection, for the stages `stages`.
    pub fn without_reflection(stages: &[&'static str]) -> Self {
        Self {
            reflected: false,
            stages: stages.iter().map(|stage| (*stage, None)).collect(),
        }
    }

    /// The facts as the sites a case emits under one node, so both sides emit one shape.
    pub fn node(&self, name: impl Into<NodeName>) -> Node {
        let mut node = Node::branch(name, Vec::new()).with(Node::leaf(
            "reflected",
            Observed::value(Value::Bool(self.reflected)),
        ));
        for (stage, bindings) in &self.stages {
            node = node.with(match bindings {
                Some(bindings) => bindings_node(*stage, bindings),
                None => Node::leaf(*stage, Observed::value(Value::unit_variant("None"))),
            });
        }
        node
    }
}
