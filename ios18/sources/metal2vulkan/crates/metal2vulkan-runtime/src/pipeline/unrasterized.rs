//! Metal's refusal of a render pipeline state with rasterization disabled whose vertex function
//! returns a value.
//!
//! - **What Metal refuses** (on an M4 under macOS 27.0.1). `newRenderPipelineStateWithDescriptor:`
//!   with `rasterizationEnabled` NO answers nil with an `NSError` where the vertex function's
//!   return type is not `void`: a struct of varyings, a struct of its position alone, or a bare
//!   position, with or without `supportIndirectCommandBuffers`. A `void` vertex function is made.
//!   With `supportIndirectCommandBuffers` this refusal comes first: a vertex function that returns
//!   a position read from a texture gets this error, not the indirect command buffers' one.
//! - **The answer.** [`Error::MetalError`] with [`Problem::UnrasterizedVertexReturn`] and Metal's
//!   error: code 3, its description, `userInfo` holding only `NSLocalizedDescription`, and the
//!   domain of Metal's GPU driver class, as `indirect_arguments.rs` answers it.
//! - **Tessellation too** (on an M4 under macOS 27.0.1). A post-tessellation
//!   vertex function that returns a value is refused alike, with or without a fragment function, a
//!   color attachment or `supportIndirectCommandBuffers`, and one that returns nothing is made,
//!   whatever the descriptor's tessellation state.
//! - **Where it binds.** A state that rasterizes nothing covers no sample, so it stores into no
//!   attachment: it binds in a pass of any colors it does not name and of any sample count, and
//!   its draws leave every attachment as it was. Metal's API validation accepts it, and the
//!   attachments' stored bytes and the visibility counts are those of the pass without the draw
//!   (measured on an M3 Max under macOS 27.0.1, over passes of 1 and 8 colors of mixed formats, 2
//!   and 4 samples, depth, stencil or both, a two-slice target, vertex amplification, two
//!   viewports, a memoryless attachment, imageblocks and a tile dispatch; empty draws alone).
//!   `ShapeInputs::check_pipeline` (`render.rs`) therefore refuses a state of neither colors nor
//!   rasterization in neither case, where a state that rasterizes without a fragment function is
//!   still refused a color it lacks, and a mesh render pipeline's state is refused as before (both
//!   unmeasured). A non-empty draw of such a state is unmeasured on Metal and draws nothing here.
//! - **Decided from the AIR.** Whether the vertex function returns a value is the compile layer's
//!   answer (`RenderPipelineDescriptor::vertex_returns_value`); one it refuses (a stage it already
//!   compiled, which a render descriptor's Metal form never holds) decides nothing.

use super::indirect_arguments::{CODE, DOMAIN};
use crate::error::{Error, MetalError, Problem};
use metal2vulkan::error::Error as CompileError;

/// Metal's `localizedDescription` for it.
const DESCRIPTION: &str =
    "RasterizationEnabled is false but the vertex shader's return type is not void";

/// Refused in `call` (the module docs): a state with rasterization disabled whose vertex
/// function `returns` a value, as the descriptor answers under the creation's budget.
pub(crate) fn check(call: &'static str, returns: Result<bool, CompileError>) -> Result<(), Error> {
    match returns {
        Ok(true) => Err(Error::MetalError(MetalError::new(
            call,
            Problem::UnrasterizedVertexReturn,
            DOMAIN,
            CODE,
            DESCRIPTION,
        ))),
        Ok(false) | Err(_) => Ok(()),
    }
}
