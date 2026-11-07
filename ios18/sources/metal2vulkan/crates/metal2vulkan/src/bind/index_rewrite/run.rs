//! A run of the index rewrite pass: the lists of direct indexed draws of one program, order and
//! `k'` over one index buffer, each draw's three phases ([`RewritePhase`](super::RewritePhase)) in one workgroup of one
//! dispatch for the whole run ([`index_rewrite_run_module`](super::index_rewrite_run_module)),
//! where a draw's own pass takes three dispatches between barriers. A draw joins where its walk
//! takes one group ([`IndexRewritePass::groups`]: at most
//! [`IndexRewritePass::GROUP_POSITIONS`] positions), which every strip of a few thousand indices
//! does; its list is the one its own pass writes, index for index.
//!
//! - **The words** ([`IndexRewriteRun::words`], at [`IndexRewriteModule::WORDS_BINDING`](super::IndexRewriteModule::WORDS_BINDING)): one
//!   slot of [`IndexRewriteRun::SLOT_WORDS`] words per draw, workgroup `w`'s from word
//!   `w · SLOT_WORDS`, each laid out as a pass's rewrite words through its one group's words (the
//!   parent module's table), and its word 26 the first word of the draw's region of the output.
//!   The draw's `VkDrawIndexedIndirectCommand` lies at [`IndexRewriteRun::draw_offset`].
//! - **The output** (at [`IndexRewriteModule::OUTPUT_BINDING`](super::IndexRewriteModule::OUTPUT_BINDING)): the run's own, each draw's region
//!   of its program's capacity of its count ([`RewriteProgram::capacity`]) after the regions
//!   before it; the draw draws it as the run's output's indices from its region's first.
//! - **Bindings**: the arguments as any 4 bytes (every draw is direct), the indices
//!   ([`IndexRewriteRun::indices_range`]: the bytes the consumer names, so one binding serves
//!   every draw of the run; any 4 bytes where it is `None`), and the status as a pass's.
//! - **Synchronization** (consumer): a pass's, for the one dispatch.

use super::{
    IndexRewritePass, Parameter, RewriteOrder, RewriteOutput, RewriteProgram, RewriteSource,
};
use crate::bind::index_bounds::word_range;
use crate::bind::instantiate::BufferRange;

/// A run of direct indexed draws of the index rewrite pass (the module docs).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct IndexRewriteRun {
    program: RewriteProgram,
    order: RewriteOrder,
    restart: u32,
    /// The bound index bytes, and their end.
    indices: Option<(BufferRange, u64)>,
    alignment: u64,
    max_range: u32,
    output_words: u32,
    /// The output's words the draws' regions take.
    written: u32,
    /// Each draw's slot.
    slots: Vec<[u32; IndexRewriteRun::SLOT_WORDS as usize]>,
}

/// One direct indexed draw of a run: Metal's arguments, its index offset and the end of the bytes
/// its fetch reads (in the bound buffer's offsets), and whether its indices are 32-bit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RewriteRunDraw {
    /// Metal's index count: the draw's positions.
    pub count: u32,
    /// Metal's instance count.
    pub instances: u32,
    /// Metal's base vertex.
    pub base_vertex: i32,
    /// Metal's base instance.
    pub base_instance: u32,
    /// The draw's index offset: a multiple of its index size.
    pub index_offset: u64,
    /// Where the draw's fetch stops reading bytes: a position from there on reads index 0.
    pub index_end: u64,
    /// 4-byte indices, else 2-byte ones.
    pub wide: bool,
}

impl IndexRewriteRun {
    /// Words of a draw's slot: a pass's rewrite words through one group's, rounded up to 16
    /// bytes.
    pub const SLOT_WORDS: u32 = 36;
    /// Bytes of a draw's slot.
    pub const SLOT_BYTES: u64 = 4 * Self::SLOT_WORDS as u64;
    /// The most draws of a run: Vulkan's least `maxComputeWorkGroupCount[0]`.
    pub const MAX_DRAWS: u32 = 65535;

    /// An empty run of draws of `program` in `order` restarting at `restart` (`k'`), whose
    /// indices lie in the bytes from `index_start` to `index_end` of a buffer, bound at an offset
    /// aligned to `alignment` (`minStorageBufferOffsetAlignment`) over at most `max_range` bytes
    /// (`maxStorageBufferRange`), into an output of `output_words` words. `None` where the bytes
    /// are past `max_range`.
    pub fn new(
        (program, order, restart): (RewriteProgram, RewriteOrder, u32),
        (index_start, index_end): (u64, u64),
        (alignment, max_range): (u64, u32),
        output_words: u32,
    ) -> Option<IndexRewriteRun> {
        let indices = match index_end > index_start {
            true => {
                let (range, _) = word_range(index_start, index_end, alignment, max_range)?;
                Some((range, range.offset + range.range))
            }
            false => None,
        };
        Some(IndexRewriteRun {
            program,
            order,
            restart,
            indices,
            alignment,
            max_range,
            output_words,
            written: 0,
            slots: Vec::new(),
        })
    }

    /// Add `draw` to the run: its slot. `None` where the run cannot take it, and is as before: a
    /// run of [`MAX_DRAWS`](Self::MAX_DRAWS), an index offset not a multiple of the index size,
    /// indices read outside the bound bytes, a walk of more than one group, or a list past the
    /// output.
    pub fn push(&mut self, draw: RewriteRunDraw) -> Option<u32> {
        let slot = u32::try_from(self.slots.len()).ok()?;
        if slot >= Self::MAX_DRAWS {
            return None;
        }
        let capacity = u32::try_from(self.program.capacity(u64::from(draw.count))).ok()?;
        let size = if draw.wide { 4 } else { 2 };
        let pass = IndexRewritePass::new(
            RewriteSource::Direct {
                count: draw.count,
                instances: draw.instances,
                base_vertex: draw.base_vertex,
                base_instance: draw.base_instance,
            },
            self.restart,
            (size, draw.index_offset, draw.index_end),
            RewriteOutput::Owned { capacity },
            self.alignment,
            self.max_range,
        )?
        .with_order(self.order);
        if pass.groups != 1 {
            return None;
        }
        // The draw's indices within the run's bound bytes: its byte there.
        let mut parameters = pass.parameters;
        if pass.indices_range.is_some() {
            let inside = u64::from(parameters[Parameter::Inside as usize]);
            let end = draw.index_offset.checked_add(inside.checked_mul(size)?)?;
            let (range, bound_end) = self.indices?;
            if draw.index_offset < range.offset || end > bound_end {
                return None;
            }
            parameters[Parameter::IndicesByte as usize] =
                u32::try_from(draw.index_offset - range.offset).ok()?;
        }
        let first = self.written;
        let written = first.checked_add(capacity)?;
        if written > self.output_words {
            return None;
        }
        let mut words = [0; Self::SLOT_WORDS as usize];
        words[..IndexRewritePass::PARAMETER_WORDS].copy_from_slice(&parameters);
        words[Parameter::OwnedFirst as usize] = first;
        self.slots.push(words);
        self.written = written;
        Some(slot)
    }

    /// The run's draws.
    pub fn len(&self) -> usize {
        self.slots.len()
    }

    /// Whether the run has no draw.
    pub fn is_empty(&self) -> bool {
        self.slots.is_empty()
    }

    /// The workgroups of the run's dispatch: one per draw.
    pub fn workgroups(&self) -> u32 {
        self.slots.len() as u32
    }

    /// The output's words the draws' regions take.
    pub fn written(&self) -> u32 {
        self.written
    }

    /// The range of the index buffer to bind at [`IndexRewriteModule::INDICES_BINDING`](super::IndexRewriteModule::INDICES_BINDING): the
    /// bytes [`new`](Self::new) names, aligned down, through a whole word; `None` where they are
    /// none.
    pub fn indices_range(&self) -> Option<BufferRange> {
        self.indices.map(|(range, _)| range)
    }

    /// The words to upload at [`IndexRewriteModule::WORDS_BINDING`](super::IndexRewriteModule::WORDS_BINDING), each draw's slot in turn.
    pub fn words(&self) -> Vec<u8> {
        self.slots
            .iter()
            .flatten()
            .flat_map(|w| w.to_le_bytes())
            .collect()
    }

    /// The byte of the words at which slot `slot`'s draw's `VkDrawIndexedIndirectCommand` lies.
    pub fn draw_offset(slot: u32) -> u64 {
        u64::from(slot) * Self::SLOT_BYTES + IndexRewritePass::DRAW_OFFSET
    }
}

#[cfg(test)]
mod tests;
