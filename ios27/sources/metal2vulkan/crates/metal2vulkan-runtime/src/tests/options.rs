//! A pipeline state creation's options (`pipeline::options`): Metal's bit values, every raw bit
//! kept, and a budget whose deadline only ever comes earlier.

use crate::{PipelineOption, PipelineOptions};
use metal2vulkan::CancelToken;
use std::time::{Duration, Instant};

/// `MTLPipelineOption`'s values in `MTLDevice.h` (the macOS 26.5 and 27.0 SDKs): `None` 0,
/// `BindingInfo` (and the deprecated `ArgumentInfo`) `1 << 0`, `BufferTypeInfo` `1 << 1`,
/// `FailOnBinaryArchiveMiss` `1 << 2`.
#[test]
fn the_defined_bits_are_metals() {
    assert_eq!(PipelineOption::NONE.raw(), 0);
    assert_eq!(PipelineOption::BINDING_INFO.raw(), 1 << 0);
    assert_eq!(PipelineOption::BUFFER_TYPE_INFO.raw(), 1 << 1);
    assert_eq!(PipelineOption::FAIL_ON_BINARY_ARCHIVE_MISS.raw(), 1 << 2);
    assert_eq!(PipelineOption::default(), PipelineOption::NONE);
    assert_eq!(PipelineOptions::default().options, PipelineOption::NONE);
}

/// Metal accepts every raw value (measured on an M3 Max and an M4), so every bit is kept.
#[test]
fn undefined_bits_are_kept() {
    for raw in [8, 16, 1 << 10, 1 << 31, 1 << 40, u64::MAX] {
        assert_eq!(PipelineOption::from_raw(raw).raw(), raw);
    }
    let both = PipelineOption::FAIL_ON_BINARY_ARCHIVE_MISS | PipelineOption::from_raw(1 << 40);
    assert_eq!(both.raw(), (1 << 40) | 4);
    assert_eq!(PipelineOptions::new(both).options, both);
}

/// Metal tests the option word against zero and nothing finer: every word but `NONE` asks a
/// `_with_reflection` creation for its reflection, an undefined bit alone and
/// `FAIL_ON_BINARY_ARCHIVE_MISS` alone included (measured on an M3 Max under macOS 27.0.1 by the
/// conformance suite's `pipeline_options` family, on a compute and a render creation).
///
/// The row that first moved this is `FAIL_ON_BINARY_ARCHIVE_MISS` on its own: Apple's
/// documentation says nothing about reflection for that bit, and reading only the two information
/// bits answered no reflection where Metal answers one -- a reflection a consumer asked for and
/// did not get.
#[test]
fn every_option_word_but_none_asks_for_a_reflection() {
    assert!(!PipelineOptions::default().options.reflects());
    assert!(!PipelineOption::NONE.reflects());
    for raw in [
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        1 << 31,
        1 << 40,
        1 | (1 << 3),
        u64::MAX,
    ] {
        assert!(
            PipelineOption::from_raw(raw).reflects(),
            "the word {raw} asks for a reflection"
        );
    }
}

#[test]
fn a_deadline_only_shortens_the_standard_budget() {
    let standard = Duration::from_secs(20);
    let before = Instant::now();
    let budget = PipelineOptions::default().budget();
    let after = Instant::now();
    let deadline = budget.deadline.expect("the standard deadline");
    assert!(before + standard <= deadline && deadline <= after + standard);
    assert!(budget.cancel.is_none());

    let mut options = PipelineOptions::default();
    let soon = Instant::now() + Duration::from_secs(1);
    options.deadline = Some(soon);
    assert_eq!(
        options.budget().deadline,
        Some(soon),
        "an earlier deadline holds"
    );
    options.deadline = Some(Instant::now() + Duration::from_secs(600));
    let after = Instant::now();
    let deadline = options.budget().deadline.expect("a deadline");
    assert!(deadline <= Instant::now() + standard && deadline >= after + standard);

    let token = CancelToken::new();
    options.cancel = Some(token.clone());
    let budget = options.budget();
    token.cancel();
    assert!(
        budget.cancel.expect("the token").is_cancelled(),
        "the budget shares it"
    );
}
