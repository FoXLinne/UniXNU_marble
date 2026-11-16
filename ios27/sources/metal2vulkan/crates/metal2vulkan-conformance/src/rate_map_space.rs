//! The rasterization-rate space the suite sweeps, and why each point of it is a point.
//!
//! A rate map is almost entirely **numbers**: a screen extent, a layer count, and per layer a
//! quality sample on each axis. Nothing a program sets is read back by the GPU directly — every
//! observable answer is **derived** by the compile, which is what makes this family worth
//! sweeping and what makes the obvious case worthless. A case that sets a quality sample and reads
//! it back passes whether or not Metal did anything with it, and sweeping that comparison compares
//! nothing thirty times instead of once. So every site this module feeds is something the map
//! **derives**: a physical extent, a screen-to-physical or physical-to-screen mapping, the
//! granularity, the parameter buffer's size and alignment, or the parameter data itself.
//!
//! # What the compile does to a quality sample, and where its steps are
//!
//! Metal leaves the compiled map to the device: a layer's samples "may be rounded up to the
//! nearest supported value or granularity". Apple M3's rule is written down and measured in
//! `metal2vulkan::bind`'s rate-map module, and the two parts of it that place every probe point here
//! are:
//!
//! - each sample `q` becomes `Q = max(min(q + 1/256, 1), 0.01)` — a **bias** then **two clamps**;
//! - tiles of 32 physical pixels are laid from screen 0, each spanning `m = ⌊32 / Q⌋` screen
//!   pixels (for `Q` the quality function's maximum over the tile).
//!
//! `⌊32 / Q⌋` is a **step function**, so the derived answer is piecewise constant in `q` and the
//! interesting values of `q` are the ones **either side of a step**. A step sits where `Q` crosses
//! `32/m` for an integer `m`, which is `q = 32/m - 1/256`; the two clamps put a step at
//! `q = 1 - 1/256` and another at `q = 0.01 - 1/256`, below which every quality derives one
//! answer. [`qualities`] is that set: each clamp, each edge of a chosen set of `m`, the `f32`
//! either side of each edge, and a uniform fill between them so the sweep is not only edges.
//!
//! **Two inputs deriving one answer is the measurement, not a failure.** Below `0.01 - 1/256`
//! every quality is one answer, above `1 - 1/256` every quality is another, and inside a step every
//! quality is the step's. [`same_bucket`] names the pairs that must agree for that reason, and
//! `the_sweep_straddles_every_step_it_names` holds the table to it from the documented rule, so a
//! table edited to a value that no longer straddles its step fails rather than quietly comparing
//! one answer twice.
//!
//! # How every device-limited value is bounded
//!
//! A rate map is a shape that can fault a device: a physical extent past what the device grants, or
//! a layer count past its maximum, is exactly the shape that ends a process. Every bound here is
//! the **device's own answer** or a **measured** limit, never a guess:
//!
//! - **layer count** is bounded by `supportsRasterizationRateMapWithLayerCount:`, which the
//!   suite's own `device-rate-map-layer-counts` case records: on this family's oracle, an Apple M3
//!   Max on macOS 27.0.1, 1 and 2 are granted and 3 and 4 are refused. [`MAX_LAYERS`] is 2 and
//!   `no_shape_asks_for_more_layers_than_the_device_grants` holds every shape to it. A descriptor
//!   of three layers is **measured** to end plain Metal's process (SIGSEGV with no validation
//!   layer at all), so no case here builds one.
//! - **screen extent** is bounded by [`MAX_SWEPT_EXTENT`] of 1024, three orders of magnitude
//!   inside Metal's own validation bound of 32,768 and inside the 16,384 above which Apple's
//!   compile is measured to crash or return a corrupt map. The sweep has no use for a larger one:
//!   the step structure it measures repeats, and the parameter data is one fixed size at every
//!   extent (measured: 24,624 bytes at every extent probed).
//! - **physical extent** is derived and never allocated: nothing in this family makes a texture,
//!   opens a pass or reaches a queue, so a physical size is a number the case reads rather than
//!   memory a device must find.
//! - **quality samples per axis** are bounded by [`MAX_SWEPT_SAMPLES`] of 9 on a one-layer map and
//!   4 on a two-layer one, the most Apple's compile was measured with at each layer count, and by
//!   the screen extent plus one, past which the compile sees peaks between screen pixels by a rule
//!   nobody measured.
//!
//! # The three shapes that are not here, because each ends a process
//!
//! Measured on this oracle, with no validation layer: a descriptor of three layers ends plain
//! Metal; `mapScreenToPhysicalCoordinates:forLayer:` ends it over a map of **no layers** and over a
//! map of a **zero screen extent**. So [`CoordinateGrid`] is asked for only over a map with at
//! least one layer and no zero extent, which `no_coordinate_probe_reaches_a_fatal_map` holds.

use crate::case::RateMapLayer;

/// The physical width and height of every tile but each axis's last, for every map Metal compiles
/// (`physicalGranularity`, measured 32 by 32 on an Apple M3 Max and an M4).
pub const GRANULARITY: u32 = 32;

/// Apple's compile adds this to every quality sample before it clamps it, which puts a step of the
/// derived answer at `1 - BIAS` and another at `MIN_QUALITY - BIAS`.
pub const BIAS: f32 = 1.0 / 256.0;

/// The least quality Apple's compile clamps a biased sample to: one physical pixel per 100 screen
/// pixels. Every quality at or below `MIN_QUALITY - BIAS` derives one answer.
pub const MIN_QUALITY: f32 = 0.01;

/// The most layers any shape of the sweep asks for: `supportsRasterizationRateMapWithLayerCount:`
/// answers yes to 1 and 2 and no to 3 and 4 on this family's oracle, which the suite's own
/// `device-rate-map-layer-counts` recording states. A descriptor of three layers is measured to end
/// plain Metal's process, so the bound is the device's answer and not a preference.
pub const MAX_LAYERS: usize = 2;

/// The largest screen extent any shape of the sweep asks for.
///
/// Metal's own API validation stops an extent past 32,768, and Apple's compile is measured to crash
/// above 16,384 and to return a corrupt map at 16,385; this is half the lower of the two and a
/// quarter of the higher. Nothing is allocated at any extent — the sweep makes no texture, opens no
/// pass and reaches no queue — so an extent is arithmetic and a 24,624-byte parameter buffer, not
/// memory a device must find. Measured cost of the compile at this extent: 0.55 ms, against the
/// 20-second ceiling.
///
/// It is this large because a tile-span step is only **observable** where the span is narrower than
/// the screen: wider, and the whole screen is one tile either side of the step, so no derived answer
/// can move. The low end of the quality range lays tiles 3,200 screen pixels wide, so measuring its
/// step at all needs a screen wider than that. [`visible_extent`] is the rule, and most of the
/// sweep still runs at [`SWEEP_EXTENT`].
pub const MAX_SWEPT_EXTENT: u32 = 8192;

/// The most quality samples one axis carries, by the map's layer count: 9 on a map of one layer and
/// 4 on a map of two, the most Apple's compile was measured with at each. Indexed by layer count
/// minus one.
pub const MAX_SWEPT_SAMPLES: [usize; MAX_LAYERS] = [9, 4];

/// The screen extent the quality and sample sweeps hold fixed, so the only thing moving is the
/// quality. 256 is 8 whole tiles of the granularity at quality 1, so a step of the tile rule shows
/// as a whole number of tiles rather than inside the one partial tile at the end.
pub const SWEEP_EXTENT: u32 = 256;

/// `Q` of a quality sample: what Apple's compile clamps it to before it lays tiles. The sweep's own
/// tables are chosen by this, and nothing is ever **compared** against it — Metal is the answer.
pub fn biased(quality: f32) -> f32 {
    let biased = quality + BIAS;
    // A `NaN` sample reads 1, which is what Apple's compile does and what `f32::min` does by
    // answering the side that is not `NaN`. `f32::clamp` answers `NaN` instead, so the rule is
    // written out rather than left to it. No table here states a `NaN` — the sweep's own test
    // holds every sample inside [0, 1] — and writing the rule faithfully costs one line.
    if biased.is_nan() {
        return 1.0;
    }
    biased.clamp(MIN_QUALITY, 1.0)
}

/// The screen pixels one tile of the granularity spans at quality `quality`: `⌊32 / Q⌋`, the step
/// function whose edges the quality sweep straddles.
pub fn tile_span(quality: f32) -> u32 {
    (f32::from(GRANULARITY as u16) / biased(quality)) as u32
}

/// The screen extents the quality sweep runs at, narrowest first.
///
/// A ladder rather than one extent per quality, so the sweep holds three screen sizes and not
/// thirty: each quality takes the narrowest rung at which its own step is observable. Most of the
/// table sits on the first rung.
pub const EXTENT_LADDER: &[u32] = &[SWEEP_EXTENT, 1024, MAX_SWEPT_EXTENT];

/// The narrowest rung of [`EXTENT_LADDER`] at which a step of a `span`-pixel tile can be seen.
///
/// A tile-span step moves the derived answer only where the span is narrower than the screen: wider
/// than the screen and the whole screen is one tile either side of the step, so every derived answer
/// is the last-tile rule's and cannot move. Twice the span is the bound used, so the step moves a
/// whole tile and not only the partial one at the end. The widest rung is the answer where no rung
/// is wide enough, and `the_sweep_straddles_every_step_it_names` holds the table to the rungs being
/// wide enough for every step it names.
pub fn visible_extent(span: u32) -> u32 {
    for rung in EXTENT_LADDER {
        if *rung >= span.saturating_mul(2) {
            return *rung;
        }
    }
    MAX_SWEPT_EXTENT
}

/// One point of the quality sweep: a sample value, the extent it is swept at, and the name its site
/// carries.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Quality {
    /// The site's name, which says what the point is for rather than what it is.
    pub at: &'static str,
    /// The quality sample both axes of the layer take.
    pub quality: f32,
    /// The screen extent this quality is swept at: [`visible_extent`] of its own tile span, so the
    /// step it sits on is one a derived answer can show.
    pub extent: u32,
}

impl Quality {
    /// A point at the extent its own step is visible at.
    fn new(at: &'static str, quality: f32) -> Quality {
        Quality {
            at,
            quality,
            extent: visible_extent(tile_span(quality)),
        }
    }
}

/// The quality sample just below `quality` in `f32`.
pub const fn just_below(quality: f32) -> f32 {
    f32::from_bits(quality.to_bits() - 1)
}

/// The quality sample just above `quality` in `f32`.
pub const fn just_above(quality: f32) -> f32 {
    f32::from_bits(quality.to_bits() + 1)
}

/// How many `f32` steps the boundary searches walk before giving up.
///
/// Public because [`step_over`], [`over_low_clamp`] and [`under_high_clamp`] all name it in their
/// own contracts: each answers `None` where no quality within this many steps crosses its
/// boundary. A boundary is one or two
/// steps away in every case the tables use; the bound exists so a search cannot run away if a
/// constant here is ever edited into nonsense, and `every_boundary_is_found` holds each table
/// entry to having been found rather than defaulted.
pub const BOUNDARY_WALK: u32 = 64;

/// The edge of the tile rule's step at `m` screen pixels a tile: the quality whose biased value is
/// exactly `32 / m`, which is the **top** of bucket `m`.
///
/// The bucket is `Q ∈ (32/(m+1), 32/m]`, so the edge is inside bucket `m` and the other side of the
/// step is **above** it, not below — one more of quality is one fewer screen pixel a tile.
/// [`step_over`] finds that side.
pub fn step_edge(m: u32) -> f32 {
    f32::from(GRANULARITY as u16) / m as f32 - BIAS
}

/// The quality just over the step at [`step_edge`] of `m`: the nearest `f32` above it that lays
/// tiles of a **different** span, or `None` where no quality within [`BOUNDARY_WALK`] does.
///
/// `None` is the honest answer at the ends of the space, not a failure. At `m = 32` the edge is
/// `1 - BIAS`, where the biased quality is already clamped to 1, so every quality above it is one
/// answer and there is no step to cross; [`same_bucket`] is where that is stated instead.
///
/// This **searches** rather than assuming one `f32` crosses the boundary, which it often does not:
/// `1 - BIAS` plus one `f32` still biases to 1, because the sum lands half an ulp from 1 and
/// rounds back to it.
pub fn step_over(m: u32) -> Option<f32> {
    let edge = step_edge(m);
    let span = tile_span(edge);
    let mut quality = edge;
    for _ in 0..BOUNDARY_WALK {
        quality = just_above(quality);
        if quality > 1.0 {
            return None;
        }
        if tile_span(quality) != span {
            return Some(quality);
        }
    }
    None
}

/// The nearest quality above the low clamp's boundary whose biased value is no longer clamped: the
/// outside of the bucket every quality at or below `MIN_QUALITY - BIAS` sits in.
///
/// Searched for the same reason [`step_over`] searches: `MIN_QUALITY - BIAS` plus one `f32` biases
/// to a sum that rounds back to `MIN_QUALITY`, so one step does not leave the clamp.
pub fn over_low_clamp() -> Option<f32> {
    let mut quality = MIN_QUALITY - BIAS;
    for _ in 0..BOUNDARY_WALK {
        quality = just_above(quality);
        if biased(quality) > MIN_QUALITY {
            return Some(quality);
        }
    }
    None
}

/// The nearest quality below the high clamp's boundary whose biased value is no longer 1: the
/// outside of the bucket every quality at or above `1 - BIAS` sits in.
pub fn under_high_clamp() -> Option<f32> {
    let mut quality = 1.0 - BIAS;
    for _ in 0..BOUNDARY_WALK {
        quality = just_below(quality);
        if biased(quality) < 1.0 {
            return Some(quality);
        }
    }
    None
}

/// Every quality the sweep compares, over a map of one layer at [`SWEEP_EXTENT`].
///
/// Three groups, and the names say which is which. `clamp-*` are the two clamps and the values
/// either side of each: every quality at or below `MIN_QUALITY - BIAS` derives one answer and every
/// quality at or above `1 - BIAS` derives another, so each clamp is compared **at** its boundary,
/// one `f32` inside it, and one `f32` outside. `step-m<N>-*` are the tile rule's edges: `at` is the
/// quality whose biased value is exactly `32/N` and `below` is the `f32` under it, which must land
/// in the next step. `fill-*` is a uniform eighth-of-the-range fill, so the sweep is not only
/// edges and the measurement says what lies between them.
///
/// 0 and 1 are both here, as the ends of the space a program can state.
pub fn qualities() -> Vec<Quality> {
    let mut out = vec![
        // The ends of the declared range, and the low clamp around them. `clamp-low-zero`,
        // `clamp-low-bias` and `clamp-low-at` must derive ONE answer, at every extent: that is the
        // clamp, and `same_bucket` says so. `clamp-low-over` is the other side of it, and it is
        // swept where the side can be seen — the clamp lays tiles 3,200 screen pixels wide, so at
        // the sweep's own 256-pixel extent both sides are one tile and the step is invisible.
        Quality::new("clamp-low-zero", 0.0),
        Quality::new("clamp-low-bias", BIAS),
        Quality::new("clamp-low-at", MIN_QUALITY - BIAS),
        Quality::new("clamp-low-over", over_low_clamp().unwrap_or(MIN_QUALITY)),
        // The high clamp: `clamp-high-at` and `clamp-high-one` must derive one answer, and
        // `clamp-high-under` is the other side of the bias's boundary — which no derived answer
        // sees, because the tile span does not move across it. `same_derived_answer` states that.
        Quality::new("clamp-high-under", under_high_clamp().unwrap_or(1.0 - BIAS)),
        Quality::new("clamp-high-at", 1.0 - BIAS),
        Quality::new("clamp-high-one", 1.0),
    ];
    // The tile rule's steps. Each `m` contributes the quality at the top of bucket `m` and the
    // nearest quality ABOVE it that lays tiles of a different span — above, because one more of
    // quality is one fewer screen pixel a tile. Where no quality crosses the step the edge stands
    // alone: at `m = 32` the clamp has already swallowed both sides, which `same_bucket` states.
    for m in STEP_TILES {
        let edge = step_edge(*m);
        out.push(Quality::new(step_name(*m, false), edge));
        if let Some(over) = step_over(*m) {
            out.push(Quality::new(step_name(*m, true), over));
        }
    }
    // The uniform fill. An eighth of the range, which the oracle derives as eight distinct physical
    // widths, one of them (0.625) the only probed quality whose coordinate maps answer a
    // non-integer: a point no edge of the table would have found.
    for (index, eighth) in FILL.iter().enumerate() {
        out.push(Quality::new(FILL_NAMES[index], *eighth));
    }
    out
}

/// The tile spans whose step edges the sweep straddles: the identity, the two either side of it,
/// the halves and quarters of the range, and the span the low clamp itself derives.
pub const STEP_TILES: &[u32] = &[32, 33, 48, 64, 65, 128, 256, 512, 1024, 3200];

/// The uniform fill's qualities: an eighth of the declared range, ends excluded (the clamps hold
/// those).
pub const FILL: &[f32] = &[0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875];

/// The fill's site names, in [`FILL`]'s order. Spelled out rather than formatted, so a site name is
/// a literal of this source and a reader can grep a recording for it.
pub const FILL_NAMES: &[&str] = &[
    "fill-eighth-1",
    "fill-eighth-2",
    "fill-eighth-3",
    "fill-eighth-4",
    "fill-eighth-5",
    "fill-eighth-6",
    "fill-eighth-7",
];

/// The site name of a tile-rule step edge.
fn step_name(m: u32, below: bool) -> &'static str {
    // A literal per point, because a site name must be a constant a reader can find in this
    // source and in a recording. The table and this function are held together by
    // `every_step_has_a_name`.
    match (m, below) {
        (32, false) => "step-m32-at",
        (32, true) => "step-m32-over",
        (33, false) => "step-m33-at",
        (33, true) => "step-m33-over",
        (48, false) => "step-m48-at",
        (48, true) => "step-m48-over",
        (64, false) => "step-m64-at",
        (64, true) => "step-m64-over",
        (65, false) => "step-m65-at",
        (65, true) => "step-m65-over",
        (128, false) => "step-m128-at",
        (128, true) => "step-m128-over",
        (256, false) => "step-m256-at",
        (256, true) => "step-m256-over",
        (512, false) => "step-m512-at",
        (512, true) => "step-m512-over",
        (1024, false) => "step-m1024-at",
        (1024, true) => "step-m1024-over",
        (3200, false) => "step-m3200-at",
        (3200, true) => "step-m3200-over",
        _ => "step-unnamed",
    }
}

/// Pairs of the quality sweep that must derive the **same** answer, each with why.
///
/// This is the quantization, stated as a prediction the comparison can falsify. A pair here whose
/// answers differ is a finding about the clamp; a pair not here whose answers agree is a step the
/// table failed to straddle, which `the_sweep_straddles_every_step_it_names` catches for the steps
/// it names.
pub fn same_bucket() -> Vec<(&'static str, &'static str, &'static str)> {
    vec![
        (
            "clamp-low-zero",
            "clamp-low-at",
            "every quality at or below MIN_QUALITY - BIAS biases to MIN_QUALITY",
        ),
        (
            "clamp-low-bias",
            "clamp-low-at",
            "the bias itself is inside the low clamp",
        ),
        (
            "clamp-high-at",
            "clamp-high-one",
            "every quality at or above 1 - BIAS biases to 1",
        ),
        (
            "step-m32-at",
            "clamp-high-one",
            "32/32 is 1, so the identity's step edge IS the high clamp and has no step above it",
        ),
        (
            "step-m3200-at",
            "clamp-low-at",
            "32/3200 is MIN_QUALITY, so the last step edge is the low clamp",
        ),
        (
            "step-m3200-over",
            "clamp-low-over",
            "the low clamp's own step IS m3200's, so the first quality past the clamp and the \
             first quality over that step are one quality",
        ),
    ]
}

/// Pairs of the quality sweep whose **biased** quality differs and whose derived answer is
/// predicted to be the same anyway, each with why.
///
/// [`same_bucket`] is the other prediction and a stronger one: those pairs bias to one quality, so
/// everything downstream of the bias is bound to agree, at any screen extent. This pair biases to
/// *two* qualities and still derives one answer, because the tile rule `⌊32 / Q⌋` is a coarser step
/// than the `f32` grid the bias moves on and the bias's own boundary falls inside one of its steps.
///
/// **That is the measurement, not a weakness.** A boundary of the bias that no derived answer can
/// see is a boundary another implementation is free to put somewhere else, and this is the suite
/// saying so out loud rather than a case quietly passing. The low clamp is **not** here, and the
/// difference between the two clamps is the whole lesson: `32/0.01` is exactly 3,200, so leaving
/// the low clamp drops the tile span to 3,199 at once and the step shows at any screen wider than
/// it — which is why [`visible_extent`] sweeps that quality at 8,192 instead of 256. The high
/// clamp's span does not move at all: `32/1` is 32 screen pixels a tile and the next step down is
/// at `32/33`, three hundredths of quality away, so no extent makes it visible.
pub fn same_derived_answer() -> Vec<(&'static str, &'static str, &'static str)> {
    vec![
        (
            "clamp-high-at",
            "clamp-high-under",
            "the tile span does not move across the high clamp's boundary: 32/1 is 32 screen \
             pixels a tile and the next step down is at 32/33, so no screen extent can show the \
             bias moving",
        ),
        // The three below are the table's own structure showing through, and they are listed for
        // the same reason as the clamp: a collapse nobody wrote down is a site that looks like a
        // measurement and is not. `step_over(m)` is the first quality INTO bucket `m - 1`, and
        // `step_edge(m - 1)` is the top of that same bucket, so naming two adjacent spans in
        // `STEP_TILES` necessarily names one bucket twice. 32/33 and 64/65 are both adjacent
        // pairs, and they are kept adjacent on purpose: a step is only a measurement if both of
        // its sides are stated, and the cost of stating them is this overlap.
        (
            "step-m33-over",
            "step-m32-at",
            "the quality over m33's step is the first one in bucket 32, whose top is m32's edge — \
             and bucket 32 is the identity, so this collapses into the high clamp's answer too",
        ),
        (
            "step-m65-over",
            "step-m64-at",
            "the quality over m65's step is the first one in bucket 64, whose top is m64's edge",
        ),
        (
            "step-m64-over",
            "fill-eighth-4",
            "the quality over m64's step is in bucket 63, and so is the fill's half (0.5 biases \
             to 0.50390625, which lays 63-pixel tiles)",
        ),
    ]
}

/// The screen extents the sweep compares, at one fixed quality.
///
/// The granularity is 32, so the point is the extents that **do** and **do not** divide by it and
/// the extent either side of each boundary: 32 and 33 are the pair that matters most, because the
/// oracle derives a **smaller** physical width for 33 than for 32 (measured: 32 and 24), so an
/// implementation that rounded the last tile up instead of down reads larger at 33 and passes
/// every other extent. 1 and 2 are the smallest maps a program can ask for; 0 is not here because
/// the suite already compares it whole, parameter data included, as `rate-map-zero-screen-width`.
pub const SCREENS: &[u32] = &[
    1, 2, 31, 32, 33, 63, 64, 65, 127, 128, 255, 256, 257, 512, 1023, 1024,
];

/// The quality the screen sweep holds fixed. A half is inside every step of the tile rule rather
/// than on an edge of one, so a derived extent that moves with the screen size moves for the screen
/// size's own reason.
pub const SCREEN_SWEEP_QUALITY: f32 = 0.5;

/// A quality profile over one axis, as the samples a layer carries.
///
/// The sample count is its own axis of the space and needs a profile the count can **change**. A
/// linear ramp cannot: a straight line sampled at any number of points is the same piecewise linear
/// function, and the oracle derives one answer (192) for every count from 2 to 9 over one. This is
/// a peak instead, placed off centre so that two samples differ from one as well.
pub fn peak_profile(samples: usize) -> Vec<f32> {
    if samples == 1 {
        return vec![PEAK_LOW];
    }
    (0..samples)
        .map(|index| {
            let t = index as f32 / (samples - 1) as f32;
            // A peak at 3/8 of the axis: asymmetric, so an even count does not straddle it the way
            // an odd one does and the derived answer moves with the parity too.
            let distance = (t - 0.375).abs() / 0.625_f32.max(0.375);
            PEAK_LOW + (PEAK_HIGH - PEAK_LOW) * (1.0 - distance).max(0.0)
        })
        .collect()
}

/// The peak profile's low and high quality. Both are inside a step of the tile rule, so the derived
/// answer moves because the **function** moved and not because one sample crossed a clamp.
pub const PEAK_LOW: f32 = 0.25;
pub const PEAK_HIGH: f32 = 0.9;

/// The sample counts the sweep compares on a map of one layer: every count from 1 to the most
/// Apple's compile was measured with.
pub fn sample_counts() -> Vec<usize> {
    (1..=MAX_SWEPT_SAMPLES[0]).collect()
}

/// A screen coordinate and a physical coordinate to map, and the layer to map them in.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CoordinateGrid {
    /// `mapScreenToPhysicalCoordinates:forLayer:` is asked at each of these, as `(x, y)`.
    pub screen: &'static [(f32, f32)],
    /// `mapPhysicalToScreenCoordinates:forLayer:` is asked at each of these, as a fraction of the
    /// layer's derived physical extent, so one grid reads every map at its own extremes.
    pub physical: &'static [(f32, f32)],
}

/// The grid every swept map's coordinate maps are read over.
///
/// Screen coordinates are **absolute**, at the extremes and at the tile boundaries the granularity
/// puts them: the origin, one pixel in, either side of the first tile's edge, the middle, the last
/// pixel, and one past the last — which is the extreme Metal answers for and the one an
/// implementation that clamped a pixel early would read wrong.
///
/// Physical coordinates are **fractions of the derived extent**, because the extent is what the
/// sweep is measuring: an absolute physical coordinate inside one map is outside another, and a
/// grid that read 0 to 32 of every map would read the first tile of a wide one and the whole of a
/// narrow one. A fraction reads the same place in every map.
pub const GRID: CoordinateGrid = CoordinateGrid {
    screen: &[
        (0.0, 0.0),
        (1.0, 1.0),
        (31.0, 31.0),
        (32.0, 32.0),
        (33.0, 33.0),
        (128.0, 128.0),
    ],
    physical: &[(0.0, 0.0), (0.5, 0.5), (1.0, 1.0)],
};

/// The site a mapped answer sits at, as `<direction>-<index>`.
pub fn mapped_site(direction: &str, index: usize) -> String {
    format!("{direction}-{index}")
}

/// `screen-to-physical`, the direction Metal refuses over a map of no layers and over a map of a
/// zero screen extent.
pub const SCREEN_TO_PHYSICAL: &str = "screen-to-physical";

/// `physical-to-screen`, which Metal answers over a map of a zero screen extent and ends its
/// process over a map of no layers.
pub const PHYSICAL_TO_SCREEN: &str = "physical-to-screen";

/// One layer whose quality is `quality` on both axes.
pub fn uniform_layer(quality: f32) -> RateMapLayer {
    RateMapLayer::uniform(quality, quality)
}

/// The two-layer shapes the sweep compares, each with the name its site carries and why it is here.
///
/// Two is the device's maximum, so this is the whole of the layer axis above one. Metal's parameter
/// data holds **one** vertical axis for every layer, so a descriptor whose layers disagree
/// vertically is the shape that says which one the compile keeps: measured on the oracle, a map of
/// vertical qualities 0.25 and 1.0 derives a vertical extent of 256 for **both** layers, the
/// larger. That is the row an implementation taking layer 0's vertical axis, or the smaller of the
/// two, reads wrong.
pub fn layer_shapes() -> Vec<(&'static str, Vec<RateMapLayer>)> {
    vec![
        (
            "one-layer",
            vec![RateMapLayer::uniform(1.0, SCREEN_SWEEP_QUALITY)],
        ),
        (
            "two-layers-horizontal",
            vec![
                RateMapLayer::uniform(1.0, SCREEN_SWEEP_QUALITY),
                RateMapLayer::uniform(0.25, SCREEN_SWEEP_QUALITY),
            ],
        ),
        (
            "two-layers-vertical-differ",
            vec![
                RateMapLayer::uniform(SCREEN_SWEEP_QUALITY, 0.25),
                RateMapLayer::uniform(SCREEN_SWEEP_QUALITY, 1.0),
            ],
        ),
        (
            "two-layers-equal",
            vec![
                RateMapLayer::uniform(0.5, 0.5),
                RateMapLayer::uniform(0.5, 0.5),
            ],
        ),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeSet;

    /// Every step the table names is **straddled**, and at an extent where the straddle can be
    /// seen: the quality at the top of the bucket and the one over it lay tiles of different spans,
    /// and the extent both are swept at is wider than either span.
    ///
    /// This is the check that makes the sweep a measurement. Without it a table could hold thirty
    /// qualities that all derive one answer and every site would pass. It is also the check that
    /// found the first version of this table wrong three times: one `f32` above `1 - BIAS` still
    /// biases to 1; the step at `m = 32` is the clamp itself; and the steps below `m = 128` lay
    /// tiles wider than the 256-pixel screen they were swept at, so the screen was one tile either
    /// side and nothing could move.
    #[test]
    fn the_sweep_straddles_every_step_it_names() {
        let by_name: std::collections::BTreeMap<&str, Quality> =
            qualities().into_iter().map(|q| (q.at, q)).collect();
        for m in STEP_TILES {
            let edge = step_edge(*m);
            match step_over(*m) {
                Some(over) => {
                    assert_ne!(
                        tile_span(edge),
                        tile_span(over),
                        "m{m}: the edge quality {edge} and {over} over it lay tiles of the same \
                         span, so the pair compares one answer twice"
                    );
                    // And the extent they are swept at can show it. Both sides take the same rung,
                    // because the rule reads the span and the two spans differ by one.
                    let at = by_name[step_name(*m, false)];
                    let past = by_name[step_name(*m, true)];
                    assert_eq!(
                        at.extent, past.extent,
                        "m{m}'s two sides are swept at {} and {}, so they are not comparable",
                        at.extent, past.extent
                    );
                    assert!(
                        at.extent > tile_span(edge) && at.extent > tile_span(over),
                        "m{m} is swept at {} with tiles of {} and {} screen pixels, so the whole \
                         screen is one tile either side of the step and no derived answer can move",
                        at.extent,
                        tile_span(edge),
                        tile_span(over)
                    );
                }
                // No quality crosses the step, which is the honest answer at the ends of the
                // space and is why the table leaves the pair's second half out there. `m = 32`'s
                // edge is the high clamp and `m = 3200`'s is the low one, and `same_bucket`
                // states both.
                None => assert!(
                    *m == 32,
                    "m{m} names a step no quality crosses, so it is a point of the table that \
                     compares nothing"
                ),
            }
        }
    }

    /// Every quality is swept at an extent wider than the tiles it lays, so each point of the
    /// sweep sits on a screen the step structure it measures can appear on.
    #[test]
    fn every_quality_is_swept_where_its_step_shows() {
        for quality in qualities() {
            assert!(
                quality.extent <= MAX_SWEPT_EXTENT,
                "{} is swept at {}, past the bound {MAX_SWEPT_EXTENT}",
                quality.at,
                quality.extent
            );
            assert!(
                EXTENT_LADDER.contains(&quality.extent),
                "{} is swept at {}, which is not a rung of the ladder",
                quality.at,
                quality.extent
            );
            assert!(
                quality.extent > tile_span(quality.quality),
                "{} lays tiles of {} screen pixels on a screen of {}, so the whole screen is one \
                 tile and nothing it measures can move",
                quality.at,
                tile_span(quality.quality),
                quality.extent
            );
        }
    }

    /// Every boundary the table searches for was **found**, so no entry of it is a default
    /// standing in for a boundary that does not exist.
    #[test]
    fn every_boundary_is_found() {
        assert!(
            over_low_clamp().is_some(),
            "no quality within {BOUNDARY_WALK} f32 steps leaves the low clamp"
        );
        assert!(
            under_high_clamp().is_some(),
            "no quality within {BOUNDARY_WALK} f32 steps leaves the high clamp"
        );
        for m in STEP_TILES {
            if *m == 32 || *m == 3200 {
                continue;
            }
            assert!(
                step_over(*m).is_some(),
                "no quality within {BOUNDARY_WALK} f32 steps crosses m{m}'s step"
            );
        }
    }

    /// Each pair the table calls one bucket really is one bucket under the documented rule, so a
    /// pair of sites whose answers agree is a prediction this module made and not a coincidence.
    #[test]
    fn every_same_bucket_pair_biases_to_one_quality() {
        let by_name: std::collections::BTreeMap<&str, f32> =
            qualities().into_iter().map(|q| (q.at, q.quality)).collect();
        for (left, right, why) in same_bucket() {
            let a = by_name[left];
            let b = by_name[right];
            assert_eq!(
                biased(a).to_bits(),
                biased(b).to_bits(),
                "{left} and {right} are declared one bucket ({why}) and bias to {} and {}",
                biased(a),
                biased(b)
            );
            // One bias is one tile span is one rung of the ladder, so the pair is swept at one
            // extent and its two sites are comparable at all.
            assert_eq!(
                visible_extent(tile_span(a)),
                visible_extent(tile_span(b)),
                "{left} and {right} are declared one bucket and swept at two extents ({why})"
            );
        }
    }

    /// Each clamp bites where the table says it does: at the boundary the biased quality is
    /// exactly the clamp, and at the quality the table puts past it the clamp no longer bites.
    ///
    /// The assertion is on the **bias**, which is where a clamp lives. Whether the derived answer
    /// can see the clamp at all is a different question, and the measured answer at
    /// [`SWEEP_EXTENT`] is that it cannot — [`same_derived_answer`] states that, with the reason.
    #[test]
    fn each_clamp_bites_where_the_table_says() {
        let by_name: std::collections::BTreeMap<&str, f32> =
            qualities().into_iter().map(|q| (q.at, q.quality)).collect();
        assert_eq!(biased(by_name["clamp-low-at"]), MIN_QUALITY);
        assert!(
            biased(by_name["clamp-low-over"]) > MIN_QUALITY,
            "clamp-low-over still clamps, so the table has no outside of the low clamp"
        );
        assert_eq!(biased(by_name["clamp-high-at"]), 1.0);
        assert!(
            biased(by_name["clamp-high-under"]) < 1.0,
            "clamp-high-under still clamps, so the table has no outside of the high clamp"
        );
    }

    /// The pair declared to derive one answer despite two biases really does bias apart, and
    /// really does lay tiles of one span — which is the only reason it can derive one answer.
    #[test]
    fn the_same_derived_pairs_really_bias_apart() {
        let by_name: std::collections::BTreeMap<&str, f32> =
            qualities().into_iter().map(|q| (q.at, q.quality)).collect();
        for (left, right, why) in same_derived_answer() {
            let (a, b) = (by_name[left], by_name[right]);
            assert_ne!(
                biased(a).to_bits(),
                biased(b).to_bits(),
                "{left} and {right} bias to one quality, so the pair belongs in same_bucket \
                 rather than here ({why})"
            );
            assert_eq!(
                tile_span(a),
                tile_span(b),
                "{left} lays tiles of {} screen pixels and {right} of {}, so the pair's claim \
                 that no extent can tell them apart is wrong — sweep it where the step shows \
                 instead ({why})",
                tile_span(a),
                tile_span(b)
            );
        }
    }

    /// No site name is used twice, so every point of the sweep has its own site and a diff names
    /// one quality rather than a set of them.
    #[test]
    fn every_quality_has_its_own_site() {
        let names: BTreeSet<&str> = qualities().iter().map(|q| q.at).collect();
        assert_eq!(names.len(), qualities().len(), "a site name is used twice");
        assert!(
            !names.contains("step-unnamed"),
            "a step of STEP_TILES has no name in step_name"
        );
    }

    /// Every step of the table has a spelled-out name, so adding one to `STEP_TILES` without a
    /// name fails here rather than writing `step-unnamed` into a recording.
    #[test]
    fn every_step_has_a_name() {
        for m in STEP_TILES {
            for below in [false, true] {
                assert_ne!(
                    step_name(*m, below),
                    "step-unnamed",
                    "m{m} (below: {below}) has no spelled-out site name"
                );
            }
        }
    }

    /// No shape of the sweep asks for more layers than the device grants. The bound is the
    /// device's own `supportsRasterizationRateMapWithLayerCount:`, which the suite records; three
    /// layers is measured to end plain Metal's process.
    #[test]
    fn no_shape_asks_for_more_layers_than_the_device_grants() {
        for (name, layers) in layer_shapes() {
            assert!(
                layers.len() <= MAX_LAYERS,
                "{name} asks for {} layers and the device grants {MAX_LAYERS}",
                layers.len()
            );
            assert!(!layers.is_empty(), "{name} has no layer");
        }
    }

    /// No shape of the sweep asks for a screen extent past the bound, and no sample count past the
    /// most Apple's compile was measured with at its layer count.
    #[test]
    fn every_swept_number_is_inside_its_measured_bound() {
        for extent in SCREENS {
            assert!(
                *extent <= MAX_SWEPT_EXTENT,
                "screen extent {extent} is past the swept bound {MAX_SWEPT_EXTENT}"
            );
            assert!(*extent > 0, "extent 0 is `rate-map-zero-screen-width`'s");
        }
        for count in sample_counts() {
            assert!(
                count <= MAX_SWEPT_SAMPLES[0],
                "{count} samples on a one-layer map, above the measured {}",
                MAX_SWEPT_SAMPLES[0]
            );
            assert!(
                count as u32 <= SWEEP_EXTENT + 1,
                "{count} samples over {SWEEP_EXTENT} pixels is a density nobody measured"
            );
        }
        for (name, layers) in layer_shapes() {
            let max = MAX_SWEPT_SAMPLES[layers.len() - 1];
            for (index, layer) in layers.iter().enumerate() {
                for (axis, samples) in [&layer.horizontal, &layer.vertical].into_iter().enumerate()
                {
                    assert!(
                        samples.len() <= max,
                        "{name} layer {index} axis {axis} has {} samples, above the {max} \
                         measured at {} layers",
                        samples.len(),
                        layers.len()
                    );
                }
            }
        }
    }

    /// The peak profile distinguishes its sample counts: a profile that derived one quality
    /// function for every count would compare one answer nine times, which is what a linear ramp
    /// does (measured: every count from 2 to 9 derives 192).
    ///
    /// The check is on the **profile**, which is what this module owns; that the derived answer
    /// then moves is Metal's to say and the sweep's to record.
    #[test]
    fn the_peak_profile_is_a_different_function_at_every_count() {
        // Two profiles describe one function where sampling the first at the second's points gives
        // the second. Sampling the peak at an integer count is exact only where the counts share
        // the peak's position, so comparing the samples' own shape is enough: the maximum's
        // position along the axis moves with the count.
        let mut peaks = BTreeSet::new();
        for count in sample_counts() {
            let profile = peak_profile(count);
            assert_eq!(profile.len(), count);
            let top = profile
                .iter()
                .enumerate()
                .max_by(|a, b| a.1.partial_cmp(b.1).expect("no NaN in the profile"))
                .expect("a profile has a sample")
                .0;
            // Where along the axis the sampled peak sits, in 1/1000ths, which is the profile's
            // shape rather than its sample count.
            let at = if count == 1 {
                0
            } else {
                (1000 * top / (count - 1)) as u32
            };
            peaks.insert((at, profile.len()));
            for sample in &profile {
                assert!(
                    (0.0..=1.0).contains(sample),
                    "{count} samples put {sample} outside [0, 1], where Apple's compile is \
                     measured only for a lone sample"
                );
            }
        }
        assert_eq!(
            peaks.len(),
            sample_counts().len(),
            "two sample counts give the same sampled peak, so the sweep compares one function \
             twice"
        );
    }

    /// Every coordinate the grid names is inside the map the sweep reads it over, and the physical
    /// side is a fraction so one grid fits every derived extent.
    #[test]
    fn the_grid_stays_inside_the_maps_it_is_read_over() {
        for (x, y) in GRID.screen {
            assert!(
                *x <= SWEEP_EXTENT as f32 && *y <= SWEEP_EXTENT as f32,
                "the grid asks ({x}, {y}) of a map of {SWEEP_EXTENT}"
            );
            assert!(*x >= 0.0 && *y >= 0.0);
        }
        for (x, y) in GRID.physical {
            assert!(
                (0.0..=1.0).contains(x) && (0.0..=1.0).contains(y),
                "a physical probe is a fraction of the derived extent, and ({x}, {y}) is not one"
            );
        }
    }

    /// No shape the coordinate maps are read over is one of the three measured to end Metal's
    /// process: a map of no layers, or a map with a zero screen extent.
    #[test]
    fn no_coordinate_probe_reaches_a_fatal_map() {
        for (name, layers) in layer_shapes() {
            assert!(
                !layers.is_empty(),
                "{name} has no layer, and the coordinate maps end Metal's process over one"
            );
        }
        for extent in SCREENS {
            assert!(
                *extent > 0,
                "a zero screen extent ends Metal's process in screen-to-physical"
            );
        }
        for rung in EXTENT_LADDER {
            assert!(
                *rung > 0,
                "a rung of the extent ladder is 0, where screen-to-physical ends Metal's process"
            );
        }
    }
}
