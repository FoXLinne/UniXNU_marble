//! What a function's reflection says of its arguments: `MTLBinding` and the types under it.
//!
//! This is the other half of the argument-buffer story. [`crate::argument_encoders`] compares where
//! an encoder **puts** each argument; this compares what Metal says the argument **is** — and for a
//! buffer whose struct type is an argument buffer, that struct type is the layout contract itself:
//! every member's `offset`, its `argumentIndex`, an array's `stride` and `argumentIndexStride`, a
//! pointer's `alignment` and `dataSize`. A suite that compares the encoder's bytes and not this
//! would be checking the answer without checking the question.
//!
//! Every field here is a number, an enumeration or a name, so the whole tree compares exactly and
//! nothing in it is the host's own.
//!
//! # `MTLArgument` is not what this reads, and cannot be
//!
//! `MTLArgument` states the same seventeen facts in the spelling Apple deprecated in favour of
//! `MTLBinding`, and the coverage map's own counterpart for every one of them is this crate's
//! `Binding`. Reaching them would mean calling `arguments` and `MTLArgument`'s getters, every one
//! of which `objc2-metal` marks `#[deprecated]`: under this tree's warnings-are-errors rule that
//! does not build, and the only way to make it build is the `#[allow]` the repository forbids. So
//! the facts are compared here through the type Apple replaced it with, and the `MTLArgument` rows
//! of the coverage document stay unexercised with this as the reason.
//!
//! # The depth bound
//!
//! A member's type can hold another type, and a case must not walk an unbounded tree. Both sides
//! walk at most [`DEPTH`] levels and then record [`TypeFacts::Deeper`], so the bound is part of the
//! vocabulary rather than one side's choice: a type nested deeper than any of this suite's
//! fixtures goes is a readable difference instead of an unbounded walk.

use crate::transcript::{Node, NodeName, Observed, Value};
use metal2vulkan::metal::{BindingAccess, BindingType, DataType, TextureType};

/// How many levels of a member's own type the two sides walk. The fixtures here nest two deep (an
/// argument buffer's struct, and a member's texture reference or pointer), so this is slack rather
/// than a limit either side reaches.
pub const DEPTH: u32 = 4;

/// What one reflected argument answers: `MTLBinding`'s own fields, and the fields of the
/// subprotocol its `type` says it conforms to.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BindingFacts {
    /// `name`.
    pub name: String,
    /// `type`.
    pub binding_type: BindingType,
    /// `access`.
    pub access: BindingAccess,
    /// `index`.
    pub index: u32,
    /// `isUsed`.
    pub used: bool,
    /// `isArgument`.
    pub argument: bool,
    /// `MTLBufferBinding`'s fields, for a buffer argument.
    pub buffer: Option<BufferFacts>,
    /// `MTLThreadgroupBinding`'s fields, for a threadgroup-memory argument.
    pub threadgroup: Option<ThreadgroupFacts>,
    /// `MTLTextureBinding`'s fields, for a texture argument.
    pub texture: Option<TextureFacts>,
    /// `MTLObjectPayloadBinding`'s fields, for an object or mesh function's payload.
    pub payload: Option<PayloadFacts>,
}

/// `MTLBufferBinding`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BufferFacts {
    /// `bufferAlignment`.
    pub alignment: u64,
    /// `bufferDataSize`.
    pub data_size: u64,
    /// `bufferDataType`.
    pub data_type: DataType,
    /// `bufferStructType`'s members, where it answers one.
    pub struct_type: Option<StructFacts>,
    /// `bufferPointerType`, where it answers one.
    pub pointer_type: Option<PointerFacts>,
}

/// `MTLThreadgroupBinding`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ThreadgroupFacts {
    /// `threadgroupMemoryAlignment`.
    pub alignment: u64,
    /// `threadgroupMemoryDataSize`.
    pub data_size: u64,
}

/// `MTLObjectPayloadBinding`: the payload an object function writes and a mesh function reads.
///
/// Metal states only its size and its alignment, and neither is in the source: the payload's
/// struct is declared in MSL and these two numbers are what Metal's own layout rules make of it,
/// which is the same class of derivation [`BufferFacts::alignment`] and
/// [`BufferFacts::data_size`] are for a buffer's pointee.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PayloadFacts {
    /// `objectPayloadAlignment`.
    pub alignment: u64,
    /// `objectPayloadDataSize`.
    pub data_size: u64,
}

/// `MTLTextureBinding`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TextureFacts {
    /// `textureType`.
    pub texture_type: TextureType,
    /// `textureDataType`.
    pub data_type: DataType,
    /// `isDepthTexture`.
    pub depth: bool,
    /// `arrayLength`.
    pub array_length: u32,
}

/// `MTLStructType`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StructFacts {
    /// `members`, in declaration order.
    pub members: Vec<MemberFacts>,
    /// Whether `memberByName:` of every member's own name answered that same member.
    ///
    /// A bool rather than the members again: what the getter is for is finding a member by name,
    /// and the fact worth comparing is that it finds the one `members` holds. A side whose lookup
    /// answered another member, or nothing, says `false` here and the difference is readable.
    pub found_by_name: bool,
}

/// `MTLStructMember`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MemberFacts {
    /// `name`.
    pub name: String,
    /// `offset`.
    pub offset: u64,
    /// `dataType`.
    pub data_type: DataType,
    /// `argumentIndex`.
    pub argument_index: u32,
    /// Which of `structType`, `arrayType`, `textureReferenceType` and `pointerType` the member
    /// answers, and what it holds.
    pub kind: TypeFacts,
}

/// Which of a member's or an array element's four type getters answers, and what it holds.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TypeFacts {
    /// All four answered nil: the member is its data type and nothing more.
    Plain,
    /// `structType`, or an array's or a pointer's `elementStructType`.
    Struct(StructFacts),
    /// `arrayType`, or an array's or a pointer's `elementArrayType`.
    Array(Box<ArrayFacts>),
    /// `textureReferenceType`, or an array's `elementTextureReferenceType`.
    TextureReference(ReferenceFacts),
    /// `pointerType`, or an array's `elementPointerType`.
    Pointer(Box<PointerFacts>),
    /// The walk reached [`DEPTH`] and stopped. A fact of the vocabulary, not of either side.
    Deeper,
}

/// `MTLArrayType`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ArrayFacts {
    /// `elementType`.
    pub element_type: DataType,
    /// `arrayLength`.
    pub array_length: u32,
    /// `stride`.
    pub stride: u64,
    /// `argumentIndexStride`.
    pub argument_index_stride: u32,
    /// What the element is beyond its data type.
    pub element: TypeFacts,
}

/// `MTLPointerType`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PointerFacts {
    /// `elementType`.
    pub element_type: DataType,
    /// `access`.
    pub access: BindingAccess,
    /// `alignment`.
    pub alignment: u64,
    /// `dataSize`.
    pub data_size: u64,
    /// `elementIsArgumentBuffer`.
    pub element_is_argument_buffer: bool,
    /// What the pointee is beyond its data type.
    pub element: TypeFacts,
}

/// `MTLTextureReferenceType`: an argument-buffer member's texture.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ReferenceFacts {
    /// `textureDataType`.
    pub data_type: DataType,
    /// `textureType`.
    pub texture_type: TextureType,
    /// `access`.
    pub access: BindingAccess,
    /// `isDepthTexture`.
    pub depth: bool,
}

/// An enumeration as a site records it: its Metal enumerator name, so a diff reads
/// `MTLDataTypeFloat2` rather than 4 and a value neither side has a name for is still readable.
fn named(name: &'static str, raw: u32) -> Value {
    Value::fields([
        ("name", Value::Lines(vec![name.to_owned()])),
        ("raw", Value::Uint(u64::from(raw))),
    ])
}

/// A `MTLDataType` as a site records it.
fn data_type(value: DataType) -> Value {
    named(value.name(), value.as_raw())
}

/// A `MTLBindingAccess` as a site records it.
fn access(value: BindingAccess) -> Value {
    named(value.name(), value.as_raw())
}

/// A `MTLTextureType` as a site records it.
fn texture_type(value: TextureType) -> Value {
    named(value.name(), value.as_raw())
}

impl BindingFacts {
    /// The facts as the sites a case emits under one node, so both sides emit one shape.
    pub fn node(&self, name: impl Into<NodeName>) -> Node {
        let mut node = Node::branch(name, Vec::new())
            .with(Node::leaf(
                "name",
                Observed::value(Value::Lines(vec![self.name.clone()])),
            ))
            .with(Node::leaf(
                "type",
                Observed::value(named(self.binding_type.name(), self.binding_type.as_raw())),
            ))
            .with(Node::leaf("access", Observed::value(access(self.access))))
            .with(Node::leaf(
                "index",
                Observed::value(Value::Uint(u64::from(self.index))),
            ))
            .with(Node::leaf("used", Observed::value(Value::Bool(self.used))))
            .with(Node::leaf(
                "argument",
                Observed::value(Value::Bool(self.argument)),
            ));
        if let Some(buffer) = &self.buffer {
            node = node.with(buffer.node("buffer"));
        }
        if let Some(threadgroup) = &self.threadgroup {
            node = node.with(
                Node::branch("threadgroup", Vec::new())
                    .with(Node::leaf(
                        "alignment",
                        Observed::value(Value::Uint(threadgroup.alignment)),
                    ))
                    .with(Node::leaf(
                        "data_size",
                        Observed::value(Value::Uint(threadgroup.data_size)),
                    )),
            );
        }
        if let Some(texture) = &self.texture {
            node = node.with(
                Node::branch("texture", Vec::new())
                    .with(Node::leaf(
                        "texture_type",
                        Observed::value(texture_type(texture.texture_type)),
                    ))
                    .with(Node::leaf(
                        "data_type",
                        Observed::value(data_type(texture.data_type)),
                    ))
                    .with(Node::leaf(
                        "depth",
                        Observed::value(Value::Bool(texture.depth)),
                    ))
                    .with(Node::leaf(
                        "array_length",
                        Observed::value(Value::Uint(u64::from(texture.array_length))),
                    )),
            );
        }
        if let Some(payload) = &self.payload {
            node = node.with(
                Node::branch("payload", Vec::new())
                    .with(Node::leaf(
                        "alignment",
                        Observed::value(Value::Uint(payload.alignment)),
                    ))
                    .with(Node::leaf(
                        "data_size",
                        Observed::value(Value::Uint(payload.data_size)),
                    )),
            );
        }
        node
    }
}

impl BufferFacts {
    fn node(&self, name: impl Into<NodeName>) -> Node {
        let mut node = Node::branch(name, Vec::new())
            .with(Node::leaf(
                "alignment",
                Observed::value(Value::Uint(self.alignment)),
            ))
            .with(Node::leaf(
                "data_size",
                Observed::value(Value::Uint(self.data_size)),
            ))
            .with(Node::leaf(
                "data_type",
                Observed::value(data_type(self.data_type)),
            ));
        node = node.with(match &self.struct_type {
            Some(held) => held.node("struct_type"),
            None => Node::leaf("struct_type", Observed::value(Value::unit_variant("None"))),
        });
        node = node.with(match &self.pointer_type {
            Some(held) => held.node("pointer_type"),
            None => Node::leaf("pointer_type", Observed::value(Value::unit_variant("None"))),
        });
        node
    }
}

impl StructFacts {
    fn node(&self, name: impl Into<NodeName>) -> Node {
        let mut node = Node::branch(name, Vec::new())
            .with(Node::leaf(
                "members",
                Observed::value(Value::Uint(self.members.len() as u64)),
            ))
            .with(Node::leaf(
                "found_by_name",
                Observed::value(Value::Bool(self.found_by_name)),
            ));
        for (at, member) in self.members.iter().enumerate() {
            node = node.with(member.node(format!("member/{at}")));
        }
        node
    }
}

impl MemberFacts {
    fn node(&self, name: impl Into<NodeName>) -> Node {
        Node::branch(name, Vec::new())
            .with(Node::leaf(
                "name",
                Observed::value(Value::Lines(vec![self.name.clone()])),
            ))
            .with(Node::leaf(
                "offset",
                Observed::value(Value::Uint(self.offset)),
            ))
            .with(Node::leaf(
                "data_type",
                Observed::value(data_type(self.data_type)),
            ))
            .with(Node::leaf(
                "argument_index",
                Observed::value(Value::Uint(u64::from(self.argument_index))),
            ))
            .with(self.kind.node("kind"))
    }
}

impl ArrayFacts {
    fn node(&self, name: impl Into<NodeName>) -> Node {
        Node::branch(name, Vec::new())
            .with(Node::leaf(
                "element_type",
                Observed::value(data_type(self.element_type)),
            ))
            .with(Node::leaf(
                "array_length",
                Observed::value(Value::Uint(u64::from(self.array_length))),
            ))
            .with(Node::leaf(
                "stride",
                Observed::value(Value::Uint(self.stride)),
            ))
            .with(Node::leaf(
                "argument_index_stride",
                Observed::value(Value::Uint(u64::from(self.argument_index_stride))),
            ))
            .with(self.element.node("element"))
    }
}

impl PointerFacts {
    fn node(&self, name: impl Into<NodeName>) -> Node {
        Node::branch(name, Vec::new())
            .with(Node::leaf(
                "element_type",
                Observed::value(data_type(self.element_type)),
            ))
            .with(Node::leaf("access", Observed::value(access(self.access))))
            .with(Node::leaf(
                "alignment",
                Observed::value(Value::Uint(self.alignment)),
            ))
            .with(Node::leaf(
                "data_size",
                Observed::value(Value::Uint(self.data_size)),
            ))
            .with(Node::leaf(
                "element_is_argument_buffer",
                Observed::value(Value::Bool(self.element_is_argument_buffer)),
            ))
            .with(self.element.node("element"))
    }
}

impl TypeFacts {
    /// The kind as one site, whose name says which getter answered and whose children hold what it
    /// answered.
    pub fn node(&self, name: impl Into<NodeName>) -> Node {
        let name = name.into();
        match self {
            TypeFacts::Plain => Node::leaf(name, Observed::value(Value::unit_variant("Plain"))),
            TypeFacts::Deeper => Node::leaf(name, Observed::value(Value::unit_variant("Deeper"))),
            TypeFacts::Struct(held) => Node::branch(name.clone(), Vec::new())
                .with(Node::leaf(
                    "which",
                    Observed::value(Value::unit_variant("Struct")),
                ))
                .with(held.node("struct")),
            TypeFacts::Array(held) => Node::branch(name.clone(), Vec::new())
                .with(Node::leaf(
                    "which",
                    Observed::value(Value::unit_variant("Array")),
                ))
                .with(held.node("array")),
            TypeFacts::Pointer(held) => Node::branch(name.clone(), Vec::new())
                .with(Node::leaf(
                    "which",
                    Observed::value(Value::unit_variant("Pointer")),
                ))
                .with(held.node("pointer")),
            TypeFacts::TextureReference(held) => Node::branch(name.clone(), Vec::new())
                .with(Node::leaf(
                    "which",
                    Observed::value(Value::unit_variant("TextureReference")),
                ))
                .with(
                    Node::branch("texture_reference", Vec::new())
                        .with(Node::leaf(
                            "data_type",
                            Observed::value(data_type(held.data_type)),
                        ))
                        .with(Node::leaf(
                            "texture_type",
                            Observed::value(texture_type(held.texture_type)),
                        ))
                        .with(Node::leaf("access", Observed::value(access(held.access))))
                        .with(Node::leaf(
                            "depth",
                            Observed::value(Value::Bool(held.depth)),
                        )),
                ),
        }
    }
}

/// `MTLRenderPipelineReflection`: **all five** of its stage lists, whichever kind of render
/// pipeline answered it.
///
/// The five are always recorded, for a pipeline of every kind, and that is the point of the type.
/// A tile pipeline's reflection answers tile bindings and four empty lists; a mesh pipeline's
/// answers object, mesh and fragment and two empty ones; a drawing pipeline's answers vertex and
/// fragment and three empty ones (all three measured on an M3 Max under Metal 27.0.1). So the
/// **shape** of the five is itself the comparable fact: a side that answered a tile function's
/// arguments under `fragmentBindings`, or a mesh function's under `objectBindings`, holds the same
/// bindings and differs here, and nothing else in a reflection would see it. An empty list
/// compared against an empty list says nothing on its own, but the five sites together say which
/// stage Metal attributes each argument to, and that is a fact with five possible answers.
#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub struct StageBindings {
    /// `vertexBindings`.
    pub vertex: Vec<BindingFacts>,
    /// `fragmentBindings`.
    pub fragment: Vec<BindingFacts>,
    /// `tileBindings`.
    pub tile: Vec<BindingFacts>,
    /// `objectBindings`.
    pub object: Vec<BindingFacts>,
    /// `meshBindings`.
    pub mesh: Vec<BindingFacts>,
}

impl StageBindings {
    /// The five lists as the sites a case emits under one node, so both sides emit one shape.
    pub fn node(&self, name: impl Into<NodeName>) -> Node {
        Node::branch(name, Vec::new())
            .with(bindings_node("vertex", &self.vertex))
            .with(bindings_node("fragment", &self.fragment))
            .with(bindings_node("tile", &self.tile))
            .with(bindings_node("object", &self.object))
            .with(bindings_node("mesh", &self.mesh))
    }
}

/// What one render-pipeline creation under one `MTLPipelineOption` value answered: whether it was
/// refused, and the reflection's five stage lists where one came back.
///
/// This is [`crate::pipeline_option_reflection::UnderOptions`]'s shape for a render pipeline
/// rather than a compute one — the same two questions, over five stage lists instead of one
/// binding list — and it is a separate type rather than a widening of that one because the two
/// creations answer two different reflection classes (`MTLComputePipelineReflection` and
/// `MTLRenderPipelineReflection`), and a type that held both would have to say which half is
/// absent at every site.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StageReflection {
    /// Whether the creation was refused.
    ///
    /// A bool and not the error, for the reason
    /// [`UnderOptions::refused`](crate::pipeline_option_reflection::UnderOptions::refused) states.
    /// Every row of an options sweep must answer `false`: a creation plain Metal makes and we
    /// refuse is a gap of ours.
    pub refused: bool,
    /// The five stage lists, or `None` where the creation answered no reflection at all
    /// (`MTLPipelineOptionNone`, as Metal leaves `reflection` nil).
    pub stages: Option<StageBindings>,
}

impl StageReflection {
    /// The answer of a creation that was refused.
    pub fn refused() -> Self {
        Self {
            refused: true,
            stages: None,
        }
    }

    /// The answer of a creation that was made and reflected `stages`.
    pub fn reflected(stages: StageBindings) -> Self {
        Self {
            refused: false,
            stages: Some(stages),
        }
    }

    /// The answer of a creation that was made and answered no reflection.
    pub fn unreflected() -> Self {
        Self {
            refused: false,
            stages: None,
        }
    }

    /// The sites a case emits under one node, so both sides emit one shape.
    pub fn node(&self, name: impl Into<NodeName>) -> Node {
        let node = Node::branch(name, Vec::new()).with(Node::leaf(
            "refused",
            Observed::value(Value::Bool(self.refused)),
        ));
        match &self.stages {
            Some(stages) => node.with(stages.node("reflection")),
            None => node.with(Node::leaf(
                "reflection",
                Observed::value(Value::unit_variant("None")),
            )),
        }
    }
}

/// The node a case emits for every binding a function's reflection answered.
pub fn bindings_node(name: impl Into<NodeName>, bindings: &[BindingFacts]) -> Node {
    let mut node = Node::branch(name, Vec::new()).with(Node::leaf(
        "count",
        Observed::value(Value::Uint(bindings.len() as u64)),
    ));
    // By the argument's own index and name rather than by its position in Metal's array: the
    // order a reflection lists its bindings in is not something either side promises, and a site
    // named by the argument compares the same argument on both sides.
    for binding in bindings {
        node = node.with(binding.node(format!(
            "{}/{}/{}",
            binding.binding_type.name(),
            binding.index,
            binding.name
        )));
    }
    node
}
