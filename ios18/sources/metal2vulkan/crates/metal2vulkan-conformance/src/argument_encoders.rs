//! The argument-encoder vocabulary: what a case asks an `MTLArgumentEncoder` to do, and what it
//! can observe of the result.
//!
//! An argument encoder is how a Metal program hands a function the resources its argument buffer
//! names, and the suite had no vocabulary for it at all: [`Side::argument_buffer`] writes the
//! resource IDs by hand, as a Metal 3 program may, and so reaches no item of
//! `MTLArgumentEncoder`. This module is the other half — the encoder itself.
//!
//! [`Side::argument_buffer`]: crate::case::Side::argument_buffer
//!
//! # What is comparable here, and what is not
//!
//! The **words** an encoder writes are not comparable across the two sides and must never enter a
//! transcript: a buffer's argument is its Metal address plus an offset, and a texture's, a
//! sampler's and a pipeline state's are resource IDs. Each is an implementation's own number, and
//! a recording holds nothing host-specific ([`crate::transcript`]).
//!
//! Three things are exactly comparable, and between them they are the whole contract an argument
//! buffer's layout has:
//!
//! - **What the encoder says of itself**: `encodedLength`, `alignment`, `label` and whether
//!   `device` is the device the case's own objects were made on ([`EncoderFacts`]).
//! - **Where each argument landed**: the case fills the argument buffer with [`BLANK`] before the
//!   encoder touches it and then asks which eight-byte words changed ([`changed_words`]). A word
//!   an encoder wrote is an address or a resource ID, and neither is ever the pattern, so the mask
//!   is a fact about the **layout** — the offset, the stride and the alignment of every argument —
//!   with none of the host's own numbers in it. Pre-filling is what makes the question falsifiable:
//!   over a buffer created zeroed, an encoder that wrote nothing and one that wrote a zero word
//!   read alike.
//! - **What the function read**: a case that dispatches a kernel over the encoded buffer compares
//!   the words the kernel wrote, which is the end-to-end answer and the one a wrong layout shows up
//!   in as plausible-looking wrong output rather than as a crash.
//!
//! `constantDataAtIndex:` answers a CPU address, which is host-specific for the same reason. What
//! a case compares is its **offset inside the arguments** ([`Side::constant_data_offset`]), which
//! is the layout fact the address stands for.
//!
//! [`Side::constant_data_offset`]: crate::case::Side::constant_data_offset

use crate::case::Handle;
use crate::transcript::{ByteLayout, Element, Gaps, Node, Observed, Region, Shape, Value};
use metal2vulkan::metal::{BindingAccess, DataType, TextureType};

/// The byte a case writes over an argument buffer before an encoder touches it.
///
/// Neither an address nor a resource ID is ever `0xabababababababab`, so a word holding the
/// pattern is a word the encoder did not write. The value is the one the sampler families already
/// blank their result buffers with, so a reader meets one pattern in this suite rather than two.
pub const BLANK: u8 = 0xab;

/// The bytes one argument of an argument buffer takes, which is what a word of the changed-word
/// mask covers (`ARGUMENT_HANDLE_BYTES`).
pub const ARGUMENT_WORD: usize = 8;

/// One argument of an argument buffer a case describes itself: `MTLArgumentDescriptor`'s fields,
/// as `newArgumentEncoderWithArguments:` takes them.
///
/// The vocabulary is Metal's own enumerations as the translator's public API names them, so a
/// value a case writes reaches `MTLDataType`, `MTLBindingAccess` and `MTLTextureType` by their raw
/// ordinals and reaches the runtime's own descriptor unchanged. There is no second spelling to keep
/// in step.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ArgumentDescriptorSpec {
    /// `dataType`.
    pub data_type: DataType,
    /// `index`: the argument's index, or an array's element 0.
    pub index: u32,
    /// `arrayLength`: 0 for a single argument.
    pub array_length: u32,
    /// `access`, which changes no byte of the layout.
    pub access: BindingAccess,
    /// `textureType`, which changes no byte of the layout.
    pub texture_type: TextureType,
    /// `constantBlockAlignment`: 0 for the type's own alignment.
    pub constant_block_alignment: u32,
}

impl ArgumentDescriptorSpec {
    /// An argument of `data_type` at `index`, with Metal's defaults for the rest: single,
    /// read-only, a 2D texture type, and the type's own alignment.
    pub const fn new(data_type: DataType, index: u32) -> Self {
        Self {
            data_type,
            index,
            array_length: 0,
            access: BindingAccess::ReadOnly,
            texture_type: TextureType::Type2D,
            constant_block_alignment: 0,
        }
    }

    /// The descriptor as an array of `length` elements from its index.
    #[must_use]
    pub const fn array(mut self, length: u32) -> Self {
        self.array_length = length;
        self
    }

    /// The descriptor with `access`.
    #[must_use]
    pub const fn accessed(mut self, access: BindingAccess) -> Self {
        self.access = access;
        self
    }

    /// The descriptor with `textureType`.
    #[must_use]
    pub const fn of_texture_type(mut self, texture_type: TextureType) -> Self {
        self.texture_type = texture_type;
        self
    }

    /// The descriptor with `constantBlockAlignment`.
    #[must_use]
    pub const fn block_aligned(mut self, alignment: u32) -> Self {
        self.constant_block_alignment = alignment;
        self
    }

    /// The words a case's input fingerprint covers: every field, so a descriptor that moves is a
    /// change to the case's inputs.
    pub fn words(&self) -> Vec<u8> {
        [
            self.data_type.as_raw(),
            self.index,
            self.array_length,
            self.access.as_raw(),
            self.texture_type.as_raw(),
            self.constant_block_alignment,
        ]
        .iter()
        .flat_map(|word| word.to_le_bytes())
        .collect()
    }
}

/// One write a case asks an argument encoder to make.
///
/// Every variant is one `MTLArgumentEncoder` call, named by [`ArgumentWrite::call`], so a refusal
/// reads as the Metal call we refused rather than as a step number. `None` is Metal's nil, which
/// writes a zero word.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ArgumentWrite {
    /// `setTexture:atIndex:`.
    Texture { texture: Option<Handle>, index: u32 },
    /// `setTextures:withRange:`, from index `first`.
    Textures {
        textures: Vec<Option<Handle>>,
        first: u32,
    },
    /// `setSamplerState:atIndex:`.
    Sampler { sampler: Option<Handle>, index: u32 },
    /// `setSamplerStates:withRange:`, from index `first`.
    Samplers {
        samplers: Vec<Option<Handle>>,
        first: u32,
    },
    /// `setBuffer:offset:atIndex:`.
    Buffer {
        buffer: Option<Handle>,
        offset: u64,
        index: u32,
    },
    /// `setBuffers:offsets:withRange:`, from index `first`.
    Buffers {
        buffers: Vec<(Option<Handle>, u64)>,
        first: u32,
    },
    /// `setComputePipelineState:atIndex:`.
    ComputePipelineState { state: Option<Handle>, index: u32 },
    /// `setComputePipelineStates:withRange:`, from index `first`.
    ComputePipelineStates {
        states: Vec<Option<Handle>>,
        first: u32,
    },
    /// `setRenderPipelineState:atIndex:`.
    RenderPipelineState { state: Option<Handle>, index: u32 },
    /// `setRenderPipelineStates:withRange:`, from index `first`.
    RenderPipelineStates {
        states: Vec<Option<Handle>>,
        first: u32,
    },
    /// `setIndirectCommandBuffer:atIndex:`.
    IndirectCommandBuffer {
        commands: Option<Handle>,
        index: u32,
    },
    /// `setIndirectCommandBuffers:withRange:`, from index `first`.
    IndirectCommandBuffers {
        commands: Vec<Option<Handle>>,
        first: u32,
    },
    /// `setVisibleFunctionTable:atIndex:`.
    VisibleFunctionTable { table: Option<Handle>, index: u32 },
    /// `setVisibleFunctionTables:withRange:`, from index `first`.
    VisibleFunctionTables {
        tables: Vec<Option<Handle>>,
        first: u32,
    },
    /// `constantDataAtIndex:`, and the bytes the program then writes there itself. A constant has
    /// no setter: the encoder answers where it goes and the program fills it.
    Constant { index: u32, bytes: Vec<u8> },
}

impl ArgumentWrite {
    /// The site name the write's answer is emitted at: the Metal call it makes.
    pub fn call(&self) -> &'static str {
        match self {
            ArgumentWrite::Texture { .. } => "setTexture",
            ArgumentWrite::Textures { .. } => "setTextures",
            ArgumentWrite::Sampler { .. } => "setSamplerState",
            ArgumentWrite::Samplers { .. } => "setSamplerStates",
            ArgumentWrite::Buffer { .. } => "setBuffer",
            ArgumentWrite::Buffers { .. } => "setBuffers",
            ArgumentWrite::ComputePipelineState { .. } => "setComputePipelineState",
            ArgumentWrite::ComputePipelineStates { .. } => "setComputePipelineStates",
            ArgumentWrite::RenderPipelineState { .. } => "setRenderPipelineState",
            ArgumentWrite::RenderPipelineStates { .. } => "setRenderPipelineStates",
            ArgumentWrite::IndirectCommandBuffer { .. } => "setIndirectCommandBuffer",
            ArgumentWrite::IndirectCommandBuffers { .. } => "setIndirectCommandBuffers",
            ArgumentWrite::VisibleFunctionTable { .. } => "setVisibleFunctionTable",
            ArgumentWrite::VisibleFunctionTables { .. } => "setVisibleFunctionTables",
            ArgumentWrite::Constant { .. } => "constantDataAtIndex",
        }
    }

    /// The textures this write names, which a side that binds the argument buffer makes resident:
    /// Metal keeps a resource a function reaches through an argument buffer resident only where it
    /// was told so.
    pub fn textures(&self) -> Vec<Handle> {
        match self {
            ArgumentWrite::Texture { texture, .. } => texture.iter().copied().collect(),
            ArgumentWrite::Textures { textures, .. } => {
                textures.iter().flatten().copied().collect()
            }
            _ => Vec::new(),
        }
    }

    /// The words the write fingerprints as: the call, the indices, and the bytes a constant
    /// carries. A handle is the side's own number and is **not** a word here, as no recording
    /// holds one; what identifies the resource is the case's own bytes for it.
    pub fn words(&self) -> Vec<u8> {
        let mut out = self.call().as_bytes().to_vec();
        let mut index = |value: u32| out.extend(value.to_le_bytes());
        match self {
            ArgumentWrite::Texture { index: at, .. }
            | ArgumentWrite::Sampler { index: at, .. }
            | ArgumentWrite::ComputePipelineState { index: at, .. }
            | ArgumentWrite::RenderPipelineState { index: at, .. }
            | ArgumentWrite::IndirectCommandBuffer { index: at, .. }
            | ArgumentWrite::VisibleFunctionTable { index: at, .. } => index(*at),
            ArgumentWrite::Buffer {
                offset, index: at, ..
            } => {
                index(*at);
                out.extend(offset.to_le_bytes());
            }
            ArgumentWrite::Textures { textures, first } => {
                index(*first);
                index(textures.len() as u32);
            }
            ArgumentWrite::Samplers { samplers, first } => {
                index(*first);
                index(samplers.len() as u32);
            }
            ArgumentWrite::ComputePipelineStates { states, first }
            | ArgumentWrite::RenderPipelineStates { states, first } => {
                index(*first);
                index(states.len() as u32);
            }
            ArgumentWrite::IndirectCommandBuffers { commands, first } => {
                index(*first);
                index(commands.len() as u32);
            }
            ArgumentWrite::VisibleFunctionTables { tables, first } => {
                index(*first);
                index(tables.len() as u32);
            }
            ArgumentWrite::Buffers { buffers, first } => {
                index(*first);
                index(buffers.len() as u32);
                for (_, offset) in buffers {
                    out.extend(offset.to_le_bytes());
                }
            }
            ArgumentWrite::Constant { index: at, bytes } => {
                index(*at);
                out.extend(bytes);
            }
        }
        out
    }
}

/// What an argument encoder answers of itself. Each field is one Metal getter's answer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EncoderFacts {
    /// `encodedLength`: the bytes the encoder's arguments take in an argument buffer.
    pub encoded_length: u64,
    /// `alignment`.
    pub alignment: u64,
    /// `label`, read back after `setLabel:`.
    pub label: Option<String>,
    /// Whether `device` is the device the case's other objects were made on.
    ///
    /// The device object itself is not comparable — its name and its identity are the host's — so
    /// what the case asks is the one portable question the getter answers: did the encoder come
    /// from the case's own device.
    pub device_is_the_cases: bool,
}

impl EncoderFacts {
    /// The facts as the sites a case emits under one node, so both sides emit one shape.
    pub fn node(&self, name: impl Into<crate::transcript::NodeName>) -> Node {
        let label = match &self.label {
            Some(label) => Value::Variant {
                name: "Some".to_owned(),
                payload: Box::new(Value::Lines(vec![label.clone()])),
            },
            None => Value::unit_variant("None"),
        };
        Node::branch(name, Vec::new())
            .with(Node::leaf(
                "encoded_length",
                Observed::value(Value::Uint(self.encoded_length)),
            ))
            .with(Node::leaf(
                "alignment",
                Observed::value(Value::Uint(self.alignment)),
            ))
            .with(Node::leaf("label", Observed::value(label)))
            .with(Node::leaf(
                "device",
                Observed::value(Value::Bool(self.device_is_the_cases)),
            ))
    }
}

/// Which eight-byte words of an argument buffer the encoder wrote: 1 for a word that no longer
/// holds [`BLANK`], 0 for one it left alone.
///
/// `after` is the whole argument buffer as the host read it back. Only the first `words` words are
/// reported, so a case states how much of the buffer it is asking about rather than letting its
/// own buffer length into the answer.
pub fn changed_words(after: &[u8], words: usize) -> Vec<u8> {
    (0..words)
        .map(|word| {
            let at = word * ARGUMENT_WORD;
            let held = after.get(at..at + ARGUMENT_WORD);
            match held {
                Some(bytes) if bytes.iter().all(|byte| *byte == BLANK) => 0,
                // A word the read-back did not reach is reported as changed rather than silently
                // as untouched: a case that asked about more words than it read has a defect, and
                // 1 here makes it a difference rather than a quiet agreement.
                _ => 1,
            }
        })
        .collect()
}

/// The site the changed-word mask sits at: one element a word, so a diff names the **argument**
/// whose placement moved rather than a byte offset.
///
/// `offset` is where the arguments start in the argument buffer, which the region records so a
/// reader can see which bytes the mask is of.
pub fn changed_node(
    name: impl Into<crate::transcript::NodeName>,
    mask: Vec<u8>,
    offset: u64,
) -> Node {
    let words = u32::try_from(mask.len()).unwrap_or(u32::MAX);
    Node::leaf(
        name,
        Observed {
            value: Value::Bytes(mask),
            shape: Shape::Bytes(ByteLayout {
                region: Region::Buffer {
                    offset,
                    length: u64::from(words) * ARGUMENT_WORD as u64,
                },
                element: Element::U8,
                row_offset: 0,
                row_elements: words,
                row_pitch: words,
                rows: 1,
                image_pitch: words,
                images: 1,
                gaps: Gaps::Compared,
            }),
            digest: None,
        },
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The mask reads a word as written only where it no longer holds the pattern, and a word the
    /// read-back did not reach reads as written rather than as untouched.
    #[test]
    fn the_mask_is_one_byte_a_word_and_a_short_read_is_not_agreement() {
        let mut bytes = vec![BLANK; 24];
        bytes[8] = 0;
        assert_eq!(changed_words(&bytes, 3), [0, 1, 0]);
        assert_eq!(changed_words(&bytes, 4), [0, 1, 0, 1]);
        assert_eq!(changed_words(&[], 1), [1]);
    }

    /// A write's fingerprint carries no handle: a handle is the side's own number, and the two
    /// sides' pools need not agree on it.
    #[test]
    fn a_writes_fingerprint_holds_no_handle() {
        let one = ArgumentWrite::Texture {
            texture: Some(Handle(3)),
            index: 1,
        };
        let other = ArgumentWrite::Texture {
            texture: Some(Handle(9)),
            index: 1,
        };
        assert_eq!(one.words(), other.words());
        assert_ne!(
            one.words(),
            ArgumentWrite::Texture {
                texture: Some(Handle(3)),
                index: 2,
            }
            .words()
        );
    }
}
