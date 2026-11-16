//! `PipelineOption` (`MTLPipelineOption`) and `PipelineOptions`, what a pipeline state's creation
//! takes beside its function or descriptor.
//!
//! - **Every bit is kept.** Metal accepts any raw value, undefined bits included (measured on an
//!   M3 Max and an M4, validated or not: 8, 16, `1 << 10`, `1 << 31`, `1 << 40`), so the runtime
//!   keeps the raw value and acts only on the bits Metal defines.
//! - **Reflection.** Any option word but `NONE` asks the `_with_reflection` creation forms for the
//!   pipeline's reflection, and every one of them gets the same full reflection, struct and
//!   pointer types included: `BUFFER_TYPE_INFO` adds nothing over `BINDING_INFO`, and neither does
//!   `FAIL_ON_BINARY_ARCHIVE_MISS` on its own or an undefined bit on its own. Metal tests the word
//!   against zero and nothing finer (measured on an M3 Max under macOS 27.0.1, on a compute and a
//!   render creation, at `FAIL_ON_BINARY_ARCHIVE_MISS`, 8, `1 << 31` and `1 << 40`, each of which
//!   reflects, and at `NONE`, which does not; the conformance suite's `pipeline_options` family
//!   holds it). `NONE` gets none, as do the creation forms with no reflection out-parameter, which
//!   is Metal's `reflection:nil`.
//! - **Binary archives.** `binary_archives` is the descriptor's `binaryArchives` (the compile
//!   layer's descriptors are plain data, so the list rides here): where the creation looks
//!   before compiling, and, under `FAIL_ON_BINARY_ARCHIVE_MISS`, where it must find its
//!   pipeline (`binary_archive`).
//! - **The budget.** A creation compiles under the standard 20-second ceiling per attempt; a
//!   deadline can only make it earlier, and a cancel token ends it at its next poll point.
//! - **Placement.** `placement` is the descriptor's `resourceIndex` (Apple's private property, a
//!   runtime extension, as a texture's placement is): where a compute or render state made with
//!   `supportIndirectCommandBuffers` goes in the pipeline states' space of IDs.

use crate::resource_ids::IdPlacement;
use crate::BinaryArchive;
use metal2vulkan::{Budget, CancelToken};
use std::time::Instant;

/// `MTLPipelineOption`: Metal's raw value, every bit kept. Combine with `|`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct PipelineOption(u64);

impl PipelineOption {
    /// `MTLPipelineOptionNone`.
    pub const NONE: PipelineOption = PipelineOption(0);
    /// `MTLPipelineOptionBindingInfo` (and the deprecated `MTLPipelineOptionArgumentInfo`):
    /// the `_with_reflection` creation forms return the pipeline's reflection.
    pub const BINDING_INFO: PipelineOption = PipelineOption(1 << 0);
    /// `MTLPipelineOptionBufferTypeInfo`: the same as `BINDING_INFO` (Metal's reflection carries
    /// the buffers' types under either).
    pub const BUFFER_TYPE_INFO: PipelineOption = PipelineOption(1 << 1);
    /// `MTLPipelineOptionFailOnBinaryArchiveMiss`: a creation that lists a binary archive fails
    /// unless an archive holds its pipeline.
    pub const FAIL_ON_BINARY_ARCHIVE_MISS: PipelineOption = PipelineOption(1 << 2);

    /// The option of Metal's raw value `raw`, undefined bits included.
    pub const fn from_raw(raw: u64) -> PipelineOption {
        PipelineOption(raw)
    }

    /// Metal's raw value.
    pub const fn raw(self) -> u64 {
        self.0
    }

    /// Whether every bit of `other` is set.
    pub const fn contains(self, other: PipelineOption) -> bool {
        self.0 & other.0 == other.0
    }

    /// Whether the options ask for reflection: **any** word but `NONE`, which is the whole of the
    /// test Metal makes.
    ///
    /// Not `BINDING_INFO | BUFFER_TYPE_INFO`, which is what Apple's documentation for the third
    /// bit would suggest, and not the three named bits either. Measured on an M3 Max under macOS
    /// 27.0.1 by the conformance suite's `pipeline_option_reflection` and `pipeline_options`
    /// families, over all eight combinations of the named bits and over four words of undefined
    /// bits (`1 << 3`, `1 << 31`, `1 << 40` and `BINDING_INFO | 1 << 3`), on the compute and the
    /// render creation alike:
    /// `newComputePipelineStateWithFunction:options:reflection:error:` fills its `reflection`
    /// out-parameter for **every** non-zero value and leaves it nil only for
    /// `MTLPipelineOptionNone` -- `MTLPipelineOptionFailOnBinaryArchiveMiss` alone included, and
    /// a bit the header gives no meaning to included. Metal tests the word against zero and
    /// nothing finer. A creation that answered nil here would refuse a consumer the reflection
    /// Metal hands it.
    pub(crate) const fn reflects(self) -> bool {
        self.0 != PipelineOption::NONE.0
    }
}

impl std::ops::BitOr for PipelineOption {
    type Output = PipelineOption;

    fn bitor(self, other: PipelineOption) -> PipelineOption {
        PipelineOption(self.0 | other.0)
    }
}

/// What a pipeline state's creation takes beside its function or descriptor: Metal's options,
/// the descriptor's binary archives, resource index and label, and the compile's deadline and
/// cancel token. `Default` is Metal's `MTLPipelineOptionNone`, no archive, an ID the runtime
/// picks, no label, under the standard budget.
#[non_exhaustive]
#[derive(Clone, Debug, Default)]
pub struct PipelineOptions {
    /// Metal's options.
    pub options: PipelineOption,
    /// `binaryArchives`: the archives the creation looks in first, in order. An archive of
    /// another device is refused (`Invalid(ForeignObject)`).
    pub binary_archives: Vec<BinaryArchive>,
    /// The token that cancels the creation's compiles (`Error::Compile` holding
    /// `error::Error::Cancelled`); `None` cannot be cancelled.
    pub cancel: Option<CancelToken>,
    /// The instant the creation's compiles must finish by (`Error::Compile` holding
    /// `error::Error::Budget` past it). Only a deadline earlier than the standard 20 seconds from
    /// each compile's start changes anything.
    pub deadline: Option<Instant>,
    /// `resourceIndex` (Apple's private descriptor property, with or without
    /// `forceResourceIndex`: Metal places the state either way): the ID a compute or render
    /// state made with `supportIndirectCommandBuffers` takes, as `gpu_resource_id` then answers,
    /// keeping the index's low 32 bits as Metal does; `At(ResourceId(0))`, or an index whose low
    /// 32 bits are 0, is plain allocation. A state without that support, and a tile state, take
    /// no ID and answer 0, as Metal's do. Refused `Invalid(ResourceId)` at 16384 or more, which
    /// ends Metal's process (measured on an M4).
    pub placement: IdPlacement,
    /// `label`: the descriptor's label, which the state answers as its `label` (`None` is nil,
    /// `Some("")` an empty one, as Metal keeps them). The state keeps the label it was made with.
    /// It is not part of what the state compiles.
    pub label: Option<String>,
}

impl PipelineOptions {
    /// The options `options`, under the standard budget.
    pub fn new(options: PipelineOption) -> PipelineOptions {
        PipelineOptions {
            options,
            ..PipelineOptions::default()
        }
    }

    /// The budget of one compile attempt starting now: the standard 20 seconds, or the
    /// deadline if earlier, and the cancel token.
    pub(crate) fn budget(&self) -> Budget {
        let standard = Budget::standard().deadline;
        let deadline = match (standard, self.deadline) {
            (Some(standard), Some(deadline)) => Some(standard.min(deadline)),
            (standard, deadline) => standard.or(deadline),
        };
        let mut budget = Budget::with_deadline(deadline);
        budget.cancel = self.cancel.clone();
        budget
    }
}
