//! What a case asks an `MTLResourceStateCommandEncoder` to do: one program of mapping updates, its
//! commands stated in order, and what the heap and the texels said afterwards.
//!
//! `MTLResourceStateCommandEncoder` is the one implemented container the suite reached **no item of
//! at all** before this module: seven items, none exercised, so nothing in the tree held Metal to
//! its sparse mapping behaviour. The runtime's own `sparse/encoder.rs` carries paragraphs of
//! measured Metal behaviour — the tail maps whole, a region of no tiles maps nothing, a move onto a
//! mapped tile leaves it where it is — each of them a probe somebody ran once and wrote down. This
//! is the vocabulary that lets a case measure them instead.
//!
//! # The measurement
//!
//! A mapping takes memory from a sparse heap, so the heap's `usedSize` **is** the answer: it rises
//! by the tiles a map took and falls by the tiles an unmap gave back, and it does neither where
//! Metal maps nothing. It is read after every commit ([`MappingStep::Commit`]), so a program that
//! maps, unmaps and maps again records three numbers rather than one.
//!
//! Beside it, a region's **texels**: a program may write known bytes into tiles it has mapped and
//! read them back ([`MappingStep::FillRegion`], [`MappingStep::ReadRegion`]), which is what says
//! the mapping backed real memory rather than only moving a counter.
//!
//! # Why nothing here can fault
//!
//! Sparse mapping is the one shape in this round's territory where an out-of-range region could
//! fault a device, so **every region is in tiles and bounded by the texture's own tile grid**:
//!
//! - A region is stated in **tiles**, never in texels, so a case cannot state a region whose
//!   meaning depends on arithmetic it got wrong.
//! - A case states the tile grid it assumes as one of its inputs and **reads the venue's own tile
//!   size first**, aborting with a typed failure where the two disagree
//!   ([`SparseMappingProgram::tile`]). A venue whose tiles are another size records that refusal
//!   rather than being handed a region past its grid.
//! - A guard over the case's own rows checks every region against that stated grid with no device
//!   at all, so a row that grew past it fails in the test suite rather than on a GPU. A
//!   [`MappingStep::Move`] is the one step whose region may run past the grid, and only as far as
//!   the measurement did — each side's origin inside its level's tiles, no axis stating more tiles
//!   than the level holds, one image deep — because what plain Metal does with such a move is
//!   measured off the texels either side of it rather than guessed
//!   (`probes/sparse_move_past_the_bounds.m`: eight shapes, every command buffer completed with no
//!   error and no heap's `usedSize` moved).
//! - A fill or a read names a region of tiles the program **mapped** and nothing else, which a
//!   guard also holds — a move's own destination tiles included, by Metal's measured rule for
//!   which tiles a move takes. A blit over an unmapped tile reads zero on this oracle
//!   (`probes/sparse_unmapped_blit.m`), so a row stating one would record a reading that says
//!   nothing, and it stays out.
//!
//! Nothing dispatches, nothing draws, no shader runs, every wait is bounded and the heap is a few
//! mebibytes. Nothing here reads a device name, a vendor or an environment variable: a venue that
//! refuses one of these updates refuses it typed, at its own step.

use crate::case::Failure;
use crate::transcript::{Node, NodeName, Observed, Value};

/// `MTLSparseTextureMappingMode`: the two modes an update takes, and the whole of that enum.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MappingMode {
    /// `MTLSparseTextureMappingModeMap`.
    Map,
    /// `MTLSparseTextureMappingModeUnmap`.
    Unmap,
}

impl MappingMode {
    /// The raw `MTLSparseTextureMappingMode`.
    pub const fn as_raw(self) -> u64 {
        match self {
            MappingMode::Map => 0,
            MappingMode::Unmap => 1,
        }
    }

    /// The mode's name, for a site and for a case's own prose.
    pub const fn word(self) -> &'static str {
        match self {
            MappingMode::Map => "map",
            MappingMode::Unmap => "unmap",
        }
    }

    /// Both modes, which is every value `MTLSparseTextureMappingMode` has.
    pub const ALL: [MappingMode; 2] = [MappingMode::Map, MappingMode::Unmap];
}

/// A region of a texture's tile grid: an origin and a size, **in tiles**.
///
/// In tiles and not in texels, because that is the unit Metal's own
/// `updateTextureMapping:mode:region:mipLevel:slice:` takes and the unit a case can bound without
/// knowing a device's tile extent. A side converts it to the texels Metal's `MTLRegion` wants
/// through the venue's own tile size, which is the one number a case reads from the device.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TileRect {
    pub origin: [u64; 3],
    pub size: [u64; 3],
}

impl TileRect {
    /// A region of one image: `size` tiles at `origin`.
    pub const fn new(origin: [u64; 2], size: [u64; 2]) -> Self {
        Self {
            origin: [origin[0], origin[1], 0],
            size: [size[0], size[1], 1],
        }
    }

    /// The one tile at `origin`.
    pub const fn one(origin: [u64; 2]) -> Self {
        Self::new(origin, [1, 1])
    }

    /// A region of no tiles at `origin`: the shape Metal maps nothing for, which is a fact and not
    /// a nothing.
    pub const fn none(origin: [u64; 2]) -> Self {
        Self::new(origin, [0, 0])
    }

    /// The tiles the region holds.
    pub const fn tiles(&self) -> u64 {
        self.size[0] * self.size[1] * self.size[2]
    }

    /// The words a case's input fingerprint covers.
    pub fn words(&self) -> Vec<u8> {
        self.origin
            .iter()
            .chain(&self.size)
            .flat_map(|axis| axis.to_le_bytes())
            .collect()
    }
}

/// The tile grid a case assumes, in tiles a level, stated as an input and checked against the
/// venue's own `sparseTileSizeWithTextureType:pixelFormat:sampleCount:` before anything is
/// encoded.
///
/// A case cannot derive it: the tile extent is the device's answer, and a region past a level's
/// tiles is the one shape in this territory that could fault. So the case says what it assumes,
/// the side says what the device has, and a disagreement is a typed abort at a named site.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TileGrid {
    /// The texels one tile covers, on each axis.
    pub tile: [u64; 3],
    /// Bytes one texel of the program's textures takes, which a fill and a read need to size their
    /// buffer. A property of the pixel format the case chose, so the case states it.
    pub texel_bytes: u64,
}

impl TileGrid {
    /// A grid of `width` by `height` texels a tile, one image deep, of `texel_bytes` a texel.
    pub const fn new(width: u64, height: u64, texel_bytes: u64) -> Self {
        Self {
            tile: [width, height, 1],
            texel_bytes,
        }
    }

    /// The texel rectangle the tiles `region` cover: the origin and the size, both in texels.
    ///
    /// A fill and a read go through this one function on both sides, so neither can read a
    /// region's texels differently from the other.
    pub const fn texels_of(&self, region: &TileRect) -> ([u64; 3], [u64; 3]) {
        (
            [
                region.origin[0] * self.tile[0],
                region.origin[1] * self.tile[1],
                region.origin[2] * self.tile[2],
            ],
            [
                region.size[0] * self.tile[0],
                region.size[1] * self.tile[1],
                region.size[2] * self.tile[2],
            ],
        )
    }

    /// Bytes a row of the tiles `region` takes, and the bytes the whole region takes.
    pub const fn bytes_of(&self, region: &TileRect) -> (u64, u64) {
        let (_, size) = self.texels_of(region);
        let per_row = size[0] * self.texel_bytes;
        (per_row, per_row * size[1] * max_one(size[2]))
    }

    /// The tiles level `level` of a texture `width` by `height` holds, on each axis: the level's
    /// extent in texels, rounded **up** to whole tiles, as Metal's own grid rounds it.
    pub const fn tiles_at(&self, width: u64, height: u64, level: u32) -> [u64; 2] {
        let level_width = max_one(width >> level);
        let level_height = max_one(height >> level);
        [
            level_width.div_ceil(self.tile[0]),
            level_height.div_ceil(self.tile[1]),
        ]
    }

    /// The words a case's input fingerprint covers.
    pub fn words(&self) -> Vec<u8> {
        self.tile
            .iter()
            .chain(std::iter::once(&self.texel_bytes))
            .flat_map(|axis| axis.to_le_bytes())
            .collect()
    }
}

/// `n`, or 1 where it is 0: a mip level's extent never falls below one texel.
const fn max_one(n: u64) -> u64 {
    if n == 0 {
        1
    } else {
        n
    }
}

/// One command a case encodes into a sparse mapping program, in the order it lists them.
///
/// The mapping commands go into an `MTLResourceStateCommandEncoder` and the fill and the read into
/// an `MTLBlitCommandEncoder`; the side opens whichever the next command needs and ends the other,
/// because Metal takes one encoder at a time of a command buffer and the runtime refuses a second.
#[derive(Clone, Debug, PartialEq)]
pub enum MappingStep {
    /// `updateTextureMapping:mode:region:mipLevel:slice:`.
    One {
        texture: usize,
        mode: MappingMode,
        region: TileRect,
        level: u64,
        slice: u64,
    },
    /// `updateTextureMappings:mode:regions:mipLevels:slices:numRegions:`: each region with its own
    /// level and slice, in one call.
    Many {
        texture: usize,
        mode: MappingMode,
        /// The regions, each with the level and the slice it names.
        regions: Vec<(TileRect, u64, u64)>,
    },
    /// `updateTextureMapping:mode:indirectBuffer:indirectBufferOffset:`: the regions the buffer
    /// holds at `offset` — a 32-bit count, then that many `MTLMapIndirectArguments`.
    Indirect {
        texture: usize,
        mode: MappingMode,
        buffer: usize,
        offset: u64,
    },
    /// `moveTextureMappingsFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:
    /// destinationSlice:destinationLevel:destinationOrigin:`.
    ///
    /// The one step whose region may run **past** its level's own tiles: Metal clips the source
    /// region and lays the destination out as a run of `size.width` tiles to a row from the
    /// destination origin, which is measured off the texels rather than off a counter a move never
    /// moves (`probes/sparse_move_past_the_bounds.m`). A case's own guard bounds the overhang by
    /// what that measurement reached.
    Move {
        from: usize,
        from_slice: u64,
        from_level: u64,
        from_origin: [u64; 3],
        size: [u64; 3],
        to: usize,
        to_slice: u64,
        to_level: u64,
        to_origin: [u64; 3],
    },
    /// `updateFence:` of the resource state encoder.
    UpdateFence { fence: usize },
    /// `waitForFence:` of the resource state encoder.
    WaitForFence { fence: usize },
    /// `endEncoding` of whichever encoder is open, **without** committing, so the commands after
    /// it go into a second encoder of the same command buffer.
    ///
    /// A fence orders one encoder's work against another's, so a program over
    /// [`MappingStep::UpdateFence`] needs two encoders of one command buffer and this is how it
    /// says so. [`MappingStep::Commit`] is not that step: it closes the command buffer too, and
    /// the queue orders two command buffers anyway.
    NextEncoder,
    /// A blit of `value` into every byte of the tiles `region` of `level` and `slice`, out of a
    /// buffer the side makes: how a program writes known bytes into tiles it has mapped.
    ///
    /// Its region must be tiles the program mapped and has not unmapped, which the case's own
    /// guard holds.
    FillRegion {
        texture: usize,
        level: u64,
        slice: u64,
        region: TileRect,
        value: u8,
    },
    /// A blit of the tiles `region` of `level` and `slice` out to a buffer, which answers a read:
    /// the texels those tiles hold.
    ///
    /// Its region must be tiles the program mapped, as [`MappingStep::FillRegion`]'s must.
    ReadRegion {
        texture: usize,
        level: u64,
        slice: u64,
        region: TileRect,
    },
    /// `endEncoding`, `commit` and a bounded wait: the command buffer's status is recorded, every
    /// read the steps before it asked for is answered, and the heap's `usedSize` is read.
    Commit,
}

impl MappingStep {
    /// The Metal call the step makes, which is the site its answer is emitted at, so a refusal
    /// reads as the call we refused.
    pub fn call(&self) -> &'static str {
        match self {
            MappingStep::One { .. } => "updateTextureMapping",
            MappingStep::Many { .. } => "updateTextureMappings",
            MappingStep::Indirect { .. } => "updateTextureMappingIndirect",
            MappingStep::Move { .. } => "moveTextureMappingsFromTexture",
            MappingStep::UpdateFence { .. } => "updateFence",
            MappingStep::WaitForFence { .. } => "waitForFence",
            MappingStep::NextEncoder => "endEncoding",
            MappingStep::FillRegion { .. } => "copyFromBufferToTexture",
            MappingStep::ReadRegion { .. } => "copyFromTextureToBuffer",
            MappingStep::Commit => "commit",
        }
    }

    /// Whether the step answers a read.
    pub fn reads(&self) -> bool {
        matches!(self, MappingStep::ReadRegion { .. })
    }

    /// Whether the step is a mapping update, which is what the guard that bounds a program's
    /// regions reads.
    pub fn maps(&self) -> bool {
        matches!(
            self,
            MappingStep::One { .. }
                | MappingStep::Many { .. }
                | MappingStep::Indirect { .. }
                | MappingStep::Move { .. }
        )
    }

    /// The parameters the step's own call carries, for a case's input fingerprint: everything but
    /// the resources' indices, which the program itself fingerprints.
    pub fn parameters(&self) -> Vec<u8> {
        let mut words = self.call().as_bytes().to_vec();
        let axes = |words: &mut Vec<u8>, of: &[u64]| {
            for axis in of {
                words.extend(axis.to_le_bytes());
            }
        };
        match self {
            MappingStep::One {
                texture,
                mode,
                region,
                level,
                slice,
            } => {
                words.extend((*texture as u64).to_le_bytes());
                words.extend(mode.as_raw().to_le_bytes());
                words.extend(region.words());
                axes(&mut words, &[*level, *slice]);
            }
            MappingStep::Many {
                texture,
                mode,
                regions,
            } => {
                words.extend((*texture as u64).to_le_bytes());
                words.extend(mode.as_raw().to_le_bytes());
                words.extend((regions.len() as u64).to_le_bytes());
                for (region, level, slice) in regions {
                    words.extend(region.words());
                    axes(&mut words, &[*level, *slice]);
                }
            }
            MappingStep::Indirect {
                texture,
                mode,
                buffer,
                offset,
            } => {
                words.extend((*texture as u64).to_le_bytes());
                words.extend(mode.as_raw().to_le_bytes());
                words.extend((*buffer as u64).to_le_bytes());
                words.extend(offset.to_le_bytes());
            }
            MappingStep::Move {
                from,
                from_slice,
                from_level,
                from_origin,
                size,
                to,
                to_slice,
                to_level,
                to_origin,
            } => {
                axes(
                    &mut words,
                    &[
                        *from as u64,
                        *from_slice,
                        *from_level,
                        *to as u64,
                        *to_slice,
                        *to_level,
                    ],
                );
                axes(&mut words, from_origin);
                axes(&mut words, size);
                axes(&mut words, to_origin);
            }
            MappingStep::UpdateFence { fence } | MappingStep::WaitForFence { fence } => {
                words.extend((*fence as u64).to_le_bytes());
            }
            MappingStep::NextEncoder | MappingStep::Commit => {}
            MappingStep::FillRegion {
                texture,
                level,
                slice,
                region,
                value,
            } => {
                words.extend((*texture as u64).to_le_bytes());
                axes(&mut words, &[*level, *slice]);
                words.extend(region.words());
                words.push(*value);
            }
            MappingStep::ReadRegion {
                texture,
                level,
                slice,
                region,
            } => {
                words.extend((*texture as u64).to_le_bytes());
                axes(&mut words, &[*level, *slice]);
                words.extend(region.words());
            }
        }
        words
    }
}

/// One sparse mapping program a case runs: the heap, the textures over it, the buffers an indirect
/// update reads, the fences, the tile grid the regions are stated in, and the commands.
#[derive(Clone, Debug, PartialEq)]
pub struct SparseMappingProgram {
    /// The sparse heap every texture of the program is made on.
    pub heap: crate::sparse_query::SparseHeapSpec,
    /// The sparse textures the program makes, in the order its steps name them.
    pub textures: Vec<crate::sparse_query::SparseTextureSpec>,
    /// The buffers the program makes, by their bytes: what an indirect update reads.
    pub buffers: Vec<Vec<u8>>,
    /// How many fences the program makes.
    pub fences: usize,
    /// The tile grid the program's regions are stated in, checked against the venue's own tile
    /// size before anything is encoded.
    pub tile: TileGrid,
    /// The commands, in order.
    pub steps: Vec<MappingStep>,
}

impl SparseMappingProgram {
    /// The words a case's input fingerprint covers: the heap, every texture, every buffer, the
    /// fence count, the tile grid and every step's own parameters, so a program whose commands or
    /// whose order moved is a change to the case's inputs.
    pub fn words(&self) -> Vec<u8> {
        let mut words = self.heap.words();
        for texture in &self.textures {
            words.extend(texture.words());
        }
        for bytes in &self.buffers {
            words.extend((bytes.len() as u64).to_le_bytes());
            words.extend(bytes);
        }
        words.extend((self.fences as u64).to_le_bytes());
        words.extend(self.tile.words());
        for step in &self.steps {
            words.extend(step.parameters());
        }
        words
    }

    /// How many reads the program asks for.
    pub fn reads(&self) -> usize {
        self.steps.iter().filter(|step| step.reads()).count()
    }

    /// How many commits the program makes, which is how many `usedSize` readings it records.
    pub fn commits(&self) -> usize {
        self.steps
            .iter()
            .filter(|step| matches!(step, MappingStep::Commit))
            .count()
    }
}

/// What one sparse mapping program came to.
#[derive(Clone, Debug, PartialEq)]
pub struct SparseMapped {
    /// What each step answered, in the order the program gave them: `None` where the call
    /// answered, the typed error where it did not. Metal's encoder methods answer nothing, so its
    /// leg answers `None` throughout and a refusal of ours reads as a refusal rather than as a
    /// shape mismatch.
    pub refused: Vec<Option<Failure>>,
    /// The heap's `usedSize`, read once with the textures made and nothing encoded, then after
    /// every commit in commit order — the measurement a mapping case is about.
    ///
    /// The first reading is the baseline: a sparse heap holds no memory until a mapping takes a
    /// tile, so a program that maps nothing records the baseline twice and a program that maps one
    /// tile records the tile.
    pub used_size: Vec<u64>,
    /// Each texture's `firstMipmapInTail` and `tailSizeInBytes`, which say where the program's
    /// levels stand relative to the tail.
    pub tails: Vec<(u64, u64)>,
    /// Each command buffer's status, in commit order.
    pub statuses: Vec<Value>,
    /// The reads the program asked for, in the order its steps asked.
    pub reads: Vec<Vec<u8>>,
}

impl SparseMapped {
    /// The program's answer as the sites a case emits under one node, so both sides emit one
    /// shape.
    ///
    /// Each step's site is named by its **position and its call**, so two steps that make the same
    /// call are two sites rather than one name twice.
    pub fn node(&self, name: impl Into<NodeName>, steps: &[MappingStep]) -> Node {
        let mut node = Node::branch(name, Vec::new());
        for (at, size) in self.used_size.iter().enumerate() {
            node = node.with(Node::leaf(
                format!("used-size/{at}"),
                Observed::value(Value::Uint(*size)),
            ));
        }
        for (at, (first, tail)) in self.tails.iter().enumerate() {
            node = node.with(Node::leaf(
                format!("tail/{at}/first-mipmap"),
                Observed::value(Value::Uint(*first)),
            ));
            node = node.with(Node::leaf(
                format!("tail/{at}/bytes"),
                Observed::value(Value::Uint(*tail)),
            ));
        }
        for (at, status) in self.statuses.iter().enumerate() {
            node = node.with(Node::leaf(
                format!("status/{at}"),
                Observed::value(status.clone()),
            ));
        }
        for (at, (step, refused)) in steps.iter().zip(&self.refused).enumerate() {
            node = node.with(Node::leaf(
                format!("step/{at}/{}", step.call()),
                Observed::value(match refused {
                    None => Value::ok(Value::Absent),
                    Some(failure) => failure.recorded(),
                }),
            ));
        }
        node
    }

    /// The first step the program refused, with its position, where one did.
    pub fn first_refusal(&self) -> Option<(usize, &Failure)> {
        self.refused
            .iter()
            .enumerate()
            .find_map(|(at, refused)| refused.as_ref().map(|failure| (at, failure)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeSet;
    use std::mem::discriminant;

    /// Every kind of one vocabulary, from `first` through the successor each kind names.
    ///
    /// The exhaustiveness comes from `after`: a `match` over every variant of its enum with **no
    /// wildcard arm**, so a variant added to [`MappingStep`] does not compile until it is given an
    /// arm, and the arm of the kind it is placed after hands it to this walk. The guards then read
    /// the walk rather than an array written out beside the enum, which is the thing a new variant
    /// can be added without touching.
    fn every_kind<T: std::fmt::Debug>(first: T, after: impl Fn(&T) -> Option<T>) -> Vec<T> {
        let mut kinds = vec![first];
        let mut walked = vec![discriminant(&kinds[0])];
        while let Some(next) = after(kinds.last().expect("the walk starts at its first kind")) {
            assert!(
                !walked.contains(&discriminant(&next)),
                "the walk of {} returns to {next:?}, so it is a cycle rather than each kind once",
                std::any::type_name::<T>()
            );
            walked.push(discriminant(&next));
            kinds.push(next);
        }
        kinds
    }

    /// Every step a program can hold, one of each kind, in declaration order.
    fn every_step() -> Vec<MappingStep> {
        every_kind(
            MappingStep::One {
                texture: 0,
                mode: MappingMode::Map,
                region: TileRect::one([0, 0]),
                level: 0,
                slice: 0,
            },
            step_after,
        )
    }

    /// The step that follows `step` in declaration order, or `None` after the last.
    ///
    /// One arm per variant and no wildcard: see [`every_kind`] for why that is the whole point.
    fn step_after(step: &MappingStep) -> Option<MappingStep> {
        Some(match step {
            MappingStep::One { .. } => MappingStep::Many {
                texture: 0,
                mode: MappingMode::Map,
                regions: vec![(TileRect::one([0, 0]), 0, 0)],
            },
            MappingStep::Many { .. } => MappingStep::Indirect {
                texture: 0,
                mode: MappingMode::Map,
                buffer: 0,
                offset: 0,
            },
            MappingStep::Indirect { .. } => MappingStep::Move {
                from: 0,
                from_slice: 0,
                from_level: 0,
                from_origin: [0, 0, 0],
                size: [1, 1, 1],
                to: 1,
                to_slice: 0,
                to_level: 0,
                to_origin: [1, 0, 0],
            },
            MappingStep::Move { .. } => MappingStep::UpdateFence { fence: 0 },
            MappingStep::UpdateFence { .. } => MappingStep::WaitForFence { fence: 0 },
            MappingStep::WaitForFence { .. } => MappingStep::NextEncoder,
            MappingStep::NextEncoder => MappingStep::FillRegion {
                texture: 0,
                level: 0,
                slice: 0,
                region: TileRect::one([0, 0]),
                value: 1,
            },
            MappingStep::FillRegion { .. } => MappingStep::ReadRegion {
                texture: 0,
                level: 0,
                slice: 0,
                region: TileRect::one([0, 0]),
            },
            MappingStep::ReadRegion { .. } => MappingStep::Commit,
            MappingStep::Commit => return None,
        })
    }

    /// Every step names a Metal call no other step names, so a refusal reads as the call we
    /// refused and the fingerprint keys each step by its own name.
    ///
    /// It reads the compiler-kept walk, so a variant added to [`MappingStep`] is in this check the
    /// moment it compiles.
    #[test]
    fn every_step_names_a_call_of_its_own() {
        let steps = every_step();
        let named: BTreeSet<&str> = steps.iter().map(MappingStep::call).collect();
        assert_eq!(named.len(), steps.len(), "{named:?}");
    }

    /// Two steps that make the same call hold two sites, not one name twice.
    #[test]
    fn repeated_steps_hold_sites_of_their_own() {
        let steps = vec![
            MappingStep::One {
                texture: 0,
                mode: MappingMode::Map,
                region: TileRect::one([0, 0]),
                level: 0,
                slice: 0,
            };
            4
        ];
        let mapped = SparseMapped {
            refused: vec![None; steps.len()],
            used_size: vec![0],
            tails: vec![(3, 16384)],
            statuses: vec![Value::unit_variant("Completed")],
            reads: Vec::new(),
        };
        let node = mapped.node("mapping", &steps);
        let names: BTreeSet<String> = node
            .children
            .iter()
            .map(|child| child.name.to_string())
            .collect();
        assert_eq!(
            names.len(),
            node.children.len(),
            "two of the program's sites share a name: {names:?}"
        );
    }

    /// Both modes of `MTLSparseTextureMappingMode` are named, and their raw values are Metal's.
    #[test]
    fn both_mapping_modes_are_named_by_their_raw_value() {
        assert_eq!(MappingMode::ALL.len(), 2);
        assert_eq!(MappingMode::Map.as_raw(), 0);
        assert_eq!(MappingMode::Unmap.as_raw(), 1);
        let words: BTreeSet<&str> = MappingMode::ALL.iter().map(|mode| mode.word()).collect();
        assert_eq!(words.len(), 2);
    }

    /// A program whose steps or whose order moved fingerprints apart, and the same program twice
    /// fingerprints the same.
    ///
    /// Without the first half a recording would stay the answer to a program whose regions had
    /// moved — which for this family is the difference between mapping tile 0 and mapping the
    /// whole level.
    #[test]
    fn a_program_fingerprints_by_its_steps() {
        let program = |steps: Vec<MappingStep>| SparseMappingProgram {
            heap: crate::sparse_query::SparseHeapSpec::new(1 << 20),
            textures: Vec::new(),
            buffers: Vec::new(),
            fences: 0,
            tile: TileGrid::new(64, 64, 4),
            steps,
        };
        let steps = every_step();
        assert_eq!(
            program(steps.clone()).words(),
            program(steps.clone()).words()
        );
        let mut moved = steps.clone();
        moved[0] = MappingStep::One {
            texture: 0,
            mode: MappingMode::Unmap,
            region: TileRect::one([0, 0]),
            level: 0,
            slice: 0,
        };
        assert_ne!(
            program(steps.clone()).words(),
            program(moved).words(),
            "the mapping mode does not reach the fingerprint"
        );
        let mut wider = steps.clone();
        wider[0] = MappingStep::One {
            texture: 0,
            mode: MappingMode::Map,
            region: TileRect::new([0, 0], [2, 2]),
            level: 0,
            slice: 0,
        };
        assert_ne!(
            program(steps.clone()).words(),
            program(wider).words(),
            "a region's size does not reach the fingerprint"
        );
        let mut reversed = steps.clone();
        reversed.reverse();
        assert_ne!(program(steps).words(), program(reversed).words());
    }

    /// A region of no tiles holds none, and one tile holds one: what the rows that state Metal's
    /// documented special case lean on.
    #[test]
    fn a_region_of_no_tiles_holds_none() {
        assert_eq!(TileRect::none([1, 1]).tiles(), 0);
        assert_eq!(TileRect::one([1, 1]).tiles(), 1);
        assert_eq!(TileRect::new([0, 0], [4, 3]).tiles(), 12);
    }

    /// A level's tiles are its texels rounded **up**, and a level below one tile still holds one.
    ///
    /// The rounding is what bounds every region in this family, so it is checked rather than
    /// assumed: a grid that rounded down would call a level of 1 texel a level of no tiles, and
    /// every region of it would read as out of range.
    #[test]
    fn a_levels_tiles_round_up_and_never_reach_zero() {
        let grid = TileGrid::new(64, 64, 4);
        assert_eq!(grid.tiles_at(256, 256, 0), [4, 4]);
        assert_eq!(grid.tiles_at(256, 256, 1), [2, 2]);
        assert_eq!(grid.tiles_at(256, 256, 2), [1, 1]);
        assert_eq!(grid.tiles_at(256, 256, 3), [1, 1]);
        assert_eq!(grid.tiles_at(256, 256, 8), [1, 1]);
        // A level whose extent is not a whole number of tiles still holds the tile it part-fills.
        assert_eq!(grid.tiles_at(192, 65, 0), [3, 2]);
    }

    /// Only the mapping steps say they map, and only the read step says it reads, so the guards
    /// that bound a program's regions and count its reads read every one and nothing else.
    ///
    /// It reads the compiler-kept walk, so a new step is covered the moment it compiles.
    #[test]
    fn the_steps_say_which_map_and_which_read() {
        let mapping: Vec<&'static str> = every_step()
            .iter()
            .filter(|step| step.maps())
            .map(MappingStep::call)
            .collect();
        assert_eq!(
            mapping,
            [
                "updateTextureMapping",
                "updateTextureMappings",
                "updateTextureMappingIndirect",
                "moveTextureMappingsFromTexture"
            ]
        );
        let reading: Vec<&'static str> = every_step()
            .iter()
            .filter(|step| step.reads())
            .map(MappingStep::call)
            .collect();
        assert_eq!(reading, ["copyFromTextureToBuffer"]);
    }
}
