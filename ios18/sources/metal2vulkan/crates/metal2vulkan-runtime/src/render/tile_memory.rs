//! A render pass's tile threadgroup memory: `setThreadgroupMemoryLength:offset:atIndex:` and the
//! buffer the pass keeps every tile's memory in, which its tile functions' and fragment functions'
//! threadgroup arguments reach.
//!
//! Measured on an M4 and an M3 Max (identical answers):
//!
//! - **One allocation per tile** of the pass's `threadgroupMemoryLength` bytes, which persists
//!   across the pass's tile dispatches: a dispatch reads what an earlier one over the same tile
//!   left. It starts at zero with each pass, and nothing carries from one pass to the next.
//! - **One table for every stage**: an argument set at `offset` has its word 0 at that byte of the
//!   tile's memory, so arguments placed at overlapping offsets alias (Metal's validation says
//!   "there is an overlap"; Metal runs it as written).
//! - **The setter's checks** are Metal's API validation's, not plain Metal's: an index below 31, a
//!   length and an offset that are multiples of 16, and an argument that ends inside the pass's
//!   `threadgroupMemoryLength`. Only the index is refused here (`IndexOutOfRange`, which Metal
//!   itself raises); plain Metal takes every length and every offset, so the setter does too
//!   ([`RenderCommandEncoder::set_threadgroup_memory_length`] carries the measurement and the
//!   layer's own words), and a dispatch of a function whose argument the encoder never set is the
//!   binder's `MissingThreadgroupMemory`, where Metal's validation stops the dispatch.
//!
//! Measured on an M4 for a fragment function's threadgroup arguments: they
//! are the same memory of the fragment's tile, which every fragment of the tile shares, a tile
//! dispatch reads what the draws before it wrote, the draws after it read what it wrote, and a draw
//! reads what the draws before it wrote (the later of two draws' stores is the one that stays).
//! Metal's validation also stops a setter call after a draw that read the memory until a tile
//! dispatch ("A dispatch is required to reinterpret threadgroup memory"), and a draw while two
//! arguments the encoder set overlap ("there is an overlap with the threadgroupMemoryArgument");
//! without validation Metal runs both (the probe did not read what they leave), and the runtime
//! runs them as written: each draw and dispatch reads its arguments where the encoder placed them
//! last.
//!
//! **What runs.** The binder places each tile's memory in one buffer of every tile of the render
//! area (`WriteSource::TileMemory`), which a tile function's entry copies its tile's words from
//! and back to (`lower::access::tile`), and a fragment function reads and writes at its tile
//! (`lower::access::tile_memory`). A draw that reaches it states the pass's tile and render area
//! in its bind state, as a dispatch does, so both place every tile alike. The encoder makes the
//! buffer at the pass's first draw or dispatch that reads it, device-local and untracked (the
//! pass's own), and all zero as every new buffer is: Metal's start, measured. No other pass reads
//! it, so nothing carries from one pass to the next. The pass orders its accesses itself: its
//! tile dispatches record between barriers from everything before and to everything after
//! (`render/tile_dispatch.rs`), and a draw that reaches it after another draw of the current
//! render pass instance did records into a new instance after such a barrier (`render.rs`'s
//! instances, the `render-pass-split` notice), as Vulkan orders one draw's storage writes before
//! another's reads inside an instance only pixel by pixel. An execution of an indirect command
//! buffer's commands takes the arguments the encoder set at the call and the pass's buffer, and
//! orders its commands that reach the memory the same way, each in a render pass instance of its
//! own (`render/executions.rs`). The binder refuses a buffer past the device's
//! `maxStorageBufferRange` (`Unsupported`, `TileShading`), and a draw that reaches it in a layered
//! pass or amplified into more than one view (`NotImplemented(TileShading)`).

use super::RenderCall;
use super::RenderCommandEncoder;
use super::RenderLowering;
use crate::argument_table::{check_index, BUFFER_SLOTS};
use crate::buffer::Buffer;
use crate::error::{internal, Error};
use crate::values::{CpuCacheMode, HazardTrackingMode, ResourceOptions, StorageMode};
use crate::Device;
use metal2vulkan::bind::{TileThreadgroupArgument, TileThreadgroupMemory};
use std::collections::BTreeMap;

/// The encoder's tile threadgroup arguments and the pass's buffer of every tile's memory, once a
/// dispatch read it.
#[derive(Default)]
pub(super) struct TileMemory {
    arguments: BTreeMap<u32, TileThreadgroupArgument>,
    buffer: Option<Buffer>,
}

impl TileMemory {
    /// The bind state's tile threadgroup memory of a pass whose `threadgroupMemoryLength` is
    /// `length`.
    pub(super) fn bind_state(&self, length: u64) -> TileThreadgroupMemory {
        let mut memory = TileThreadgroupMemory::new(u32::try_from(length).unwrap_or(u32::MAX));
        memory.arguments = self.arguments.clone();
        memory
    }

    /// The encoder's arguments as they stand, which an execution's draws take
    /// (`render/executions.rs`), over no buffer yet.
    pub(super) fn arguments(&self) -> TileMemory {
        TileMemory {
            arguments: self.arguments.clone(),
            buffer: None,
        }
    }

    /// Whether the encoder set an argument: a draw reaches the memory only then, as one that
    /// reads an argument the encoder did not set is refused (`MissingThreadgroupMemory`).
    pub(super) fn set(&self) -> bool {
        !self.arguments.is_empty()
    }

    /// These arguments over the pass's `buffer` (`None`: no draw or dispatch made it yet).
    pub(super) fn with_buffer(self, buffer: Option<Buffer>) -> TileMemory {
        TileMemory { buffer, ..self }
    }

    /// The pass's buffer, once a draw or dispatch made it.
    pub(super) fn into_buffer(self) -> Option<Buffer> {
        self.buffer
    }

    /// The pass's buffer of every tile's memory, of `bytes` bytes (the binder's
    /// `WriteSource::TileMemory`, the same for every draw and dispatch of the pass), made on
    /// `device`, all zero, at the first draw or dispatch that reads it.
    pub(super) fn buffer(&mut self, device: &Device, bytes: u64) -> Result<Buffer, Error> {
        if let Some(buffer) = &self.buffer {
            return match buffer.length() >= bytes {
                true => Ok(buffer.clone()),
                false => Err(internal("a pass's tile memory of two sizes")),
            };
        }
        let options = ResourceOptions::new(
            StorageMode::Private,
            CpuCacheMode::DefaultCache,
            HazardTrackingMode::Untracked,
        );
        let buffer = device.own_buffer(bytes, options)?;
        self.buffer = Some(buffer.clone());
        Ok(buffer)
    }
}

impl RenderLowering {
    /// The lowering of [`RenderCommandEncoder::set_threadgroup_memory_length`].
    pub(crate) fn set_threadgroup_memory_length(
        &mut self,
        length: u64,
        offset: u64,
        index: u32,
    ) -> Result<(), Error> {
        self.log(|_| RenderCall::ThreadgroupMemoryLength(length, offset, index));
        const CALL: &str = "set_threadgroup_memory_length";
        check_index(CALL, index, BUFFER_SLOTS)?;
        // A length takes no part in the layout and bounds no access, as plain Metal's does not, so
        // any length is planned; it saturates at `u32::MAX`, which no access reads. Every offset is
        // planned too, truncated to 32 bits, which is what plain Metal does with it: byte 2^32
        // begins where byte 0 does and byte 2^32 + 4 where byte 4 does, measured. The binder then
        // places it where plain Metal begins the argument (`bind::plan`).
        let argument = TileThreadgroupArgument::new(
            u32::try_from(length).unwrap_or(u32::MAX),
            (offset % (u64::from(u32::MAX) + 1)) as u32,
        );
        self.tile_memory.arguments.insert(index, argument);
        Ok(())
    }
}

impl RenderCommandEncoder {
    /// `setThreadgroupMemoryLength:offset:atIndex:`: a tile or fragment function's threadgroup
    /// argument `index` is `length` bytes at byte `offset` of its tile's threadgroup memory, for
    /// the encoder's later tile dispatches and draws (the module docs).
    ///
    /// Every length and every offset is taken, as plain Metal takes them. Measured on an Apple M3
    /// Max under macOS 27.0.1, eighteen offsets each in its own process with Metal's API validation
    /// off, over a tile memory of 128 bytes a seeding dispatch had filled with values nothing else
    /// could write: the setter took every one, every pass completed with no error, and
    ///
    /// - the argument begins at its offset **floored to a multiple of 4** bytes: bytes 0, 2, 4, 8,
    ///   16, 17, 20 and 32 began at words 0, 0, 1, 2, 4, 4, 5 and 8, so the floor is to 4 and not
    ///   to 16;
    /// - an offset **at or past** the pass's `threadgroupMemoryLength` reads zero and drops its
    ///   stores (bytes 128 and 144 of that 128-byte memory read zero throughout), as an argument
    ///   that ends past it does from that end on;
    /// - the offset is **truncated to 32 bits**: bytes 2^32, 2^32 + 4, 2^32 + 16, 2^33 and 2^33 + 8
    ///   began where bytes 0, 4, 16, 0 and 8 did, and 2^32 + 128 read zero as byte 128 did;
    /// - the address arithmetic **wraps at 2^32** rather than reading zero for ever: an argument at
    ///   `u64::MAX`, byte `0xfffffffc` once truncated, read zero and then the memory's words 0, 1
    ///   and 2 as its reads carried past 2^32.
    ///
    /// Metal's API validation refuses the call where plain Metal takes it, and says so in these
    /// words: `offset(4) must be a multiple of 16 bytes.` for an offset that is not, and
    /// `offset + length(144) must be <= threadgroupMemoryLength(128).` for an argument that ends
    /// past the pass's memory. So a consumer running under the validation layer gets a message
    /// where this call runs, and under `MTL_DEBUG_LAYER_ERROR_MODE=assert` the layer ends the
    /// program there (measured: SIGABRT). Under `=nslog` the layer logs and the run continues, and
    /// every reading above was the same with the layer on as with it off.
    ///
    /// Refused (`Invalid`): an index past the table (`IndexOutOfRange`), which Metal itself raises.
    pub fn set_threadgroup_memory_length(
        &mut self,
        length: u64,
        offset: u64,
        index: u32,
    ) -> Result<(), Error> {
        match self.lowers_at_call() {
            true => self
                .at_call(|lowering| lowering.set_threadgroup_memory_length(length, offset, index)),
            false => self.defer(|_| RenderCall::ThreadgroupMemoryLength(length, offset, index)),
        }
    }
}
