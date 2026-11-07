//! What a **render pass descriptor** answers of itself: `MTLRenderPassDescriptor`, its three
//! attachment descriptors and its colour-attachment array, before any encoder exists.
//!
//! Every render family of the suite hands Metal a pass descriptor and compares what the pass
//! *drew*. None of them reads one back, so the descriptor's own surface — Metal's defaults, and
//! what each setter then answers — was reached by nothing. The runtime holds those defaults as
//! literals in `RenderPassDescriptor::default()` and its three attachments' `Default`
//! implementations, with "measured on an Apple M4" beside them in prose and no run in the tree
//! behind one. This is the family that holds them to Metal.
//!
//! # Depth, not one representative each
//!
//! An attachment descriptor is almost pure option space, and a suite that compares one load action
//! and one store action says nothing about the other sixteen pairs. So the shapes here **sweep**:
//!
//! - every `MTLLoadAction` against every `MTLStoreAction` **against** every
//!   `MTLStoreActionOptions` — the whole 3 × 6 × 2 product, all 36 triples, in the 18 rows of
//!   [`action_rows`], because each row states three attachments at three places of the space;
//! - every `MTLMultisampleDepthResolveFilter` against every stencil one ([`filter_rows`], all 6);
//! - the clear values at their edges: four-channel colours including NaN, the infinities and a
//!   denormal ([`CLEAR_COLORS`]); every documented depth edge ([`CLEAR_DEPTHS`]); and **every**
//!   stencil byte, which is a space small enough to be exhaustive, with the words above it
//!   ([`clear_stencils`]);
//! - the level, slice and depth-plane selections at 0, 1, the last index of an eight-slice array,
//!   the last 32-bit index and the first value past it ([`INDEX_VALUES`]);
//! - each of the pass's own eight numbers at 0, 1, its documented limit and the value either side
//!   of it ([`NUMBER_SWEEPS`]);
//! - the sample positions a program sets and takes back off ([`POSITION_ROWS`]).
//!
//! [`crate::cases::render_pass_descriptors`] says what each case is. The ratio of values compared
//! to values the type has is in that module's prose, per parameter.
//!
//! # What a sweep can catch, and what it provably cannot
//!
//! A descriptor case reads back what it has just set, so a case whose stated values are equal
//! compares nothing: a side answering one getter out of another's field would pass. Each shape
//! therefore says which crossing it catches.
//!
//! - **[`Shape::Set`]** states every field of every attachment at once, each away from its default,
//!   through [`stated_actions`] and [`stated_index`]. No two attachments share a (load, store)
//!   pair and no two of the sixty index fields hold one number, so it catches a getter crossed with
//!   another getter, and an attachment crossed with another attachment. The tests of
//!   [`crate::cases::render_pass_descriptors`] hold both properties.
//! - **[`Shape::Defaults`]** provably cannot catch either: Metal's default for `level`, `slice`,
//!   `depthPlane`, `resolveLevel`, `resolveSlice` and `resolveDepthPlane` is 0 on every attachment,
//!   so all sixty fields are one value, and the three attachments' defaults differ only in the load
//!   and store actions. It is the one shape that can say what a program gets for free, and it is
//!   the wrong shape to catch a crossing. Its test asserts the defaults are equal, so the claim is
//!   measured rather than asserted in prose.
//! - **A sweep row** gives the three attachment kinds three *different* values of the swept
//!   parameter — colour 0 takes row `k`, the depth attachment the next row and the stencil
//!   attachment the one after ([`Row::rotated`]) — so no two of them ever hold one value and a
//!   crossing between attachment kinds fails every row. Within one attachment a crossing of the
//!   load action with the store action is caught wherever the two ordinals differ, which is 15 of
//!   the 18 action rows: `(DontCare, DontCare)`, `(Load, Store)` and `(Clear, MultisampleResolve)`
//!   are the three rows whose two ordinals are 0, 1 and 2 on both sides, and those three rows
//!   cannot catch it. The index sweep sets an attachment's six index fields to one value, so it
//!   cannot catch a crossing among the six at all; that is [`Shape::Set`]'s job, and the two shapes
//!   are kept apart for it.
//! - **A narrow read** ([`Reads`]) cannot catch a crossing with a field it does not read. The
//!   stencil-clear sweep reads the clear stencil alone, so a side answering it out of the clear
//!   depth passes all 262 rows; [`Shape::Set`] reads all three clears at once, states them apart,
//!   and is what catches that.
//!
//! # What no shape here states, and why
//!
//! - **An attachment's texture and resolve texture.** A descriptor holds them, and what Metal then
//!   renders through them is every other render family's subject. The indices are the descriptor's
//!   own fields and Metal checks none of them until an encoder opens, so a shape that states a
//!   level of 2^32 on an attachment with no texture is reading the descriptor rather than the
//!   validation. Each attachment's `texture` and `resolveTexture` is still read, as whether the
//!   descriptor holds one.
//! - **A rasterization rate map and a visibility result buffer**, for the same reason from the
//!   other end: each is an object whose identity nothing here compares, and
//!   [`crate::cases::rate_maps`] and [`crate::cases::visibility`] are the families that make one.
//!   Both getters are read, as whether the descriptor holds one.
//! - **A sample position outside `[0, 1)`**, and more positions than the pass has samples: Metal
//!   documents and asserts on both, and [`crate::render::forbidden_positions`] is the suite's one
//!   guard for it, which this family's Metal side calls as the render families do.
//! - **A store action Metal's own validation stops at an encoder** —
//!   `CustomSampleDepthStore` on a colour attachment, among others — **is swept here all the
//!   same**, because the descriptor is not the encoder: no shape of this family makes a command
//!   buffer, an encoder or a resource, so nothing checks what the descriptor holds and the question
//!   is only what it answers.
//!
//! # Why every case of this family is safe
//!
//! Nothing is allocated, compiled or submitted: a case makes descriptor objects, reads them and
//! drops them. No library, no texture, no command buffer, no queue, so no shape can fault the
//! device, trip its watchdog or reach a GPU at all. Two hazards are named and avoided: the sample
//! positions above, and `objc2-metal` 0.3.2's non-null `depthAttachment`, `stencilAttachment` and
//! `objectAtIndexedSubscript:` accessors, which panic rather than answering a nil. [`Shape::Cleared`]
//! is the one shape that asks Metal for an attachment after setting nil, and it is safe because
//! Metal answers a **fresh descriptor** there rather than a nil — measured on this family's oracle,
//! one shape in one process, before the shape was committed.

use crate::transcript::{FloatWidth, Node, NodeName, Observed, Value};

/// The colour attachments a whole read covers: all eight of Metal's array, so the last valid index
/// is in every record and no bound is a case's choice. `objectAtIndexedSubscript:` past the last
/// asserts, which is why the constant is the array's length and not a number somebody picked.
pub const READ_COLORS: u32 = 8;

/// The colour attachments a sweep's row reads: the swept one and two left at Metal's default
/// beside it, so a row records what it stated next to what it did not.
pub const SWEPT_COLORS: u32 = 3;

/// The attachment a sweep's row states: colour 0, which is [`SWEPT_COLORS`]' first.
pub const SWEPT_COLOR: u32 = 0;

/// How the attachments are numbered where a stated value is derived from the attachment:
/// `0..READ_COLORS` are the colour attachments, then the depth and the stencil attachment.
pub const DEPTH_AT: u32 = READ_COLORS;

/// The stencil attachment's number ([`DEPTH_AT`]).
pub const STENCIL_AT: u32 = READ_COLORS + 1;

/// The attachments a stated table covers: the eight colour attachments, the depth and the stencil.
pub const STATED_ATTACHMENTS: u32 = READ_COLORS + 2;

/// The index fields each attachment has: `level`, `slice`, `depthPlane`, `resolveLevel`,
/// `resolveSlice` and `resolveDepthPlane`, in that order.
pub const INDEX_FIELDS: u32 = 6;

/// Which of the shapes a case states.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Shape {
    /// A fresh descriptor and nothing else: Metal's default for every field of it.
    Defaults,
    /// Every field of every attachment and every number of the pass stated, each away from its
    /// default and no two alike, read straight back.
    Set,
    /// Attachment descriptors built apart from the pass, stated, assigned into it
    /// (`setObject:atIndexedSubscript:`, `setDepthAttachment:`, `setStencilAttachment:`) and then
    /// **mutated through the object that was assigned**: what the pass answers says whether the
    /// assignment copied the descriptor or kept the caller's.
    Assigned,
    /// [`Shape::Set`], then every attachment set to nil: what a pass answers for an attachment a
    /// program has cleared.
    Cleared,
}

impl Shape {
    /// The ordinal a case's input fingerprint carries.
    pub const fn word(self) -> u8 {
        match self {
            Shape::Defaults => 0,
            Shape::Set => 1,
            Shape::Assigned => 2,
            Shape::Cleared => 3,
        }
    }

    /// Whether the shape states [`stated_actions`] and [`stated_index`] on the pass's own
    /// attachments before reading.
    pub const fn sets(self) -> bool {
        matches!(self, Shape::Set | Shape::Cleared)
    }
}

/// How much of the descriptor a case reads back.
///
/// A sweep over one parameter reads that parameter, so its recording holds its own values and a
/// row is a row rather than a copy of the whole descriptor. What a narrow read cannot catch is in
/// the module's prose.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reads {
    /// All eight colour attachments, the depth and stencil attachments, the pass's own numbers and
    /// its sample positions.
    Whole,
    /// [`SWEPT_COLORS`] colour attachments, the depth and the stencil attachment; no number of the
    /// pass itself.
    Attachments,
    /// Colour [`SWEPT_COLOR`]'s `clearColor`, by its four channels' bits.
    ClearColor,
    /// The depth attachment's `clearDepth`, by its bits.
    ClearDepth,
    /// The stencil attachment's `clearStencil`.
    ClearStencil,
    /// The pass's own numbers.
    Numbers,
    /// The pass's sample positions, and how many `getSamplePositions:count:` answers.
    Positions,
}

impl Reads {
    /// The ordinal a case's input fingerprint carries.
    pub const fn word(self) -> u8 {
        match self {
            Reads::Whole => 0,
            Reads::Attachments => 1,
            Reads::ClearColor => 2,
            Reads::ClearDepth => 3,
            Reads::ClearStencil => 4,
            Reads::Numbers => 5,
            Reads::Positions => 6,
        }
    }

    /// Whether a row is [`Row::rotated`] onto the depth and stencil attachments, or stated on all
    /// three verbatim.
    ///
    /// A read that covers more than one attachment rotates, so no two attachments hold one value
    /// of a swept parameter and a crossing between attachment kinds fails every row. A read that
    /// covers **one** field of one attachment does not: there is no second attachment in the
    /// record to cross with, and rotating would move the swept value off the attachment being
    /// read — an exhaustive sweep of the stencil clear would then compare 255 and 1 rather than
    /// 255.
    pub const fn rotates(self) -> bool {
        matches!(self, Reads::Whole | Reads::Attachments)
    }

    /// How many colour attachments the read covers.
    pub const fn colors(self) -> u32 {
        match self {
            Reads::Whole => READ_COLORS,
            Reads::Attachments => SWEPT_COLORS,
            Reads::ClearColor => 1,
            Reads::ClearDepth | Reads::ClearStencil | Reads::Numbers | Reads::Positions => 0,
        }
    }
}

/// One attachment's three action fields, by Metal's raw values.
///
/// Raw values and not this suite's enumerations: the whole of what an action means is its ordinal,
/// the runtime's own `LoadAction::from_raw` is what turns one into its value, and a sweep that
/// named each value would state a vocabulary beside the two the sides already have.
///
/// The sweeps state only the values and bits Metal **defines**. An undefined store-action option
/// bit is not swept because the runtime refuses one by design — `StoreActionOptions::from_raw`
/// answers a typed error rather than casting — so a row stating bit 1 would compare our refusal
/// against whatever Metal's setter stored, which is a refusal case and a different question from
/// this family's. What Metal does with such a bit is unmeasured.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Actions {
    /// `loadAction`: `MTLLoadAction`'s raw value, 0 to 2.
    pub load: u64,
    /// `storeAction`: `MTLStoreAction`'s raw value, 0 to 5.
    pub store: u64,
    /// `storeActionOptions`: `MTLStoreActionOptions`' bits, of which bit 0 is defined.
    pub options: u64,
}

/// How many values `MTLLoadAction` has.
pub const LOAD_ACTIONS: u64 = 3;

/// How many values `MTLStoreAction` has.
pub const STORE_ACTIONS: u64 = 6;

/// How many values `MTLStoreActionOptions` has: none and `CustomSamplePositions`.
pub const STORE_OPTIONS: u64 = 2;

/// How many values `MTLMultisampleDepthResolveFilter` has.
pub const DEPTH_FILTERS: u64 = 3;

/// How many values `MTLMultisampleStencilResolveFilter` has.
pub const STENCIL_FILTERS: u64 = 2;

/// One attachment's two resolve filters, by Metal's raw values. The depth attachment reads the
/// first and the stencil attachment the second; a colour attachment has neither.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Filters {
    /// `depthResolveFilter`'s raw value, 0 to 2.
    pub depth: u64,
    /// `stencilResolveFilter`'s raw value, 0 or 1.
    pub stencil: u64,
}

/// One attachment's six index fields, in [`INDEX_FIELDS`]' order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Indices(pub [u64; INDEX_FIELDS as usize]);

impl Indices {
    /// Every field at `value`: what an index sweep's row states.
    pub const fn all(value: u64) -> Indices {
        Indices([value; INDEX_FIELDS as usize])
    }
}

/// What one row of a sweep states on the attachment it names, over the shape's own setting.
///
/// A row states **all** of an attachment's own fields, so a row is the whole of what its case
/// feeds: a parameter left at its base is left there on purpose rather than by omission.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Row {
    pub actions: Actions,
    pub filters: Filters,
    pub indices: Indices,
    /// `clearColor`'s four channels.
    pub clear_color: [f64; 4],
    /// `clearDepth`.
    pub clear_depth: f64,
    /// `clearStencil`.
    pub clear_stencil: u32,
}

/// The row every sweep starts from: Metal's own defaults for a colour attachment, so a row that
/// states one parameter away from it differs from the defaults case in that parameter alone.
pub const BASE: Row = Row {
    actions: Actions {
        load: 0,
        store: 1,
        options: 0,
    },
    filters: Filters {
        depth: 0,
        stencil: 0,
    },
    indices: Indices::all(0),
    clear_color: [0.0, 0.0, 0.0, 1.0],
    clear_depth: 1.0,
    clear_stencil: 0,
};

impl Row {
    /// The row with `actions` stated.
    #[must_use]
    pub const fn with_actions(mut self, load: u64, store: u64, options: u64) -> Row {
        self.actions = Actions {
            load,
            store,
            options,
        };
        self
    }

    /// The row with both resolve filters stated.
    #[must_use]
    pub const fn with_filters(mut self, depth: u64, stencil: u64) -> Row {
        self.filters = Filters { depth, stencil };
        self
    }

    /// The row with every index field at `value`.
    #[must_use]
    pub const fn with_indices(mut self, value: u64) -> Row {
        self.indices = Indices::all(value);
        self
    }

    /// The row with `clearColor` stated.
    #[must_use]
    pub const fn with_clear_color(mut self, clear: [f64; 4]) -> Row {
        self.clear_color = clear;
        self
    }

    /// The row with `clearDepth` stated.
    #[must_use]
    pub const fn with_clear_depth(mut self, clear: f64) -> Row {
        self.clear_depth = clear;
        self
    }

    /// The row with `clearStencil` stated.
    #[must_use]
    pub const fn with_clear_stencil(mut self, clear: u32) -> Row {
        self.clear_stencil = clear;
        self
    }

    /// The row `shift` places on in every swept space at once: each action, each filter and each
    /// index moved by `shift` within its own range.
    ///
    /// This is what gives one row's three attachments three different values: colour
    /// [`SWEPT_COLOR`] takes the row, the depth attachment `rotated(1)` and the stencil attachment
    /// `rotated(2)`. No two of the three then hold one value of any swept parameter, so a side that
    /// answered one attachment's getter out of another's reads wrong in every row.
    #[must_use]
    pub fn rotated(self, shift: u64) -> Row {
        let Row {
            actions,
            filters,
            indices,
            clear_color,
            clear_depth,
            clear_stencil,
        } = self;
        Row {
            actions: Actions {
                load: (actions.load + shift) % LOAD_ACTIONS,
                store: (actions.store + shift) % STORE_ACTIONS,
                options: (actions.options + shift) % STORE_OPTIONS,
            },
            filters: Filters {
                depth: (filters.depth + shift) % DEPTH_FILTERS,
                stencil: (filters.stencil + shift) % STENCIL_FILTERS,
            },
            // The indices move by a stride wider than the six fields, so a rotated attachment's
            // six values never meet another attachment's.
            indices: Indices::all(indices.0[0].wrapping_add(shift.wrapping_mul(INDEX_STRIDE))),
            // A clear is rotated by the channel, not by a modulus: the values are a sweep's own
            // and the rotation only has to make the three attachments differ.
            clear_color: [
                clear_color[(shift as usize) % 4],
                clear_color[(shift as usize + 1) % 4],
                clear_color[(shift as usize + 2) % 4],
                clear_color[(shift as usize + 3) % 4],
            ],
            clear_depth: clear_depth + shift as f64,
            clear_stencil: clear_stencil.wrapping_add(shift as u32),
        }
    }

    /// The words a case's input fingerprint covers: every field the row states.
    pub fn words(&self) -> Vec<u8> {
        let mut out = Vec::new();
        for word in [
            self.actions.load,
            self.actions.store,
            self.actions.options,
            self.filters.depth,
            self.filters.stencil,
            u64::from(self.clear_stencil),
        ] {
            out.extend(word.to_le_bytes());
        }
        for index in self.indices.0 {
            out.extend(index.to_le_bytes());
        }
        for channel in self.clear_color {
            out.extend(channel.to_bits().to_le_bytes());
        }
        out.extend(self.clear_depth.to_bits().to_le_bytes());
        out
    }
}

/// How far apart two attachments' stated index fields are kept, which is wider than
/// [`INDEX_FIELDS`] so no two of them can meet.
pub const INDEX_STRIDE: u64 = 7;

/// Where the stated index fields start, above every value a default could be.
const INDEX_FLOOR: u64 = 11;

/// The value [`Shape::Set`] states for index field `field` of attachment `at`.
///
/// Distinct over every pair, because the stride is wider than the fields: field `f` of attachment
/// `a` is `11 + 7a + f`, and `f` is below 7.
pub fn stated_index(at: u32, field: u32) -> u64 {
    INDEX_FLOOR + u64::from(at) * INDEX_STRIDE + u64::from(field)
}

/// The six index fields [`Shape::Set`] states for attachment `at`.
pub fn stated_indices(at: u32) -> Indices {
    Indices(std::array::from_fn(|field| stated_index(at, field as u32)))
}

/// The actions [`Shape::Set`] states for attachment `at`.
///
/// The (load, store) pair is the `at`-th of the 18, counted so that all ten attachments hold ten
/// different pairs: the load cycles every attachment and the store every three.
pub fn stated_actions(at: u32) -> Actions {
    Actions {
        load: u64::from(at) % LOAD_ACTIONS,
        store: (u64::from(at) / LOAD_ACTIONS) % STORE_ACTIONS,
        options: u64::from(at) % STORE_OPTIONS,
    }
}

/// The colour slot [`Shape::Assigned`] assigns an attachment object into: one in the middle of
/// the array, so the slots either side of it answer Metal's defaults beside it.
pub const ASSIGNED_COLOR: u32 = 3;

/// The attachment whose stated values [`Shape::Assigned`] mutates an assigned object to **after**
/// assigning it: another attachment of the stated table, so whichever answer comes back is a value
/// the record holds elsewhere and a reader can tell which one it is.
pub const MUTATED_AFTER: u32 = 5;

/// The clear depth [`Shape::Set`] states: inside Metal's documented `[0, 1]`, away from the
/// default of 1, and equal to no other number of the stated table.
pub const STATED_CLEAR_DEPTH: f64 = 0.25;

/// The clear stencil [`Shape::Set`] states: a byte with both halves set, away from the default of
/// 0.
pub const STATED_CLEAR_STENCIL: u32 = 0xab;

/// Everything [`Shape::Set`] states on attachment `at`: its actions, its six index fields, its
/// clear colour, and the one clear and filter its kind has.
///
/// The filters and the two scalar clears are the same on every attachment, because only the depth
/// attachment reads a depth clear and a depth filter and only the stencil attachment reads theirs:
/// there is no second attachment to tell them apart from.
pub fn stated_row(at: u32) -> Row {
    Row {
        actions: stated_actions(at),
        filters: STATED_FILTERS,
        indices: stated_indices(at),
        clear_color: stated_clear_color(at),
        clear_depth: STATED_CLEAR_DEPTH,
        clear_stencil: STATED_CLEAR_STENCIL,
    }
}

/// The resolve filters [`Shape::Set`] states: the depth attachment's and the stencil
/// attachment's, each away from its default.
pub const STATED_FILTERS: Filters = Filters {
    depth: 2,
    stencil: 1,
};

/// The clear colour [`Shape::Set`] states for attachment `at`: four channels no two of which are
/// equal, and no two attachments' alike.
pub fn stated_clear_color(at: u32) -> [f64; 4] {
    let base = f64::from(at) * 16.0;
    [base + 1.0, base + 2.0, base + 4.0, base + 8.0]
}

/// The eight numbers of the pass itself, in the order [`Numbers`] holds them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Numbers(pub [u64; 8]);

/// Metal's defaults for the pass's own numbers: every one of them 0.
pub const DEFAULT_NUMBERS: Numbers = Numbers([0; 8]);

/// What [`Shape::Set`] states for the pass's own numbers: no two equal, each away from 0, and none
/// of them a value [`NUMBER_SWEEPS`] states for its own field.
///
/// The last of those is what keeps the number sweep's rows apart: every row of it holds one field
/// at a swept value and the other seven here, so a row whose swept value was its field's own
/// number here would be the stated table under a second name.
pub const STATED_NUMBERS: Numbers = Numbers([3, 128, 96, 5, 48, 24, 40, 1000]);

/// The sample positions [`Shape::Set`] states: four, which is a sample count every Apple GPU takes
/// custom positions at, each coordinate an exact sixteenth so Metal's own quantization cannot be
/// mistaken for a side's rounding, and all eight coordinates different.
pub const STATED_POSITIONS: [(f32, f32); 4] = [
    (0.0625, 0.1875),
    (0.3125, 0.4375),
    (0.5625, 0.6875),
    (0.8125, 0.9375),
];

/// What one case makes and reads.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PassSpec {
    pub shape: Shape,
    pub reads: Reads,
    /// The row this case states on colour [`SWEPT_COLOR`], the depth and the stencil attachment
    /// (each [`Row::rotated`] by its place), over whatever the shape stated. `None` leaves the
    /// shape's own setting alone.
    pub row: Option<Row>,
    /// The pass's own numbers, where the case states them over the shape's.
    pub numbers: Option<Numbers>,
    /// The sample positions, where the case states them over the shape's. Empty is a case: it is
    /// how a program takes the custom positions back off.
    pub positions: Option<&'static [(f32, f32)]>,
}

impl PassSpec {
    /// The descriptor of `shape`, read by `reads`, stating no row of its own.
    pub const fn new(shape: Shape, reads: Reads) -> PassSpec {
        PassSpec {
            shape,
            reads,
            row: None,
            numbers: None,
            positions: None,
        }
    }

    /// The same, stating `row`.
    #[must_use]
    pub const fn at(mut self, row: Row) -> PassSpec {
        self.row = Some(row);
        self
    }

    /// The same, stating the pass's own numbers.
    #[must_use]
    pub const fn with_numbers(mut self, numbers: Numbers) -> PassSpec {
        self.numbers = Some(numbers);
        self
    }

    /// The same, stating sample positions.
    #[must_use]
    pub const fn with_positions(mut self, positions: &'static [(f32, f32)]) -> PassSpec {
        self.positions = Some(positions);
        self
    }

    /// The words a case's input fingerprint covers: the shape, the read, and every value the case
    /// states, so two cases that state different values can never read one recording.
    pub fn words(&self) -> Vec<u8> {
        let mut out = vec![self.shape.word(), self.reads.word()];
        if let Some(row) = &self.row {
            out.push(1);
            out.extend(row.words());
        } else {
            out.push(0);
        }
        if let Some(numbers) = &self.numbers {
            out.push(1);
            for number in numbers.0 {
                out.extend(number.to_le_bytes());
            }
        } else {
            out.push(0);
        }
        if let Some(positions) = self.positions {
            out.push(1);
            for (x, y) in positions {
                out.extend(x.to_bits().to_le_bytes());
                out.extend(y.to_bits().to_le_bytes());
            }
        } else {
            out.push(0);
        }
        out
    }
}

/// What one attachment descriptor answers of itself, whichever of the three kinds it is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AttachmentFacts {
    /// Whether `texture` answers a texture at all; no shape of this family states one.
    pub has_texture: bool,
    /// Whether `resolveTexture` answers one.
    pub has_resolve_texture: bool,
    /// The six index fields in [`INDEX_FIELDS`]' order.
    pub indices: Indices,
    pub actions: Actions,
}

/// What a render pass descriptor answers of itself, by the read the case asked for: a field is
/// `None` where the read does not cover it, and a site a read leaves out is a site the recording
/// does not hold.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PassDescriptorFacts {
    /// `colorAttachments[i]` for `i` below [`Reads::colors`].
    pub colors: Vec<AttachmentFacts>,
    /// Each read colour attachment's `clearColor`, by its four channels' bits.
    pub color_clears: Vec<[u64; 4]>,
    /// `depthAttachment`, where the read covers it.
    pub depth: Option<AttachmentFacts>,
    /// `clearDepth`'s bits.
    pub clear_depth: Option<u64>,
    /// `depthResolveFilter`'s raw value.
    pub depth_resolve_filter: Option<u64>,
    /// `stencilAttachment`, where the read covers it.
    pub stencil: Option<AttachmentFacts>,
    /// `clearStencil`.
    pub clear_stencil: Option<u32>,
    /// `stencilResolveFilter`'s raw value.
    pub stencil_resolve_filter: Option<u64>,
    /// The pass's own eight numbers, where the read covers them.
    pub numbers: Option<Numbers>,
    /// Whether `visibilityResultBuffer` answers a buffer.
    pub has_visibility_buffer: Option<bool>,
    /// Whether `rasterizationRateMap` answers a map.
    pub has_rate_map: Option<bool>,
    /// What `getSamplePositions:count:` answers for a count of 0: how many positions the pass
    /// holds, which is the only way to ask Metal how long the array is.
    pub position_count: Option<u64>,
    /// The positions `getSamplePositions:count:` wrote, by their coordinates' bits.
    pub positions: Option<Vec<(u32, u32)>>,
}

impl PassDescriptorFacts {
    /// The facts of a read that covers nothing yet: what each side fills.
    pub fn empty() -> PassDescriptorFacts {
        PassDescriptorFacts {
            colors: Vec::new(),
            color_clears: Vec::new(),
            depth: None,
            clear_depth: None,
            depth_resolve_filter: None,
            stencil: None,
            clear_stencil: None,
            stencil_resolve_filter: None,
            numbers: None,
            has_visibility_buffer: None,
            has_rate_map: None,
            position_count: None,
            positions: None,
        }
    }
}

/// Every row of the action sweep: the 18 (load, store) pairs in order.
///
/// Each row states colour [`SWEPT_COLOR`] at the pair with no options, the depth attachment one
/// place on in all three spaces — which is the next load, the next store **and** the
/// `CustomSamplePositions` option — and the stencil attachment two places on. So the 18 rows hold
/// all 18 pairs at options none (the colour attachment's), all 18 at
/// `CustomSamplePositions` (the depth attachment's), and the whole 3 × 6 × 2 product between them.
pub fn action_rows() -> Vec<Row> {
    (0..LOAD_ACTIONS * STORE_ACTIONS)
        .map(|at| BASE.with_actions(at / STORE_ACTIONS, at % STORE_ACTIONS, 0))
        .collect()
}

/// Every row of the resolve-filter sweep: all 3 × 2 pairs of the two filters.
///
/// The depth attachment reads the depth filter and the stencil attachment the stencil one, so a
/// row's rotation moves the depth filter by one and leaves the stencil filter where it is (two
/// places on in a space of two is where it started). Over the six rows the depth attachment
/// therefore holds each of its three values and the stencil attachment each of its two.
pub fn filter_rows() -> Vec<Row> {
    (0..DEPTH_FILTERS * STENCIL_FILTERS)
        .map(|at| BASE.with_filters(at / STENCIL_FILTERS, at % STENCIL_FILTERS))
        .collect()
}

/// Every clear colour the sweep states: Metal's default, the two unit corners, a channel-ordered
/// ramp, and the floats a converter is most likely to mishandle — a quiet NaN with a payload, both
/// infinities, a denormal, and the largest and smallest doubles.
///
/// No two channels of a row are equal, so a side that answered one channel out of another's reads
/// wrong.
pub const CLEAR_COLORS: [[f64; 4]; 6] = [
    [0.0, 0.0, 0.0, 1.0],
    [1.0, 1.0, 1.0, 1.0],
    [0.0, 0.0, 0.0, 0.0],
    [0.25, 0.5, 0.75, -1.0],
    [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, -0.0],
    [f64::MAX, f64::MIN, f64::MIN_POSITIVE, f64::EPSILON],
];

/// Every clear depth the sweep states: both ends of Metal's documented `[0, 1]`, the middle, a
/// value either side of each end, both zeros, a NaN of each sign, both infinities, a denormal and
/// the double just below 1.
pub const CLEAR_DEPTHS: [f64; 13] = [
    0.0,
    1.0,
    0.5,
    -0.0,
    2.0,
    -1.0,
    f64::NAN,
    -f64::NAN,
    f64::INFINITY,
    f64::NEG_INFINITY,
    f64::MIN_POSITIVE,
    f64::EPSILON,
    0.999_999_999_999_999_9,
];

/// Every clear stencil the sweep states: **every** byte, which is a space small enough to be
/// exhaustive, then the first words above a byte, the 16-bit edge and the last two words of the
/// `uint32_t` the property is.
///
/// What this measures is the **store**: whether the descriptor keeps the word a program gave it,
/// masked or not. The masking Metal does of a clear is `buffer_target_clears`' subject, where a
/// pass runs.
pub fn clear_stencils() -> Vec<u32> {
    (0..=255u32)
        .chain([256, 257, 0xffff, 0x1_0000, 0xffff_fffe, 0xffff_ffff])
        .collect()
}

/// Every value the index sweep states on all six of an attachment's index fields: 0, the first two
/// above it, the last and first index of an eight-slice array, the last 32-bit index, the first
/// value past 32 bits, and the last value a `NSUInteger` holds.
///
/// The two above 32 bits are the point: `NSUInteger` is 64 bits on this platform and the runtime's
/// fields are `u64`, so a side that kept an index in a `u32` answers a different number.
pub const INDEX_VALUES: [u64; 8] = [0, 1, 2, 7, 8, 0xffff_ffff, 0x1_0000_0000, u64::MAX];

/// Each of the pass's own numbers, and the values the sweep states it at: 0, 1, the limit Metal
/// documents or the runtime measured, and the value either side of that limit.
///
/// The limits are all **encoder-time** checks — a render encoder refuses a tile of 8 × 8, a target
/// side past 16384, per-sample storage past 64 bytes and threadgroup memory past 32768 — so what a
/// row past one of them measures is that the descriptor stores it anyway, which is the only claim a
/// descriptor can make about it.
pub const NUMBER_SWEEPS: [(usize, &[u64]); 8] = [
    // `renderTargetArrayLength`: 0 and 1 render the slice alone, 2 and up are a layered pass, and
    // 2048 is Metal's largest array length.
    (0, &[0, 1, 2, 2048, 2049, 0xffff_ffff, 0x1_0000_0000]),
    // `renderTargetWidth` and `renderTargetHeight`: 16384 is the largest side.
    (1, &[0, 1, 16384, 16385]),
    (2, &[0, 1, 16384, 16385]),
    // `defaultRasterSampleCount`: 1, 2 and 4 on every device; 0 ends Metal's process at an encoder
    // of a pass without attachments, and 8 is a count this device has no attachments for.
    (3, &[0, 1, 2, 4, 8]),
    // `tileWidth` and `tileHeight`: 16 and 32 are the two sides a tile dispatch takes.
    (4, &[0, 16, 32, 33]),
    (5, &[0, 16, 32, 33]),
    // `imageblockSampleLength`: per-sample storage is this rounded up to 8 bytes, at most 64.
    (6, &[0, 1, 64, 65]),
    // `threadgroupMemoryLength`: at most 32768 bytes.
    (7, &[0, 1, 32768, 32769]),
];

/// Every sample-position row the sweep states: none at all, which is how a program takes custom
/// positions back off; the four exact sixteenths [`STATED_POSITIONS`] holds; two positions, which
/// is the other count an Apple GPU takes; and four coordinates that are **not** sixteenths, which
/// is what says whether `getSamplePositions:count:` answers the value a program gave or the one the
/// rasterizer would use.
///
/// Every coordinate is inside the `[0, 1)` Metal asserts on leaving, and no row holds more
/// positions than the sample count it names: [`crate::render::forbidden_positions`] is the guard,
/// and the Metal side calls it before the setter as every render family does.
pub const POSITION_ROWS: [&[(f32, f32)]; 4] = [
    &[],
    &STATED_POSITIONS,
    &[(0.0, 0.5), (0.9375, 0.0625)],
    &[(0.1, 0.2), (0.3, 0.7), (0.55, 0.45), (0.999, 0.001)],
];

/// A `double` as a site records it: by its bits, so a NaN's payload and a signed zero compare
/// exactly.
fn double(bits: u64) -> Value {
    Value::Float {
        bits,
        width: FloatWidth::Double,
    }
}

/// A `float` as a site records it, for the same reason.
fn single(bits: u32) -> Value {
    Value::Float {
        bits: u64::from(bits),
        width: FloatWidth::Single,
    }
}

impl AttachmentFacts {
    /// One attachment's answers as a site records them.
    fn value(&self) -> Value {
        Value::fields([
            ("has_texture", Value::Bool(self.has_texture)),
            ("has_resolve_texture", Value::Bool(self.has_resolve_texture)),
            ("level", Value::Uint(self.indices.0[0])),
            ("slice", Value::Uint(self.indices.0[1])),
            ("depth_plane", Value::Uint(self.indices.0[2])),
            ("resolve_level", Value::Uint(self.indices.0[3])),
            ("resolve_slice", Value::Uint(self.indices.0[4])),
            ("resolve_depth_plane", Value::Uint(self.indices.0[5])),
            ("load_action", Value::Uint(self.actions.load)),
            ("store_action", Value::Uint(self.actions.store)),
            ("store_action_options", Value::Uint(self.actions.options)),
        ])
    }
}

/// The names the pass's own numbers are recorded under, in [`Numbers`]' order.
const NUMBER_NAMES: [&str; 8] = [
    "render_target_array_length",
    "render_target_width",
    "render_target_height",
    "default_raster_sample_count",
    "tile_width",
    "tile_height",
    "imageblock_sample_length",
    "threadgroup_memory_length",
];

impl PassDescriptorFacts {
    /// The facts as the sites a case emits under one node, so both sides emit one shape.
    pub fn node(&self, name: impl Into<NodeName>) -> Node {
        let value = Observed::value;
        let mut node = Node::branch(name, Vec::new());
        if !self.colors.is_empty() {
            node = node.with(Node::leaf(
                "color_attachments",
                value(Value::List(
                    self.colors.iter().map(AttachmentFacts::value).collect(),
                )),
            ));
        }
        if !self.color_clears.is_empty() {
            node = node.with(Node::leaf(
                "color_clears",
                value(Value::List(
                    self.color_clears
                        .iter()
                        .map(|clear| Value::List(clear.iter().map(|bits| double(*bits)).collect()))
                        .collect(),
                )),
            ));
        }
        if let Some(depth) = &self.depth {
            node = node.with(Node::leaf("depth_attachment", value(depth.value())));
        }
        if let Some(bits) = self.clear_depth {
            node = node.with(Node::leaf("clear_depth", value(double(bits))));
        }
        if let Some(raw) = self.depth_resolve_filter {
            node = node.with(Node::leaf("depth_resolve_filter", value(Value::Uint(raw))));
        }
        if let Some(stencil) = &self.stencil {
            node = node.with(Node::leaf("stencil_attachment", value(stencil.value())));
        }
        if let Some(clear) = self.clear_stencil {
            node = node.with(Node::leaf(
                "clear_stencil",
                value(Value::Uint(u64::from(clear))),
            ));
        }
        if let Some(raw) = self.stencil_resolve_filter {
            node = node.with(Node::leaf(
                "stencil_resolve_filter",
                value(Value::Uint(raw)),
            ));
        }
        if let Some(numbers) = &self.numbers {
            for (name, number) in NUMBER_NAMES.iter().zip(numbers.0) {
                node = node.with(Node::leaf(*name, value(Value::Uint(number))));
            }
        }
        if let Some(held) = self.has_visibility_buffer {
            node = node.with(Node::leaf(
                "has_visibility_result_buffer",
                value(Value::Bool(held)),
            ));
        }
        if let Some(held) = self.has_rate_map {
            node = node.with(Node::leaf(
                "has_rasterization_rate_map",
                value(Value::Bool(held)),
            ));
        }
        if let Some(count) = self.position_count {
            node = node.with(Node::leaf(
                "sample_position_count",
                value(Value::Uint(count)),
            ));
        }
        if let Some(positions) = &self.positions {
            node = node.with(Node::leaf(
                "sample_positions",
                value(Value::List(
                    positions
                        .iter()
                        .map(|(x, y)| Value::List(vec![single(*x), single(*y)]))
                        .collect(),
                )),
            ));
        }
        node
    }
}
