//! What a **tile render pipeline descriptor** answers of itself:
//! `MTLTileRenderPipelineDescriptor`'s own surface, and what `reset` puts back.
//!
//! [`crate::render_descriptors`] is the same question of `MTLRenderPipelineDescriptor`, and this
//! family shares that module's [`Shape`] rather than declaring a second one.
//!
//! `tile_threadgroups` reached four of this descriptor's fifteen items by **making pipelines** with
//! it — the tile function, the colour-attachment array, `threadgroupSizeMatchesTileSize` and
//! `maxTotalThreadsPerThreadgroup` — and nothing read a getter of the descriptor back. So the
//! label, the raster sample count, the per-buffer mutability, the call-stack depth, the
//! binary-function support, the required threadgroup size, the supplied-object lists and `reset`
//! were reached by nothing at all, and the runtime's
//! [`TileRenderPipelineDescriptor::new`](metal2vulkan::pipeline::TileRenderPipelineDescriptor::new)
//! holds each of their defaults as a literal read off Apple's documentation.
//!
//! `reset` is the same claim from the other end: Metal's descriptor is mutable and `reset` puts
//! every field back, while the runtime's counterpart is `TileRenderPipelineDescriptor::new` again
//! (the coverage map says so), because a fresh struct **is** the reset.
//!
//! # What the site observes, and what it cannot
//!
//! The colour attachments are recorded as their **pixel formats**, which is the whole of what
//! `MTLTileRenderPipelineColorAttachmentDescriptor` has: a tile attachment carries no blend state,
//! and the runtime's counterpart is an array of formats
//! ([`TileRenderPipelineDescriptor::color_attachment_formats`](metal2vulkan::pipeline::TileRenderPipelineDescriptor::color_attachment_formats)).
//!
//! Three getters answer **objects a program supplies**, and the runtime takes each one somewhere
//! other than the descriptor: `binaryArchives` is `PipelineOptions.binary_archives`,
//! `preloadedLibraries` is the tile stage's `FunctionDescriptor.libraries` and `linkedFunctions` is
//! its `FunctionDescriptor.linked`. For those the site records **how many** of each the list holds
//! and a nil list is none, so **this family compares those three items at their default alone** —
//! the lists start empty and a reset empties them, and nothing is said about a descriptor carrying
//! a real archive or dynamic library. That is a narrower case, not a loosened one;
//! [`crate::render_descriptors`] gives the reasoning in full.
//!
//! [`Shape::Reset`] leaves the two per-stage lists out, for the measured reason [`SuppliedLists`]
//! states.
//!
//! `shaderValidation` is absent: Apple's enumeration has no counterpart in the runtime.
//!
//! # Why every case of this family is safe
//!
//! Nothing here is compiled, allocated or submitted. A case makes one library, one function of it,
//! and a descriptor it never hands to `newRenderPipelineStateWithTileDescriptor:`. No command
//! buffer is made, so no case can fault the device, trip its watchdog or reach a queue.
//!
//! Two hazards of this descriptor's surface are named and avoided. `setLabel:` with nil ends
//! Metal's process, and no shape here sends it: the label is set only where the shape states one,
//! and the default shape reads the nil Metal's own `init` left. And `tileFunction` is declared
//! **non-null** by `objc2-metal` 0.3.2 although a fresh descriptor's is nil, so its accessor would
//! panic on a descriptor with no tile function — every shape of this family therefore sets the tile
//! function before reading, and the reset shape sets it again after the reset.

use crate::render_descriptors::{optional, ordinal, LinkedCounts, Shape};
use crate::transcript::{Node, NodeName, Observed, Value};
use metal2vulkan::metal;

/// Every field the set shape moves away from Metal's default, each to a value no other field of
/// the record holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Away {
    pub label: &'static str,
    pub raster_sample_count: u32,
    /// `colorAttachments[i].pixelFormat`, by index below
    /// [`crate::render_descriptors::READ_COLORS`].
    pub colors: &'static [(u32, metal::PixelFormat)],
    pub threadgroup_size_matches_tile_size: bool,
    /// `tileBuffers[i].mutability`, by index. Stated by [`Shape::Set`] **alone**, for the measured
    /// defect [`TileDescriptorFacts::buffers`] states: subscripting `tileBuffers` and then
    /// resetting the descriptor leaves a freed array behind, and the descriptor's own `dealloc`
    /// then ends the process. A shape that resets therefore never touches the array at all. The
    /// table is still part of every shape's input fingerprint, which over-covers rather than
    /// under-covers: a change to it invalidates the reset recording too.
    pub buffers: &'static [(u32, metal::Mutability)],
    pub max_total_threads_per_threadgroup: u32,
    pub support_adding_binary_functions: bool,
    pub max_call_stack_depth: u32,
    /// `requiredThreadsPerThreadgroup`; Metal's default is the zero size, no requirement.
    pub required_threads_per_threadgroup: [u32; 3],
}

/// The one table of stated values, each away from Metal's default.
///
/// The numbers are chosen apart: a raster sample count of 4, a thread limit of 144, a call-stack
/// depth of 5, and a required threadgroup of 12 by 6 by 2 whose three axes differ. No two are
/// equal, so a side answering one getter out of another's field reads a different number, and they
/// are deliberately **not** [`crate::compute_descriptors::AWAY`]'s numbers either: the two
/// descriptors' readers are two functions over the same selector names, and a line copied from one
/// into the other reads a number this family never stated.
///
/// The colour attachments state slots 0 and 2, so slot 1 is a format nothing touched and its answer
/// is Metal's default for a tile attachment, recorded beside two that were set.
pub const AWAY: Away = Away {
    label: "conformance-tile-descriptor",
    raster_sample_count: 4,
    colors: &[
        (0, metal::PixelFormat::RGBA8Unorm),
        (2, metal::PixelFormat::RGBA16Float),
    ],
    threadgroup_size_matches_tile_size: true,
    buffers: &[
        (0, metal::Mutability::Immutable),
        (2, metal::Mutability::Mutable),
    ],
    max_total_threads_per_threadgroup: 144,
    support_adding_binary_functions: true,
    max_call_stack_depth: 5,
    required_threads_per_threadgroup: [12, 6, 2],
};

impl Away {
    /// The words a case's input fingerprint covers: every stated field.
    pub fn words(&self) -> Vec<u8> {
        let mut out = self.label.as_bytes().to_vec();
        for word in [
            self.raster_sample_count,
            self.max_total_threads_per_threadgroup,
            self.max_call_stack_depth,
            self.required_threads_per_threadgroup[0],
            self.required_threads_per_threadgroup[1],
            self.required_threads_per_threadgroup[2],
        ] {
            out.extend(word.to_le_bytes());
        }
        for flag in [
            self.threadgroup_size_matches_tile_size,
            self.support_adding_binary_functions,
        ] {
            out.push(u8::from(flag));
        }
        for (index, format) in self.colors {
            out.extend(index.to_le_bytes());
            out.extend(format.as_raw().to_le_bytes());
        }
        for (index, mutability) in self.buffers {
            out.extend(index.to_le_bytes());
            out.extend(mutability.as_raw().to_le_bytes());
        }
        out
    }
}

/// What one case makes and reads: the tile function, and which of the three shapes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DescriptorSpec<'a> {
    /// `tileFunction`'s entry in the case's library.
    pub tile: &'a str,
    pub shape: Shape,
}

impl<'a> DescriptorSpec<'a> {
    /// The descriptor of `tile`, in `shape`.
    pub const fn new(tile: &'a str, shape: Shape) -> Self {
        Self { tile, shape }
    }

    /// The words a case's input fingerprint covers: the function, the shape, and — where the shape
    /// sets anything — the whole stated table.
    pub fn words(&self) -> Vec<u8> {
        let mut out = self.tile.as_bytes().to_vec();
        out.push(self.shape.word());
        if self.shape.sets() {
            out.extend(AWAY.words());
        }
        out
    }
}

/// The lists of objects a program supplies, by how many each holds.
///
/// [`Shape::Reset`] reads neither, for the reason
/// [`crate::render_descriptors::SuppliedLists`] measured on this family's oracle: `reset` leaves a
/// preloaded-library list nil where `init` leaves a non-nil empty array, and `objc2-metal` 0.3.2
/// declares both `MTLTileRenderPipelineDescriptor.preloadedLibraries` and its `linkedFunctions`
/// non-null, so each accessor panics on the nil rather than answering it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SuppliedLists {
    /// `preloadedLibraries`' length.
    pub preloaded_libraries: u64,
    /// `linkedFunctions`' four lengths; a list it answers nil for is 0.
    pub linked_functions: LinkedCounts,
}

/// What a tile render pipeline descriptor answers of itself. Each field is one Metal getter's
/// answer, an enumeration by its raw ordinal so an ordinal neither side names still compares.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TileDescriptorFacts {
    /// `label`; `None` is Metal's nil.
    pub label: Option<String>,
    /// `tileFunction`'s own `name`. Never `None`: every shape sets one, because Metal's nil is a
    /// value `objc2-metal`'s accessor cannot answer.
    pub tile_function: String,
    pub raster_sample_count: u64,
    /// `colorAttachments[i].pixelFormat` for `i` below
    /// [`crate::render_descriptors::READ_COLORS`].
    pub colors: Vec<u32>,
    pub threadgroup_size_matches_tile_size: bool,
    /// `tileBuffers[i].mutability` for `i` below [`crate::render_descriptors::READ_BUFFERS`], which
    /// [`Shape::Reset`] does not read.
    ///
    /// **Metal's `reset` leaves a freed array in `tileBuffers`**, measured on this family's oracle
    /// (an Apple M3 Max, macOS 27.0.1, build 26A434). Subscripting `tileBuffers` and then sending
    /// `reset` releases the descriptor's `MTLPipelineBufferDescriptorArrayInternal` without
    /// clearing the pointer to it, with two consequences, both reproduced twice in a row:
    ///
    /// - the next `tileBuffers` answers recycled memory, and sending `mutability` to one of its
    ///   elements dies with `invalid message send to
    ///   -[MTLTileRenderPipelineColorAttachmentDescriptorInternal mutability]: method not found`;
    /// - the descriptor's own `-[MTLTileRenderPipelineDescriptorInternal dealloc]` releases the
    ///   freed array again and dies in `objc_release` with `EXC_BAD_ACCESS`, whether or not
    ///   anything read the array after the reset. Under Metal's API validation layer that is where
    ///   the process goes.
    ///
    /// A descriptor whose `tileBuffers` was never subscripted before the `reset` survives both,
    /// which is how the dependence was narrowed, and neither
    /// `MTLComputePipelineDescriptor.buffers` nor `MTLRenderPipelineDescriptor`'s two arrays do any
    /// of it -- each is stated and read on its own family's reset shape. So this is one Metal
    /// class's defect, not a rule about `reset`.
    ///
    /// So [`Shape::Reset`] neither states nor reads `tileBuffers`: a shape that reset a descriptor
    /// whose array it had touched could not be a committed case at all, because releasing the
    /// descriptor ends the process. Nine of the descriptor's items are still compared after the
    /// reset, and the buffer mutability's round trip is compared on [`Shape::Set`].
    pub buffers: Option<Vec<u32>>,
    pub max_total_threads_per_threadgroup: u64,
    /// `binaryArchives`' length; a nil list is 0.
    pub binary_archives: u64,
    /// The two lists of supplied objects, which [`Shape::Reset`] does not read.
    pub supplied: Option<SuppliedLists>,
    pub support_adding_binary_functions: bool,
    pub max_call_stack_depth: u64,
    /// `requiredThreadsPerThreadgroup`, as three axes; the zero size where none is required.
    pub required_threads_per_threadgroup: [u64; 3],
}

impl TileDescriptorFacts {
    /// The facts as the sites a case emits under one node, so both sides emit one shape.
    pub fn node(&self, name: impl Into<NodeName>) -> Node {
        let value = Observed::value;
        let mut node = Node::branch(name, Vec::new())
            .with(Node::leaf("label", value(optional(&self.label))))
            .with(Node::leaf(
                "tile_function",
                value(Value::Lines(vec![self.tile_function.clone()])),
            ))
            .with(Node::leaf(
                "raster_sample_count",
                value(Value::Uint(self.raster_sample_count)),
            ))
            .with(Node::leaf(
                "color_attachment_formats",
                value(Value::List(
                    self.colors
                        .iter()
                        .map(|raw| ordinal(*raw, metal::PixelFormat::name))
                        .collect(),
                )),
            ))
            .with(Node::leaf(
                "threadgroup_size_matches_tile_size",
                value(Value::Bool(self.threadgroup_size_matches_tile_size)),
            ))
            .with(Node::leaf(
                "max_total_threads_per_threadgroup",
                value(Value::Uint(self.max_total_threads_per_threadgroup)),
            ))
            .with(Node::leaf(
                "binary_archives",
                value(Value::Uint(self.binary_archives)),
            ))
            .with(Node::leaf(
                "support_adding_binary_functions",
                value(Value::Bool(self.support_adding_binary_functions)),
            ))
            .with(Node::leaf(
                "max_call_stack_depth",
                value(Value::Uint(self.max_call_stack_depth)),
            ))
            .with(Node::leaf(
                "required_threads_per_threadgroup",
                value(crate::pipeline_facts::size(
                    &self.required_threads_per_threadgroup,
                )),
            ));
        if let Some(buffers) = &self.buffers {
            node = node.with(Node::leaf(
                "tile_buffers",
                value(Value::List(
                    buffers
                        .iter()
                        .map(|raw| ordinal(*raw, metal::Mutability::name))
                        .collect(),
                )),
            ));
        }
        if let Some(supplied) = &self.supplied {
            node = node
                .with(Node::leaf(
                    "preloaded_libraries",
                    value(Value::Uint(supplied.preloaded_libraries)),
                ))
                .with(Node::leaf(
                    "linked_functions",
                    value(supplied.linked_functions.value()),
                ));
        }
        node
    }
}
