//! The per-call cost gate, and the ledger of calls we already know are over it.
//!
//! # Why the gate is per call and not per case
//!
//! The per-case margin is the quantity nothing can dodge by moving work between the three
//! attributions, and it is **insensitive**. Measured over the whole set: no case exceeds 3.9x, and
//! the largest absolute gap of a comparable case is 2.6x — while one *call*,
//! `replace_region`, runs at 265x on the 16 cases that make it, because a case's total dilutes one
//! expensive call. A case-level margin could never have fired on that, which is the argument for
//! this module and not for a tighter margin.
//!
//! So the gate holds each **paired call row** — summed over the cases both legs did exactly
//! ([`crate::times::paired`]) — to [`crate::times::MARGIN`], an order of magnitude. That margin is
//! kept rather than tightened on measured grounds: over two whole-set runs of one binary, Metal's
//! own per-call totals moved a median 1.28x and as much as 2.43x, so a 2x margin would fire on the
//! machine being busy while a 10x margin did not move a verdict.
//!
//! # Why this is a second file and not a row in the known-gaps ledger
//!
//! [`crate::known_gaps`] holds refusals and divergences in **one** file on an explicit argument:
//! `AGENTS.md` forbids parallel string lists, and two ledgers for two kinds of correctness verdict
//! would be exactly that. That argument has to be answered here rather than ignored, because this
//! is a third file of the same shape.
//!
//! It is answered by the line the cost half already draws. A gap row is a **correctness** verdict
//! on a case at a site, held by `replay`, which needs no Metal. A row here is a **cost** result on
//! a call, held by `times`, which needs both legs on one machine. The counters half made the same
//! split deliberately and for the same reason — its `CostVerdict` is `Within`/`Over`/`Unmeasured`
//! rather than a new `Verdict` variant, because a count is ours and not Metal's and must never
//! touch a correctness verdict. Folding a ratio and a band into a file whose rows name a verdict
//! and a venue would put cost inside the correctness vocabulary, which is the thing both halves
//! have been careful not to do.
//!
//! What it does share is the mechanism, and that is on purpose: a known gap that makes every run
//! red teaches everyone to stop reading the report, which costs us the next real one.
//!
//! **If the round would rather have one file, this folds into it**: the row gains a verdict kind
//! and a ratio, and `held` moves beside `known_gaps::held`. Nothing here depends on being separate
//! except the argument above.
//!
//! # The ledger is an expectation, not an allowlist, and it is held in both directions
//!
//! - **A call over the margin that the ledger does not list fails the run** ([`Expectation::Unlisted`]).
//! - **A listed call that has come back inside the band also fails the run**
//!   ([`Expectation::NoLongerSlow`]), saying the row looks fixed and must go.
//!
//! That second direction is the point: closing a gap forces the ledger edit, so a row cannot
//! outlive the bug it stands for.
//!
//! # The band, and why a ratio needs one where a refusal does not
//!
//! A refusal is a yes or a no, so `known-refusals.tsv` needs no tolerance. A ratio is a
//! measurement, and this one moves: the 2.43x worst movement above means a row measured at 11x can
//! read 5x on a quiet run through nothing changing. Holding it to "still over 10x" would make the
//! `NoLongerSlow` direction fire on noise.
//!
//! So a listed row is reported, not failed, until it falls under [`band`] — `MARGIN / 2.5`, where
//! 2.5 is above the measured 2.43x. Between the band and the margin a listed row is neither a pass
//! nor a failure, and the report says so in those words rather than picking a side a single run
//! cannot support.
//!
//! # What the gate deliberately cannot see
//!
//! It is an **upper bound on our own cost**, so a row where we are *faster* than Metal never fires
//! it. That is the right asymmetry here: being cheaper than Metal for a reason the suite has not
//! explained is not something a gate should bless — `library` reads 0.005x because of shader reuse
//! and a cache below us, not because of a compiler win ([`crate::run::Shaders`]) — and a gate that
//! credited it would turn an unexplained setup difference into a green light.

/// The committed ledger, compiled in so the check needs no path and no filesystem.
const LEDGER: &str = include_str!("../known-slow-calls.tsv");

/// Where a listed row stops being reported and starts failing as fixed: [`crate::times::MARGIN`]
/// divided by this.
///
/// **7.0, and the number it replaced was measured on the wrong quantity.** It was 2.5, above a
/// 2.43x worst movement measured in *Metal's own per-call totals*. What the `NoLongerSlow`
/// direction fires on is the **ratio**, and a ratio moves by more than either of its halves
/// because the two halves move independently: over the degraded whole-set pass and a good one, the
/// 98 paired rows common to both moved a median 2.62x, 14.96x at the 90th percentile and 63.9x at
/// worst. Restricted to the rows the measurement can resolve at all ([`RESOLVES_AT`]) it is a
/// median 1.87x and 6.92x at the 90th percentile — so against a 10x margin, a divisor under 6.92
/// lets a pass read a row at 11x as fixed at 1.6x and delete a live bug's ledger row. 7.0 is the
/// top of that measured range and nothing tighter is claimed.
///
/// **A third whole-set pass says that movement is the degraded pass and not the ratio.** Re-read
/// from the stored recordings, the two **undegraded** passes move a median 1.12x, 1.51x at the
/// 90th percentile and 2.00x at worst over all 100 rows they share — under even the 2.43x the
/// 2.5 divisor was set from. So this divisor is paying for a pass [`judges`] should have refused
/// and did not: that pass's instrument readings are 1.17x apart, inside what
/// [`CALIBRATION_MAY_MOVE`] allows, while its ratios move 64x.
///
/// **That check is now tightened** ([`FLOOR_MAY_REACH`]), and the pass this divisor pays for is
/// refused by its own figures — verified both ways over all three stored passes
/// (`tests::the_floor_rule_refuses_the_degraded_pass_and_keeps_the_undegraded_ones`). **So the
/// divisor is the owner's to lower, and here is what the evidence supports.** What the
/// `NoLongerSlow` direction fires on is a *resolved* row's ratio, and between the two undegraded
/// passes those moved a median 1.10x, 1.51x at the 90th percentile and **1.97x at worst** over the
/// 56 rows they share. A row genuinely over a 10x margin can therefore read as low as 10 / 1.97 =
/// 5.08x on another undegraded pass, so any divisor at or above **1.97** keeps the direction sound
/// on the measured worst case; **2.5 — the number this constant replaced — puts the band at 4.0x
/// with 27% of headroom over that worst case**, and is now justified on the right quantity rather
/// than on Metal's own totals.
///
/// Lowering it to 2.5 would move a fixed row's retirement from **1.43x to 4.0x**. Over the 19:57
/// pass and today's ledger it changes no row's verdict — the six `Known` rows read 19.8x to 48.4x
/// and the two in the band read 4.42x and 5.30x — so what it buys is forward: `tile_pipeline` at
/// 4.42x and `render_pipeline_with_reflection` at 5.30x would announce themselves fixed on
/// reaching 4.0x instead of having to reach 1.43x, which is a ratio a real fix produces and 1.43x
/// is not. Whoever lowers it also moves
/// `tests::a_listed_row_in_the_band_is_neither_a_pass_nor_a_failure`, whose assertion is written
/// against the degraded pair's 6.92x and would want the undegraded 1.97x.
///
/// Until then the cost of the wider divisor stands: a row genuinely fixed stays listed until it
/// reads under 1.43x, and the `NoLongerSlow` direction therefore says less than it did.
pub const BAND_DIVISOR: f64 = 7.0;

/// The ratio a listed row must fall under before the run reads it as fixed.
pub fn band() -> f64 {
    crate::times::MARGIN as f64 / BAND_DIVISOR
}

/// **Why a cold measurement is reported and never judged.** `--cold-shaders` gives each case a
/// device and a shader cache of its own, so it changes two things at once and only on our leg: no
/// case is handed another's compile, and every per-device first-use cost lands inside whichever
/// call touches a resource first. The whole-set numbers say how much of the second there is --
/// warm against cold on this M3 Max, `target` goes 0.9x to 14.3x, `dispatch` 1.0x to 17.7x and
/// `copy_rate_map_parameters` 0.4x to 26.4x, none of which compiles anything. A cold reading over
/// the margin therefore does not say which of the two it is, and a gate wants a reading that
/// localises. Nine calls were over the margin in the cold whole-set run against four warm; seven
/// of the nine would have been one row each for one mechanism, which is the ledger its own header
/// warns about.
pub const WHY_COLD_IS_NOT_JUDGED: &str =
    "the margin is reported and not judged for a cold measurement: --cold-shaders also gives \
     every case\na fresh device, so a reading over the margin does not separate our compile \
     cost from the cost of\nfirst use on a new device -- measured, `target` is 0.9x warm and \
     14.3x cold without compiling anything.\nRun it without --cold-shaders for the gate.";

/// **The ceiling on host load, measured.**
///
/// This was 4.0, set conservatively and labelled at the time as stated rather than localised.
/// It has since been measured, and the measurement says absolute load is very nearly the wrong
/// variable: over twenty passes of one twelve-case selection on this M3 Max, spanning a load
/// average of 8.59 to 26.45 (3.08x), `render_pipeline` stayed between 51.6x and 56.6x -- a spread
/// of **1.10x** -- and `compute_pipeline` between 29.3x and 33.1x, a spread of 1.13x. There is no
/// knee and no drift worth a gate: against a 10x margin a 1.1x wobble is nothing.
///
/// One confound is declared rather than hidden: load fell monotonically across those passes, so
/// load and pass order are collinear and the small residual correlation (+0.64 with load, -0.59
/// with order) cannot be attributed to either. It does not matter for the bound, because the
/// *effect size* is about 10% whichever variable owns it.
///
/// So the bound is the top of the range that was actually measured, and nothing above it is
/// claimed. What does corrupt a ratio is [`LOAD_MAY_MOVE`]: the legs run one after the other, so
/// load moving *between* them compares two machines.
pub const QUIET_ENOUGH: f64 = 26.0;

/// **How far the load may move across one measurement before its two halves are not one
/// condition.** A factor, applied to the larger reading over the smaller.
///
/// **This is the guard that the evidence actually supports**, and the mechanism is the leg order:
/// `times` measures Metal's leg to completion and then ours, deliberately, because interleaving
/// would put each leg's threads in the other's off-thread CPU reading. So the two legs occupy two
/// different windows -- about six seconds apart in a twelve-case pass, but minutes apart in a
/// whole-set pass -- and load moving between those windows divides one machine's Metal figure by
/// another machine's figure of ours. That, rather than the absolute level, is what produced the
/// 4.5x-against-1.6x contrast between two whole-set runs, and it is why a twelve-case pass is
/// flat across a 3x load range while an eleven-minute one is not.
///
/// The factor itself is still stated rather than localised. A better instrument is to bracket
/// *each leg* and compare the two legs' own loads, which measures the drift directly instead of
/// through the whole run's endpoints; that is the next change, not this one.
pub const LOAD_MAY_MOVE: f64 = 2.0;

/// **How far the instrument's own cost may move before the figures it took are not one
/// measurement.** A factor.
///
/// **This is the check the load average cannot do, and the measurement says so in one line.** Run
/// twice inside one minute, on one binary, over the same fifteen cases, with the one-minute load
/// average reading 6.06 before the first and 6.26 before the second: at the ordinary scheduling
/// class `function_facts` read 38.8x, and under a background scheduling class
/// (`taskpolicy -b`) it read **75.5x** — our leg's total 48,271 us against 11,565 us and Metal's
/// 639 us against 298 us. Nothing about the host's load changed, and the gate the load readings
/// hold would have judged both. What did change by the same kind of factor is the instrument's own
/// self-calibration: **0.647 us a call against 0.242 us, a factor of 2.67**.
///
/// So the quantity worth a gate here is the cost of the instrument on the CPU the figures were
/// actually taken on. Measured on this M3 Max: eleven readings over nine ordinary passes today
/// span 0.216 to 0.247 us, a factor of 1.14, and the four readings the two whole-set passes
/// recorded span 0.240 to 0.275 us, a factor of 1.15. 1.5 is above every spread an undegraded host
/// gave and well under the degraded one.
pub const CALIBRATION_MAY_MOVE: f64 = 1.5;

/// **How many of the instrument's own costs a call must cost before a row is judged at all.**
///
/// Measured over the two whole-set passes this tree recorded, which is the only pair of
/// independent whole-set measurements that exists: of the 41 paired rows whose smaller leg's
/// typical call cost under four instrument costs, the ratio moved a median **4.23x** between the
/// two passes and 19 of the 41 moved over 5x; of the 57 rows above that line it moved a median
/// **1.87x** and 7 of 57 moved over 5x. The line more than halves the movement, which is why it is
/// four and not a rounder number stated for tidiness.
///
/// What sits under it is not a short call being judged harshly, it is a call whose figure is a
/// reading of the clock and the scheduler: `texture_accesses` is five calls a case at a Metal
/// typical of 0.2 us against an instrument of 0.244 us, and its row read 19.3x in the morning pass
/// and 102.4x in the afternoon one. `replace_region` sits there too, at about one instrument cost
/// on Metal's leg, and its pooled row moved 7.4x to 31.3x across the two passes while the real
/// finding behind its ledger row is 265x over the 16 cases that make it. A pooled row that
/// unstable must neither add a row nor retire one.
pub const RESOLVES_AT: u64 = 4;

/// **How far the floor of one leg's own per-call figures may reach, in that leg's instrument
/// costs**, before the instrument's reading is not the cost of the figures that leg took.
///
/// **This is the check the instrument's own readings cannot do, and the one the degraded whole-set
/// pass needed.** [`RESOLVES_AT`] says a call must cost four instrument costs before its row is
/// judged. This is its dual and it holds the whole measurement: the suite makes hundreds of calls
/// that do almost nothing — a property getter, a setter — so a leg that can measure at all
/// resolves some of them **under** its own instrument cost, and the cheapest figure it produced is
/// therefore a statement about the CPU it ran on that needs no reference and no second pass.
///
/// Measured over the three whole-set passes this tree's recordings carry, re-read from them rather
/// than supposed. In both undegraded passes the cheapest paired row costs **41 ns on each leg**,
/// one tick of this chip's 24 MHz timebase, which is 0.168 and 0.171 instrument costs at 16:06 and
/// 0.174 and 0.176 at 19:57 — four readings spanning 1.05x — and 228 of 3,287 and 255 of 3,340 of
/// Metal's per-call figures sit under one instrument cost. In the degraded 07:44 pass **not one of
/// Metal's 2,979 figures is under even the resolve line**, and its cheapest row costs 1.416 us, or
/// **5.15 instrument costs**. The same trivial getters say it without a statistic:
/// `constant_data_offset` cost 41 ns on Metal's leg in both undegraded passes and 3.000 us at
/// 07:44, `sparse_tile_size` 41 ns against 3.125 us — 73x and 76x on calls that do nothing, while
/// that pass's instrument reading moved 1.17x and every rule above this one judged it.
///
/// So the bound sits between a measured 0.176 and a measured 5.15, and at one instrument cost it
/// is nearly the middle of that range in both directions: 5.7x of headroom over every undegraded
/// reading, and the pass it was written for 5.1x the wrong side of it. It is also the one value in
/// that range with a meaning rather than a position — **the cheapest call a leg measured must cost
/// less than the instrument costs to look at it**, which is true of every call the suite makes
/// for free and false of a leg that has lost its resolution.
pub const FLOOR_MAY_REACH: f64 = 1.0;

/// What the instrument itself cost on the CPU one leg ran on: one reading before that leg and one
/// after it.
///
/// **Bracketed for the same reason the load readings are**, and against a blind spot that was
/// measured rather than supposed. [`crate::times::instrument_nanos`] used to be read once, at the
/// start of each leg, and a leg is minutes long: the morning whole-set pass recorded 0.275 us for
/// Metal's leg and 0.269 us for ours — ordinary readings — while the figures that pass produced
/// are not reproducible by the binary that produced it. Re-run now, that binary reads
/// `function_facts` at 46.3x and 40.9x against the 89.5x it recorded, and its per-case figures
/// agree with today's to within 5%. A reading taken in the first five milliseconds of an
/// eleven-minute leg says what the CPU was doing then and nothing about the rest, and a
/// freshly-spawned thread is exactly the case a scheduler treats best.
#[derive(Clone, Copy, Debug, Default)]
pub struct Calibration {
    /// The reading taken immediately before this leg started, in nanoseconds a call.
    pub before: u64,
    /// The reading taken immediately after it finished.
    pub after: u64,
}

impl Calibration {
    /// The cost to compare this leg by: the mean of its two readings, or whichever one was taken.
    pub fn typical(self) -> Option<u64> {
        match (self.before, self.after) {
            (0, 0) => None,
            (0, one) | (one, 0) => Some(one),
            (before, after) => Some((before + after) / 2),
        }
    }

    /// How far the instrument's cost moved while this one leg ran, as a factor.
    fn moved(self) -> Option<f64> {
        let (before, after) = (self.before, self.after);
        if before == 0 || after == 0 {
            return None;
        }
        let (low, high) = if before < after {
            (before, after)
        } else {
            (after, before)
        };
        Some(high as f64 / low as f64)
    }
}

/// The instrument's cost on each leg, and what this chip's own recordings say it costs.
///
/// The reference is the median of the readings the selection's recordings carry for this chip and
/// this macOS build, and it is the only part of the gate that reaches outside the run. It is there
/// because a run degraded from its first instruction to its last has nothing inside its own
/// readings to compare against: all four of them agree, so only a figure from another day says
/// they are all wrong together. A recording holds that figure already
/// ([`crate::times::TimeRecord::instrument`]) and holds the chip and the build it was taken on, so
/// the reference needs no constant per host.
///
/// **It is not the catch it was written to be, and the recordings say so.** A pass overwrites this
/// chip's readings with its own, so the reference a pass faces is in practice its predecessor's —
/// and when the predecessor was degraded too, the reference drifts along with the degradation
/// instead of standing still. Re-read from the recordings as each pass found them: the degraded
/// 07:44 pass read 0.275 and 0.269 us against a reference of 0.295 us, **1.10x off at worst**,
/// while the undegraded 16:06 pass read 0.244 and 0.240 us against the 0.269 us 07:44 had just
/// written, **1.12x off**. The quantity ranks the degraded pass as the more credible of the two, so
/// no threshold on it can refuse one and keep the other. What catches such a pass from inside
/// itself is [`FLOOR_MAY_REACH`], which compares the instrument's reading against the figures that
/// instrument took rather than against another day's reading of itself.
#[derive(Clone, Copy, Debug, Default)]
pub struct Calibrations {
    pub metal: Calibration,
    pub ours: Calibration,
    /// What this chip's recordings say the instrument costs, in nanoseconds a call, where the
    /// selection carries enough of them to say.
    pub reference: Option<u64>,
}

impl Calibrations {
    /// Nothing measured: the gate then judges on what it does have, the way a host answering no
    /// load average is judged on the rest.
    pub fn unmeasured() -> Self {
        Self::default()
    }
}

/// What the host was doing while one leg ran: its one-minute load average on each side of that
/// leg, and nothing from the other leg's window.
#[derive(Clone, Copy, Debug, Default)]
pub struct LegLoad {
    /// The reading taken immediately before this leg started.
    pub before: Option<f64>,
    /// The reading taken immediately after it finished.
    pub after: Option<f64>,
}

impl LegLoad {
    /// The load to compare this leg by: the mean of its two readings, or whichever one the host
    /// answered.
    pub fn typical(self) -> Option<f64> {
        match (self.before, self.after) {
            (Some(before), Some(after)) => Some((before + after) / 2.0),
            (Some(one), None) | (None, Some(one)) => Some(one),
            (None, None) => None,
        }
    }

    /// How far the host moved while this one leg ran, as a factor.
    fn moved(self) -> Option<f64> {
        let (before, after) = (self.before?, self.after?);
        let (low, high) = if before < after {
            (before, after)
        } else {
            (after, before)
        };
        if low <= 0.0 {
            return None;
        }
        Some(high / low)
    }
}

/// Whether the gate judges a measurement taken under `shaders` with `metal` and `ours` describing
/// the host while each leg ran.
///
/// **The per-leg brackets are the point.** `times` measures Metal's leg to completion and then
/// ours, so the quantity that corrupts a ratio is the host moving *between* those two windows:
/// that divides one machine's Metal figure by another machine's figure of ours. Bracketing the
/// whole run could never see it, because the run's endpoints average over exactly the interval
/// where the damage happens. These brackets measure it directly.
///
/// Refused eight ways, each with its reason for the caller to print: a cold reading, which does
/// not separate our compile cost from first use on a new device; any reading over
/// [`QUIET_ENOUGH`]; a single leg whose own load moved by more than [`LOAD_MAY_MOVE`], so that
/// leg is not one measurement; the two legs' loads differing by more than [`LOAD_MAY_MOVE`],
/// so the ratio is of two machines; a leg whose **instrument cost** moved by more than
/// [`CALIBRATION_MAY_MOVE`] while it ran; the two legs' instrument costs differing by that much,
/// so the ratio divides one CPU's figure by another's; every reading of the instrument sitting that
/// far off what this chip's own recordings say it costs; and a leg whose own cheapest figure sits
/// more than [`FLOOR_MAY_REACH`] instrument costs up, which says the instrument's reading is not
/// the cost of the figures that leg took.
///
/// **The last rule is the one the measurement itself supplies, and the first seven needed it.** The
/// degraded 07:44 whole-set pass moved its per-call ratios 64x and **every one of the seven judged
/// it**: it ran at the lowest load of the three passes recorded (2.74 and 3.46 against 4.95 and
/// 4.36), its two instrument readings agree to 1.02x, its eight rounds are the most repeatable of
/// the three, and it sat 1.10x off a reference its own degraded predecessor had written while the
/// undegraded pass beside it sat 1.12x off. Each of those is a measurement of the *host* or of the
/// *instrument*, and a uniformly slow CPU is a quiet, steady, agreeable host. [`FLOOR_MAY_REACH`]
/// asks the one question none of them does — whether the instrument's reading is consistent with
/// the figures it took — and answers it 5.15 against 0.176.
///
/// The load readings and the calibration readings are both kept because they see different things:
/// measured on this host, a background scheduling class doubled a per-call ratio while the load
/// average held at 6.06 and 6.26, and the calibration moved 2.67x over the same pair of runs.
///
/// A host that answers no load average is judged on what it does answer, and judged outright if
/// it answers nothing: a missing reading is not evidence of a busy machine. `rows` is held to the
/// same rule — a measurement with no paired row has no floor to read, which is a selection without
/// a comparison rather than a degraded CPU.
pub fn judges(
    shaders: crate::run::Shaders,
    metal: LegLoad,
    ours: LegLoad,
    cpu: Calibrations,
    rows: &[crate::times::Paired],
) -> Result<(), String> {
    if matches!(shaders, crate::run::Shaders::Cold) {
        return Err(WHY_COLD_IS_NOT_JUDGED.to_owned());
    }
    let why_load = |whose: &str, seen: f64| {
        format!(
            "the margin is reported and not judged: this host's one-minute load average was \
             {seen:.2} while\n{whose} ran, and the gate wants it at {QUIET_ENOUGH:.1} or under. \
             Measured on this host, the ratios are\nflat to within 1.1x from a load of 8.6 to \
             26.5, and nothing above that was measured. Re-run it on a\nquieter host for a \
             verdict."
        )
    };
    for (whose, leg) in [("Metal's leg", metal), ("our leg", ours)] {
        if let Some(seen) = leg
            .before
            .into_iter()
            .chain(leg.after)
            .find(|seen| *seen > QUIET_ENOUGH)
        {
            return Err(why_load(whose, seen));
        }
        if let Some(moved) = leg.moved() {
            if moved > LOAD_MAY_MOVE {
                return Err(format!(
                    "the margin is reported and not judged: while {whose} ran, this host's \
                     one-minute load\naverage moved from {:.2} to {:.2}, a factor of {moved:.2} \
                     and more than the {LOAD_MAY_MOVE:.1}x the gate\nallows. That leg is not one \
                     measurement, so neither is the ratio. Re-run it on a host that\nstays put \
                     for a verdict.",
                    leg.before.unwrap_or_default(),
                    leg.after.unwrap_or_default()
                ));
            }
        }
    }
    // The interval that actually corrupts a ratio: between the two legs' windows.
    if let (Some(metal_load), Some(our_load)) = (metal.typical(), ours.typical()) {
        let (low, high) = if metal_load < our_load {
            (metal_load, our_load)
        } else {
            (our_load, metal_load)
        };
        if low > 0.0 && high / low > LOAD_MAY_MOVE {
            return Err(format!(
                "the margin is reported and not judged: Metal's leg ran at a load average of \
                 {metal_load:.2} and\nours at {our_load:.2}, a factor of {:.2} apart and more \
                 than the {LOAD_MAY_MOVE:.1}x the gate allows. The legs\nrun one after the \
                 other, so a ratio across that gap divides one machine's figure by another's.\n\
                 Re-run it on a host that stays put for a verdict.",
                high / low
            ));
        }
    }
    // The instrument's own cost on the CPU each leg ran on. The load average is a measurement of
    // the host and this is a measurement of the measurement, and the two do not substitute: a
    // background scheduling class moves this 2.67x and the load average not at all.
    for (whose, leg) in [("Metal's leg", cpu.metal), ("our leg", cpu.ours)] {
        if let Some(moved) = leg.moved() {
            if moved > CALIBRATION_MAY_MOVE {
                return Err(format!(
                    "the margin is reported and not judged: while {whose} ran, the instrument's \
                     own cost moved\nfrom {:.3} us a call to {:.3} us, a factor of {moved:.2} and \
                     more than the {CALIBRATION_MAY_MOVE:.1}x the gate allows. The\nfigures from \
                     the two ends of that leg were taken on CPUs of different speeds, so they are \
                     not one\nmeasurement. Re-run it where the measuring thread keeps what it \
                     started with.",
                    leg.before as f64 / 1_000.0,
                    leg.after as f64 / 1_000.0
                ));
            }
        }
    }
    if let (Some(metal_cost), Some(our_cost)) = (cpu.metal.typical(), cpu.ours.typical()) {
        let (low, high) = if metal_cost < our_cost {
            (metal_cost, our_cost)
        } else {
            (our_cost, metal_cost)
        };
        if low > 0 && high as f64 / low as f64 > CALIBRATION_MAY_MOVE {
            return Err(format!(
                "the margin is reported and not judged: the instrument cost {:.3} us a call while \
                 Metal's leg\nran and {:.3} us while ours did, a factor of {:.2} apart and more \
                 than the {CALIBRATION_MAY_MOVE:.1}x the gate\nallows. The legs run one after the \
                 other, so a ratio across that gap divides one CPU's figure by\nanother's. Re-run \
                 it on a host that stays put for a verdict.",
                metal_cost as f64 / 1_000.0,
                our_cost as f64 / 1_000.0,
                high as f64 / low as f64
            ));
        }
    }
    // A run degraded from its first instruction to its last has nothing inside it to disagree
    // with, so the reference is the only thing that can say its readings are all wrong together.
    if let Some(reference) = cpu.reference {
        if reference > 0 {
            for (whose, reading) in [
                ("before Metal's leg", cpu.metal.before),
                ("after Metal's leg", cpu.metal.after),
                ("before our leg", cpu.ours.before),
                ("after our leg", cpu.ours.after),
            ] {
                if reading == 0 {
                    continue;
                }
                let off = if reading > reference {
                    reading as f64 / reference as f64
                } else {
                    reference as f64 / reading as f64
                };
                if off > CALIBRATION_MAY_MOVE {
                    return Err(format!(
                        "the margin is reported and not judged: the instrument cost {:.3} us a \
                         call {whose},\nand this chip's own recordings say it costs {:.3} us -- a \
                         factor of {off:.2} and more than the\n{CALIBRATION_MAY_MOVE:.1}x the \
                         gate allows. Measured on this host, a background scheduling class reads \
                         2.67x\nhere and doubles a per-call ratio while the load average does not \
                         move at all, so this run's\nfigures are of a CPU the recorded ones were \
                         not taken on. Re-run it unthrottled for a verdict.",
                        reading as f64 / 1_000.0,
                        reference as f64 / 1_000.0
                    ));
                }
            }
        }
    }
    // The floor under the instrument, which is the measurement judging itself on its own figures
    // rather than on another reading of itself. Every rule above compares the instrument with a
    // load average, with its other end, with the other leg or with another day; this one compares
    // it with the calls it measured, and it is the only one the degraded pass could not satisfy.
    for (whose, floor, instrument) in [
        (
            "Metal's leg",
            floor(rows.iter().map(|row| row.metal.typical)),
            cpu.metal.typical(),
        ),
        (
            "our leg",
            floor(rows.iter().map(|row| row.ours.typical)),
            cpu.ours.typical(),
        ),
    ] {
        let (Some(floor), Some(instrument)) = (floor, instrument.filter(|cost| *cost > 0)) else {
            continue;
        };
        let reached = floor as f64 / instrument as f64;
        if reached > FLOOR_MAY_REACH {
            return Err(format!(
                "the margin is reported and not judged: the cheapest call {whose} measured cost \
                 {:.3} us,\nand the instrument says it cost {:.3} us a call to measure one -- a \
                 floor {reached:.2} instrument costs up,\nwhere the gate allows \
                 {FLOOR_MAY_REACH:.1}. The suite makes hundreds of calls that do almost nothing, \
                 so a leg\nthat is measuring at all resolves some of them under its own \
                 instrument: measured on this host,\nboth undegraded whole-set passes read a floor \
                 of 0.17 instrument costs -- one tick of the clock --\nwhile the pass this rule \
                 was written for read 5.15 and moved its ratios 64x with its instrument\nreading \
                 1.17x. These figures and that instrument are not of one CPU. Re-run it where the \
                 measuring\nthread keeps what it started with.",
                floor as f64 / 1_000.0,
                instrument as f64 / 1_000.0
            ));
        }
    }
    Ok(())
}

/// The floor under one leg's measurement: the cheapest figure it produced for any paired row.
///
/// A row the leg made no call for reads zero and is not a cheap call, so it is left out rather
/// than taken as a floor of nothing.
fn floor(of: impl Iterator<Item = u64>) -> Option<u64> {
    of.filter(|typical| *typical > 0).min()
}
/// One call we have written down as costing an order of magnitude more than Metal's.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct KnownSlowCall {
    /// The [`crate::case::Side`] call.
    pub call: String,
    /// The ratio the measurement that justified the row read, as its text: part of the row so a
    /// reader sees what it was when it was written, and so a row cannot be added without one.
    pub measured: String,
    /// Where the cost is, as the row's author found it: the family, and what the figure splits
    /// into.
    pub where_it_is: String,
    /// The gap it waits on: what Metal does, what we do instead, and where the fix belongs.
    pub waiting_on: String,
}

/// The committed rows.
pub fn ledger() -> Result<Vec<KnownSlowCall>, String> {
    parse(LEDGER)
}

/// `text`'s rows: tab-separated, with blank lines and lines from `#` ignored.
pub fn parse(text: &str) -> Result<Vec<KnownSlowCall>, String> {
    let mut rows = Vec::new();
    for (number, line) in text.lines().enumerate() {
        let line = line.strip_suffix('\r').unwrap_or(line);
        if line.trim().is_empty() || line.trim_start().starts_with('#') {
            continue;
        }
        let at = number + 1;
        let fields: Vec<&str> = line.split('\t').collect();
        let [call, measured, where_it_is, waiting_on] = fields.as_slice() else {
            return Err(format!(
                "known-slow-calls.tsv line {at}: {} tab-separated fields, and a row has four: the \
                 call, the ratio its measurement read, where the cost is, and the gap it waits on",
                fields.len()
            ));
        };
        if measured.trim().is_empty() {
            return Err(format!(
                "known-slow-calls.tsv line {at}: {call} names no measurement. Every bound in this \
                 suite names the measurement that justified it, and a ratio with no measurement is \
                 an opinion"
            ));
        }
        if waiting_on.trim().is_empty() {
            return Err(format!(
                "known-slow-calls.tsv line {at}: {call} names no gap. A row is an open bug, so it \
                 says what Metal does, what we do instead, and where the fix belongs"
            ));
        }
        rows.push(KnownSlowCall {
            call: call.trim().to_owned(),
            measured: measured.trim().to_owned(),
            where_it_is: where_it_is.trim().to_owned(),
            waiting_on: waiting_on.trim().to_owned(),
        });
    }
    Ok(rows)
}

/// How one paired call row stands against the margin and the ledger.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Expectation {
    /// Over the margin and listed: an open bug, reported and not failing the run.
    Known { call: String, waiting_on: String },
    /// Over the margin and **not** listed: a per-call cost nobody has written down.
    Unlisted { call: String, ratio: String },
    /// Listed, and now under [`band`]: the gap looks closed and the row must go.
    NoLongerSlow { call: String, ratio: String },
    /// Listed, between the band and the margin, where one run cannot say which side it is on.
    InTheBand { call: String, ratio: String },
    /// **The measurement cannot resolve the call above its own instrument**, so the row neither
    /// adds a ledger row nor retires one ([`RESOLVES_AT`]).
    Unresolved {
        call: String,
        ratio: String,
        /// What one such call costs on the leg that costs less, in nanoseconds.
        typical: u64,
        /// What the instrument cost on this host, in nanoseconds a call.
        instrument: u64,
    },
}

impl Expectation {
    /// Whether it fails the run.
    pub fn fails(&self) -> bool {
        matches!(
            self,
            Expectation::Unlisted { .. } | Expectation::NoLongerSlow { .. }
        )
    }

    /// Whether the gate withheld a verdict on the row rather than reaching one.
    pub fn withheld(&self) -> bool {
        matches!(self, Expectation::Unresolved { .. })
    }
}

impl std::fmt::Display for Expectation {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Expectation::Known { call, waiting_on } => write!(
                out,
                "{call} is over the margin and known-slow-calls.tsv lists it, waiting on \
                 {waiting_on}"
            ),
            Expectation::Unlisted { call, ratio } => write!(
                out,
                "{call} costs {ratio} what Metal's does and known-slow-calls.tsv does not list \
                 it: a per-call cost nobody has written down. Localise it with `times --call \
                 {call}` before adding a row"
            ),
            Expectation::NoLongerSlow { call, ratio } => write!(
                out,
                "{call} now costs {ratio} what Metal's does, inside the band, and the \
                 known-slow-calls ledger still lists it: the gap looks closed, so drop the row"
            ),
            Expectation::InTheBand { call, ratio } => write!(
                out,
                "{call} costs {ratio} what Metal's does, between the band and the margin, where \
                 one run cannot say which side it is on"
            ),
            Expectation::Unresolved {
                call,
                ratio,
                typical,
                instrument,
            } => write!(
                out,
                "{call} reads {ratio}, and the run withholds a verdict on it: one such call costs \
                 {:.3} us on the cheaper leg against the instrument's own {:.3} us, under the \
                 {RESOLVES_AT} instrument costs a row is judged at. A row this thin moved a median \
                 4.23x between the two whole-set passes against 1.87x for the rest, so it neither \
                 adds a ledger row nor retires one. Localise it with `times --call {call}`",
                *typical as f64 / 1_000.0,
                *instrument as f64 / 1_000.0
            ),
        }
    }
}

/// Hold `rows` to the margin and the ledger.
///
/// A row the measurement could not compare at all is left out: its ratio is not a number, and a
/// call with no Metal figure is a coverage hole rather than a cost result.
///
/// A cold measurement never reaches here: [`judges`] turns the gate off for one, because its
/// readings do not separate our compile cost from the cost of first use on a new device.
///
/// A row the measurement cannot resolve above its own instrument is [`Expectation::Unresolved`]
/// and is held in **neither** direction: it cannot add a row and it cannot retire one. That is a
/// deliberate hole and it is the right one — the alternative is a pass reading `replace_region`'s
/// pooled row at 7.4x and failing the run to delete a ledger row whose localised finding is 265x.
pub fn held(
    rows: &[crate::times::Paired],
    ledger: &[KnownSlowCall],
    instrument: u64,
) -> Vec<Expectation> {
    let margin = crate::times::MARGIN as f64;
    let band = band();
    let mut held = Vec::new();
    for row in rows {
        let Some(ratio) = row.ratio() else {
            continue;
        };
        let listed = ledger.iter().find(|known| known.call == row.call);
        let said = format!("{ratio:.1}x");
        // The row has to be of the call before it can be of anything: a call whose cheaper leg
        // costs a couple of instrument costs is a reading of the clock and the scheduler, and the
        // measurement says how much worse such a row behaves rather than supposing it.
        let typical = row.metal.typical.min(row.ours.typical);
        if instrument > 0 && typical < RESOLVES_AT.saturating_mul(instrument) {
            held.push(Expectation::Unresolved {
                call: row.call.clone(),
                ratio: said,
                typical,
                instrument,
            });
            continue;
        }
        held.push(match (listed, ratio) {
            (Some(known), ratio) if ratio > margin => Expectation::Known {
                call: row.call.clone(),
                waiting_on: known.waiting_on.clone(),
            },
            (Some(_), ratio) if ratio < band => Expectation::NoLongerSlow {
                call: row.call.clone(),
                ratio: said,
            },
            (Some(_), _) => Expectation::InTheBand {
                call: row.call.clone(),
                ratio: said,
            },
            (None, ratio) if ratio > margin => Expectation::Unlisted {
                call: row.call.clone(),
                ratio: said,
            },
            (None, _) => continue,
        });
    }
    held
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::times::{CallTotal, Paired};

    /// What the instrument costs in the gate's tests, in nanoseconds a call: a round number near
    /// this host's measured 0.244 us.
    const INSTRUMENT: u64 = 250;

    /// A paired row at `ratio` times Metal's cost, resolved well clear of [`RESOLVES_AT`] so the
    /// margin is what the test exercises.
    fn row(call: &str, ratio: u64) -> Paired {
        resolved_at(call, ratio, INSTRUMENT * RESOLVES_AT * 10)
    }

    /// A paired row at `ratio` times Metal's cost whose cheaper leg's typical call costs `typical`
    /// nanoseconds.
    fn resolved_at(call: &str, ratio: u64, typical: u64) -> Paired {
        Paired {
            call: call.to_owned(),
            metal: CallTotal {
                calls: 1,
                wall: 100,
                on_cpu: 100,
                blocked: 0,
                typical,
            },
            ours: CallTotal {
                calls: 1,
                wall: 100 * ratio,
                on_cpu: 100 * ratio,
                blocked: 0,
                typical: typical * ratio.max(1),
            },
        }
    }

    /// The committed ledger parses, and every row names its measurement and its gap.
    #[test]
    fn the_ledger_parses_and_every_row_names_its_evidence() {
        let rows = ledger().unwrap();
        assert!(!rows.is_empty(), "the ledger holds no rows");
        for known in &rows {
            assert!(!known.call.is_empty());
            assert!(
                known.measured.contains('x'),
                "{}: {}",
                known.call,
                known.measured
            );
            assert!(
                known.waiting_on.len() > 20,
                "{}: too thin a gap",
                known.call
            );
        }
    }

    /// A row with a missing measurement, a missing gap or an unknown mode is refused, so a row
    /// cannot be added without its evidence.
    #[test]
    fn a_row_without_its_evidence_is_refused() {
        assert!(
            parse("a\t\twhere\tgap").is_err(),
            "a row with no measurement parsed"
        );
        assert!(parse("a\t3x\twhere\t").is_err(), "a row with no gap parsed");
        assert!(parse("a\t3x\twhere").is_err(), "a three-field row parsed");
        assert!(parse("# just a comment\n\n").unwrap().is_empty());
    }

    /// **A cold measurement is reported and never judged**, because `--cold-shaders` also hands
    /// every case a fresh device and a reading over the margin then does not say which of the two
    /// it is. Measured on this M3 Max, `target` is 0.9x warm and 14.3x cold without compiling
    /// anything.
    /// One leg that held still at `load`.
    fn steady(load: f64) -> LegLoad {
        LegLoad {
            before: Some(load),
            after: Some(load),
        }
    }

    /// One leg whose instrument held still at `nanos` a call.
    fn calibrated(nanos: u64) -> Calibration {
        Calibration {
            before: nanos,
            after: nanos,
        }
    }

    /// The gate over a warm measurement whose instrument said nothing: the load rules alone.
    fn loads(metal: LegLoad, ours: LegLoad) -> Result<(), String> {
        judges(
            crate::run::Shaders::Warm,
            metal,
            ours,
            Calibrations::unmeasured(),
            &[],
        )
    }

    /// The gate over a warm measurement on a quiet host, judged by its instrument alone.
    ///
    /// No rows, so the floor rule has nothing to read and the instrument rules are what the test
    /// exercises.
    fn instrument(
        metal: Calibration,
        ours: Calibration,
        reference: Option<u64>,
    ) -> Result<(), String> {
        judges(
            crate::run::Shaders::Warm,
            steady(1.0),
            steady(1.0),
            Calibrations {
                metal,
                ours,
                reference,
            },
            &[],
        )
    }

    #[test]
    fn the_gate_does_not_judge_a_cold_measurement() {
        assert!(
            loads(steady(1.0), steady(1.0)).is_ok(),
            "the warm gate is off on a quiet host"
        );
        let cold = judges(
            crate::run::Shaders::Cold,
            steady(1.0),
            steady(1.0),
            Calibrations::unmeasured(),
            &[],
        )
        .expect_err(
            "a cold measurement was judged, and its readings do not separate the two causes",
        );
        assert!(
            cold.contains("--cold-shaders"),
            "the reason does not name the switch that causes it: {cold}"
        );
    }

    /// **A loaded host is reported and never judged**, whichever leg was on it. The bound is the
    /// top of the range the ratios were measured flat across, and nothing above it is claimed.
    #[test]
    fn the_gate_does_not_judge_a_loaded_host() {
        // Either leg over the bound refuses, and the reason names which one.
        let why = loads(steady(QUIET_ENOUGH + 0.01), steady(QUIET_ENOUGH))
            .expect_err("a loaded Metal leg was judged");
        assert!(
            why.contains("Metal's leg"),
            "the reason does not name the loaded leg: {why}"
        );
        assert!(
            loads(steady(QUIET_ENOUGH), steady(QUIET_ENOUGH + 0.01)).is_err(),
            "a loaded leg of ours was judged"
        );
        assert!(
            loads(steady(QUIET_ENOUGH), steady(QUIET_ENOUGH)).is_ok(),
            "the bound itself is refused, so no host can ever meet it"
        );
        assert!(
            loads(LegLoad::default(), LegLoad::default()).is_ok(),
            "a host with no load average was refused a verdict for not having one"
        );
    }

    /// **A leg that did not hold still is not one measurement.** Its own two readings disagree, so
    /// the figures it produced are of a machine that changed under it.
    #[test]
    fn the_gate_does_not_judge_a_leg_that_moved_under_itself() {
        let moved = LegLoad {
            before: Some(1.0),
            after: Some(1.0 * LOAD_MAY_MOVE + 1.0),
        };
        // **This is the case the run-level bracket could not see.** Its two readings are the
        // run's first and last -- `moved.before` is 1.0 and `steady(1.0).after` is 1.0 -- so the
        // endpoints are identical and a bracket of the whole run reports a host that never
        // changed, while the load in fact tripled inside Metal's leg.
        assert_eq!(
            moved.before,
            steady(1.0).after,
            "this test no longer pins the case the run-level bracket missed"
        );
        let why = loads(moved, steady(1.0)).expect_err("a leg whose own load moved was judged");
        assert!(
            why.contains("Metal's leg") && why.contains("moved from"),
            "the reason does not name the leg that moved: {why}"
        );
        assert!(
            loads(steady(1.0), moved).is_err(),
            "our leg moving under itself was judged"
        );
    }

    /// **The interval that actually corrupts a ratio is between the legs**, because they run one
    /// after the other: a ratio across that gap divides one machine's Metal figure by another
    /// machine's figure of ours. This is the case bracketing the whole run could never catch,
    /// since the run's endpoints average over exactly that interval. The numbers are the measured
    /// ones: a whole-set pass ran Metal's leg near 16.8 and ours near 55.9.
    #[test]
    fn the_gate_does_not_judge_a_ratio_taken_across_a_load_change() {
        let why = loads(steady(1.0), steady(LOAD_MAY_MOVE + 1.0))
            .expect_err("a ratio taken across a load change between the legs was judged");
        assert!(
            why.contains("Metal's leg ran at") && why.contains("ours at"),
            "the reason does not give both legs' loads: {why}"
        );
        // Exactly at the allowance is still one machine.
        assert!(
            loads(steady(1.0), steady(LOAD_MAY_MOVE)).is_ok(),
            "the allowance itself is refused"
        );
        // An idle host reads near zero on both legs and must not trip the ratio.
        assert!(
            loads(steady(0.0), steady(0.0)).is_ok(),
            "an idle host was refused for having no load to compare"
        );
    }

    /// **The instrument's own cost is the measurement of the measurement**, and a leg whose
    /// instrument moved while it ran did not take its figures on one CPU.
    ///
    /// This is the case the single start-of-leg reading could not see, and it is not supposed: the
    /// morning whole-set pass recorded 0.275 us for Metal's leg and 0.269 us for ours, both
    /// ordinary, and the figures it produced are not reproducible by the binary that produced
    /// them — re-run, that binary reads `function_facts` at 46.3x and 40.9x against the 89.5x it
    /// recorded.
    #[test]
    fn the_gate_does_not_judge_a_leg_whose_instrument_moved_under_it() {
        let moved = Calibration {
            before: 240,
            after: (240.0 * CALIBRATION_MAY_MOVE) as u64 + 1,
        };
        // The same blind spot the load brackets were introduced for: a bracket of the whole run
        // reads 240 at both ends and reports an instrument that never moved.
        assert_eq!(
            moved.before,
            calibrated(240).after,
            "this test no longer pins the case a run-level reading misses"
        );
        let why = instrument(moved, calibrated(240), None)
            .expect_err("a leg whose instrument moved under it was judged");
        assert!(
            why.contains("Metal's leg") && why.contains("own cost moved"),
            "the reason does not name the leg whose instrument moved: {why}"
        );
        assert!(
            instrument(calibrated(240), moved, None).is_err(),
            "our leg's instrument moving under it was judged"
        );
        assert!(
            instrument(calibrated(240), calibrated(240), None).is_ok(),
            "a steady instrument was refused"
        );
        assert!(
            instrument(Calibration::default(), Calibration::default(), None).is_ok(),
            "a run that took no calibration was refused a verdict for not having one"
        );
    }

    /// **The legs run one after the other**, so an instrument that cost one thing while Metal ran
    /// and another while we did divides one CPU's figure by another's.
    #[test]
    fn the_gate_does_not_judge_a_ratio_taken_across_two_cpus() {
        let why = instrument(
            calibrated(240),
            calibrated((240.0 * CALIBRATION_MAY_MOVE) as u64 + 1),
            None,
        )
        .expect_err("a ratio across two instrument costs was judged");
        assert!(
            why.contains("while Metal's leg") && why.contains("while ours did"),
            "the reason does not give both legs' instrument costs: {why}"
        );
        assert!(
            instrument(
                calibrated(240),
                calibrated((240.0 * CALIBRATION_MAY_MOVE) as u64),
                None
            )
            .is_ok(),
            "the allowance itself is refused"
        );
    }

    /// **A run degraded from its first instruction to its last agrees with itself**, so only a
    /// figure from another day can say its readings are all wrong together.
    ///
    /// Measured on this host, one binary and fifteen cases inside one minute: at the ordinary
    /// scheduling class the instrument read 0.242 us a call and `function_facts` read 38.8x; under
    /// a background scheduling class the instrument read 0.647 us and the same row read 75.5x. The
    /// one-minute load average was 6.06 and 6.26, so every load rule above judged both.
    #[test]
    fn the_gate_does_not_judge_a_run_whose_instrument_is_nothing_like_the_recorded_one() {
        let degraded = calibrated(647);
        let reference = Some(244);
        // Internally consistent: all four readings agree and both legs agree with each other, so
        // nothing above this rule can see it.
        assert!(
            degraded.moved().is_none() || degraded.moved() == Some(1.0),
            "the degraded run must agree with itself, or this test proves nothing"
        );
        let why = instrument(degraded, degraded, reference)
            .expect_err("a run 2.7x off this chip's recorded instrument was judged");
        assert!(
            why.contains("recordings say") && why.contains("0.647"),
            "the reason does not give the reading and the reference: {why}"
        );
        // An instrument cheaper than the recorded one by the same factor is equally not the
        // machine the reference was taken on.
        assert!(
            instrument(calibrated(100), calibrated(100), reference).is_err(),
            "a run far faster than the reference was judged"
        );
        assert!(
            instrument(calibrated(244), calibrated(244), reference).is_ok(),
            "a run at this chip's own recorded instrument was refused"
        );
        // No reference, no refusal: a selection whose recordings carry no reading is not evidence
        // of a degraded host.
        assert!(
            instrument(degraded, degraded, None).is_ok(),
            "a run was refused for the recordings having nothing to compare it with"
        );
    }

    /// **A row the measurement cannot resolve above its own instrument is held in neither
    /// direction.**
    ///
    /// Measured over the two whole-set passes this tree recorded: of the 41 paired rows whose
    /// cheaper leg's typical call cost under four instrument costs, the ratio moved a median 4.23x
    /// between the passes and 19 of 41 moved over 5x, against 1.87x and 7 of 57 above the line.
    /// `texture_accesses` is the row that names it — five calls a case at a Metal typical of 0.2 us
    /// against an instrument of 0.244 us, reading 19.3x in one pass and 102.4x in the other.
    #[test]
    fn a_row_the_instrument_swamps_neither_adds_a_ledger_row_nor_retires_one() {
        let thin = resolved_at("thin", 100, INSTRUMENT * RESOLVES_AT - 1);
        let unlisted = held(std::slice::from_ref(&thin), &[], INSTRUMENT);
        assert!(
            matches!(&unlisted[0], Expectation::Unresolved { .. }),
            "{unlisted:?}"
        );
        assert!(
            !unlisted[0].fails(),
            "an unresolvable row at 100x failed the run and would have demanded a ledger row \
             nobody can reproduce"
        );
        assert!(unlisted[0].withheld());
        // Listed and unresolvable: the row is neither confirmed nor retired. `replace_region`
        // lives here -- about one instrument cost on Metal's leg, a pooled row that moved 7.4x to
        // 31.3x across the two passes, and a localised finding of 265x over the 16 cases that make
        // it.
        let known = vec![KnownSlowCall {
            call: "thin".to_owned(),
            measured: "265.0x".to_owned(),
            where_it_is: "somewhere".to_owned(),
            waiting_on: "a gap written down at length somewhere".to_owned(),
        }];
        let fixed = held(
            &[resolved_at("thin", 1, INSTRUMENT * RESOLVES_AT - 1)],
            &known,
            INSTRUMENT,
        );
        assert!(
            matches!(&fixed[0], Expectation::Unresolved { .. }),
            "{fixed:?}"
        );
        assert!(
            !fixed[0].fails(),
            "an unresolvable row read as fixed and would have deleted a live bug's ledger row"
        );
        // One instrument cost above the line, the same row is judged.
        let resolved = held(
            &[resolved_at("thin", 100, INSTRUMENT * RESOLVES_AT)],
            &[],
            INSTRUMENT,
        );
        assert!(
            matches!(&resolved[0], Expectation::Unlisted { .. }),
            "{resolved:?}"
        );
        assert!(resolved[0].fails());
    }

    /// **Both directions.** An unlisted call over the margin fails; a listed call back inside the
    /// band fails too, so a row cannot outlive its bug.
    #[test]
    fn the_gate_is_held_in_both_directions() {
        let known = vec![KnownSlowCall {
            call: "listed".to_owned(),
            measured: "100.0x".to_owned(),
            where_it_is: "somewhere".to_owned(),
            waiting_on: "a gap written down at length somewhere".to_owned(),
        }];
        let over = held(
            &[row("listed", 100), row("unlisted", 50)],
            &known,
            INSTRUMENT,
        );
        assert!(matches!(&over[0], Expectation::Known { .. }), "{over:?}");
        assert!(matches!(&over[1], Expectation::Unlisted { .. }), "{over:?}");
        assert!(!over[0].fails());
        assert!(
            over[1].fails(),
            "an unlisted call over the margin did not fail the run"
        );

        // The listed call, fixed: now inside the band, which the measured movement of a per-call
        // ratio puts at 1.43x ([`BAND_DIVISOR`]).
        let fixed = held(&[row("listed", 1)], &known, INSTRUMENT);
        assert!(
            matches!(&fixed[0], Expectation::NoLongerSlow { .. }),
            "{fixed:?}"
        );
        assert!(
            fixed[0].fails(),
            "a ledger row whose gap closed did not fail the run, so the row could outlive its bug"
        );
    }

    /// One whole-set pass as its recordings carry it: what the instrument read on each leg, the
    /// cheapest figure each leg produced, and the one-minute load each leg ran at.
    struct Pass {
        what: &'static str,
        metal_instrument: u64,
        ours_instrument: u64,
        metal_floor: u64,
        ours_floor: u64,
        metal_load: f64,
        ours_load: f64,
        /// What this chip's recordings said the instrument cost when the pass was judged, which is
        /// in practice its predecessor's reading ([`Calibrations`]).
        reference: u64,
    }

    /// The three whole-set passes this tree's recordings carry, re-read from them rather than
    /// restated: the figures are each pass's own stored per-leg `instrument`, the minimum
    /// `typical` over its paired rows, and the loads its `measured` words name.
    ///
    /// The per-leg calibration **brackets** are not stored — a recording keeps only their mean —
    /// so each leg's two ends are given that mean, which is the reading most favourable to the
    /// pass and makes the refusal below a conservative one.
    const PASSES: [Pass; 3] = [
        Pass {
            what: "the degraded 07:44 pass, whose ratios move 64x",
            metal_instrument: 275,
            ours_instrument: 269,
            metal_floor: 1416,
            ours_floor: 41,
            metal_load: 2.74,
            ours_load: 3.46,
            reference: 295,
        },
        Pass {
            what: "the undegraded 16:06 pass",
            metal_instrument: 244,
            ours_instrument: 240,
            metal_floor: 41,
            ours_floor: 41,
            metal_load: 4.95,
            ours_load: 4.36,
            reference: 269,
        },
        Pass {
            what: "the undegraded 19:57 pass",
            metal_instrument: 236,
            ours_instrument: 233,
            metal_floor: 41,
            ours_floor: 41,
            metal_load: 3.18,
            ours_load: 2.63,
            reference: 240,
        },
    ];

    impl Pass {
        /// The gate's verdict on this pass, by every rule.
        fn judged(&self) -> Result<(), String> {
            judges(
                crate::run::Shaders::Warm,
                steady(self.metal_load),
                steady(self.ours_load),
                Calibrations {
                    metal: calibrated(self.metal_instrument),
                    ours: calibrated(self.ours_instrument),
                    reference: Some(self.reference),
                },
                &self.rows(),
            )
        }

        /// The gate's verdict on this pass with the floor rule's input withheld, which is what the
        /// gate read before this rule existed.
        fn judged_without_its_floor(&self) -> Result<(), String> {
            judges(
                crate::run::Shaders::Warm,
                steady(self.metal_load),
                steady(self.ours_load),
                Calibrations {
                    metal: calibrated(self.metal_instrument),
                    ours: calibrated(self.ours_instrument),
                    reference: Some(self.reference),
                },
                &[],
            )
        }

        /// One paired row per leg floor: the gate reads the minimum, so the pass's cheapest figure
        /// on each leg is the whole of what it needs.
        fn rows(&self) -> Vec<Paired> {
            let mut row = resolved_at("cheapest", 1, self.metal_floor);
            row.metal.typical = self.metal_floor;
            row.ours.typical = self.ours_floor;
            vec![row]
        }
    }

    /// **The two-way proof over all three stored whole-set passes.** The floor rule refuses the
    /// degraded one and judges both undegraded ones, and the test fails if either direction
    /// breaks.
    ///
    /// It also pins *why* the rule was needed: with the floor withheld, **every other rule judges
    /// all three passes**, the degraded one included. That pass ran at the lowest load of the
    /// three and sat closer to its reference than the undegraded pass beside it, so no threshold
    /// on what the gate read before could have separated them.
    #[test]
    fn the_floor_rule_refuses_the_degraded_pass_and_keeps_the_undegraded_ones() {
        let [degraded, ref undegraded @ ..] = PASSES;

        // The direction that must fire.
        let why = degraded
            .judged()
            .expect_err("the degraded whole-set pass was judged, and its ratios move 64x");
        assert!(
            why.contains("cheapest call Metal's leg measured") && why.contains("1.416"),
            "the reason does not name the leg and the floor it read: {why}"
        );
        assert!(
            degraded.metal_floor as f64 / degraded.metal_instrument as f64 > FLOOR_MAY_REACH,
            "this test no longer pins a pass over the bound"
        );

        // The direction that must not.
        for pass in undegraded {
            assert!(
                pass.judged().is_ok(),
                "{} was refused, and a gate that refuses a good measurement is a gate nobody \
                 runs: {}",
                pass.what,
                pass.judged().unwrap_err()
            );
        }

        // **The negative control.** Every rule but this one judges every one of the three.
        for pass in &PASSES {
            assert!(
                pass.judged_without_its_floor().is_ok(),
                "{} is refused by a rule other than the floor, so this test does not prove the \
                 floor is what separates them: {}",
                pass.what,
                pass.judged_without_its_floor().unwrap_err()
            );
        }

        // And the margin either side is stated, so a reader sees how much room there is rather
        // than taking a pass or a failure on trust.
        let reached = |pass: &Pass| {
            (pass.metal_floor as f64 / pass.metal_instrument as f64)
                .max(pass.ours_floor as f64 / pass.ours_instrument as f64)
        };
        let worst_good = undegraded.iter().map(reached).fold(0.0f64, f64::max);
        assert!(
            worst_good < FLOOR_MAY_REACH,
            "the undegraded passes reach {worst_good:.3} instrument costs, at or over the bound"
        );
        println!(
            "the degraded pass reaches {:.2} instrument costs and the two undegraded ones {:.3} at \
             worst, against a bound of {FLOOR_MAY_REACH:.1}: {:.1}x over it and {:.1}x under it",
            reached(&degraded),
            worst_good,
            reached(&degraded) / FLOOR_MAY_REACH,
            FLOOR_MAY_REACH / worst_good
        );
    }

    /// **The rule holds each leg on its own**, the way the load and instrument rules do: a ratio
    /// is corrupt when either of its halves lost its resolution, and at 07:44 only Metal's leg
    /// had.
    #[test]
    fn either_leg_losing_its_floor_refuses_the_measurement() {
        let instrument = 240;
        let under = |floor: u64| {
            let mut row = resolved_at("row", 1, INSTRUMENT * RESOLVES_AT * 10);
            row.metal.typical = floor;
            row.ours.typical = floor;
            row
        };
        let at_the_bound = (instrument as f64 * FLOOR_MAY_REACH) as u64;
        let judge = |metal_floor: u64, ours_floor: u64| {
            let mut row = under(metal_floor);
            row.ours.typical = ours_floor;
            judges(
                crate::run::Shaders::Warm,
                steady(1.0),
                steady(1.0),
                Calibrations {
                    metal: calibrated(instrument),
                    ours: calibrated(instrument),
                    reference: Some(instrument),
                },
                std::slice::from_ref(&row),
            )
        };
        let why = judge(at_the_bound + 1, at_the_bound)
            .expect_err("a Metal leg whose floor sits over its instrument was judged");
        assert!(
            why.contains("Metal's leg"),
            "the reason does not name the leg that lost its floor: {why}"
        );
        let why = judge(at_the_bound, at_the_bound + 1)
            .expect_err("our leg's floor sitting over its instrument was judged");
        assert!(
            why.contains("our leg"),
            "the reason does not name our leg: {why}"
        );
        assert!(
            judge(at_the_bound, at_the_bound).is_ok(),
            "the allowance itself is refused, so no measurement can ever meet it"
        );
        // A measurement with no row has no floor, and a selection without a comparison is not a
        // degraded CPU.
        assert!(
            judges(
                crate::run::Shaders::Warm,
                steady(1.0),
                steady(1.0),
                Calibrations {
                    metal: calibrated(instrument),
                    ours: calibrated(instrument),
                    reference: Some(instrument),
                },
                &[],
            )
            .is_ok(),
            "a measurement was refused for having no paired row to read a floor from"
        );
        // Neither does a row for a call a leg never made: a zero is not a cheap call.
        let mut absent = under(0);
        absent.ours.typical = 0;
        assert!(
            judges(
                crate::run::Shaders::Warm,
                steady(1.0),
                steady(1.0),
                Calibrations {
                    metal: calibrated(instrument),
                    ours: calibrated(instrument),
                    reference: Some(instrument),
                },
                std::slice::from_ref(&absent),
            )
            .is_ok(),
            "a row whose legs made no call was read as a floor of nothing"
        );
        // And a run that took no calibration has nothing to normalise a floor by.
        assert!(
            judges(
                crate::run::Shaders::Warm,
                steady(1.0),
                steady(1.0),
                Calibrations::unmeasured(),
                std::slice::from_ref(&under(at_the_bound * 100)),
            )
            .is_ok(),
            "a run was refused for the instrument having said nothing to compare its floor with"
        );
    }

    /// **The same rule over the committed recordings, re-derived rather than restated.** The pass
    /// the tree carries is rebuilt from the cases' own time records through the real
    /// [`crate::times::paired`], and the gate judges it.
    ///
    /// It is the half of the two-way proof that cannot drift from the tree: the constants above
    /// are a reading of three passes, and this is a reading of the pass that is actually here. It
    /// also holds the rule's premise — that the suite makes calls it can resolve under one
    /// instrument cost — so a tree whose cheap calls all left would fail here and say so, rather
    /// than leaving the gate refusing every honest measurement.
    #[test]
    fn the_committed_pass_floor_sits_under_its_own_instrument() {
        use crate::times::{CaseTimes, Leg, Measurement};

        let mut legs: Vec<(Leg, Vec<CaseTimes>, Vec<u64>)> = vec![
            (Leg::Metal, Vec::new(), Vec::new()),
            (Leg::Ours, Vec::new(), Vec::new()),
        ];
        for case in crate::cases::all() {
            let Ok(recording) = crate::recording::read(case.name()) else {
                continue;
            };
            for held in &recording.times {
                if held.chip != crate::recording::ORACLE_CHIP
                    || held.os_build != crate::recording::ORACLE_OS_BUILD
                {
                    continue;
                }
                for (leg, cases, instruments) in legs.iter_mut() {
                    if held.leg == *leg {
                        cases.push(held.times.clone());
                        if held.instrument > 0 {
                            instruments.push(held.instrument);
                        }
                    }
                }
            }
        }
        let built = |leg: Leg, cases: Vec<CaseTimes>, instrument: u64| Measurement {
            leg,
            venue: leg.label().to_owned(),
            rounds: 0,
            instrument,
            calibration: calibrated(instrument),
            cpu_clock: true,
            cases,
            calls: Vec::new(),
            noticed_calls: Vec::new(),
        };
        let mut held = legs.into_iter();
        let (metal_leg, metal_cases, mut metal_instruments) = held.next().unwrap();
        let (our_leg, our_cases, mut our_instruments) = held.next().unwrap();
        metal_instruments.sort_unstable();
        our_instruments.sort_unstable();
        let (Some(&metal_instrument), Some(&our_instrument)) = (
            metal_instruments.get((metal_instruments.len().max(1) - 1) / 2),
            our_instruments.get((our_instruments.len().max(1) - 1) / 2),
        ) else {
            panic!(
                "the recordings carry no instrument reading for {} on {}, so the gate's premise \
                 cannot be read back",
                crate::recording::ORACLE_CHIP,
                crate::recording::ORACLE_OS_BUILD
            );
        };
        let metal = built(metal_leg, metal_cases, metal_instrument);
        let ours = built(our_leg, our_cases, our_instrument);
        let rows = crate::times::paired(&metal, &ours);
        assert!(
            !rows.is_empty(),
            "the committed recordings pair no call at all, so there is no floor to read"
        );

        let metal_floor = floor(rows.iter().map(|row| row.metal.typical))
            .expect("no Metal leg figure in the committed pass");
        let our_floor = floor(rows.iter().map(|row| row.ours.typical)).expect("no figure of ours");
        let reached = (metal_floor as f64 / metal_instrument as f64)
            .max(our_floor as f64 / our_instrument as f64);
        println!(
            "the committed pass pairs {} row(s); its floor is {:.3} us on Metal's leg against a \
             {:.3} us instrument and {:.3} us on ours against {:.3} us, reaching {reached:.3} \
             instrument costs",
            rows.len(),
            metal_floor as f64 / 1_000.0,
            metal_instrument as f64 / 1_000.0,
            our_floor as f64 / 1_000.0,
            our_instrument as f64 / 1_000.0,
        );
        assert!(
            judges(
                crate::run::Shaders::Warm,
                steady(1.0),
                steady(1.0),
                Calibrations {
                    metal: calibrated(metal_instrument),
                    ours: calibrated(our_instrument),
                    reference: Some(our_instrument),
                },
                &rows,
            )
            .is_ok(),
            "the gate refuses the pass this tree carries, whose floor reaches {reached:.3} \
             instrument costs against a bound of {FLOOR_MAY_REACH:.1}. Either the measurement it \
             was recorded on was degraded, or the suite no longer makes a call it can resolve \
             under one instrument cost and the bound is measuring the wrong thing"
        );
    }

    /// A call that is *faster* than Metal's never fires the gate: it is an upper bound on our own
    /// cost, and an unexplained win is not something a gate should bless.
    #[test]
    fn being_faster_than_metal_never_fires_the_gate() {
        let quick = held(&[row("quick", 0)], &[], INSTRUMENT);
        assert!(quick.is_empty(), "{quick:?}");
    }

    /// Between the band and the margin, a listed row is neither a pass nor a failure, because one
    /// run cannot say which side it is on: Metal's own totals moved 2.43x between two whole-set
    /// runs of one binary.
    #[test]
    fn a_listed_row_in_the_band_is_neither_a_pass_nor_a_failure() {
        let known = vec![KnownSlowCall {
            call: "listed".to_owned(),
            measured: "11.0x".to_owned(),
            where_it_is: "somewhere".to_owned(),
            waiting_on: "a gap written down at length somewhere".to_owned(),
        }];
        let inside = held(&[row("listed", 3)], &known, INSTRUMENT);
        assert!(
            matches!(&inside[0], Expectation::InTheBand { .. }),
            "{inside:?}"
        );
        assert!(!inside[0].fails());
        assert!(
            band() * 6.92 <= crate::times::MARGIN as f64,
            "the band is inside the measured 6.92x movement of a resolved per-call ratio, so a \
             pass could read a listed row as fixed and delete a live bug's row"
        );
        assert!((band() - 10.0 / 7.0).abs() < 1e-9, "{}", band());
    }
}
