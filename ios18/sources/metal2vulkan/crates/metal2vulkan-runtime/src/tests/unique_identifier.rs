//! `uniqueIdentifier`: the device's allocations of the identifiers from 2^36, and the plain count
//! past them.

use std::sync::Arc;

use crate::degradation::Degradation;
use crate::resource_ids::StateKind;
use crate::unique_identifier::{
    state_value_degradation, ScheduledWork, UniqueIdentifier, UniqueIdentifiers, BUFFERS_END,
    BUFFER_FIRST, COMPUTE_FIRST, COMPUTE_STEP, INDIRECT_BASE, PLAIN_FIRST, RENDER_STEP,
};
use crate::used_resources::EncoderKind;

fn device() -> Arc<UniqueIdentifiers> {
    Arc::default()
}

/// Indirect command buffers lie as Metal's do: buffers of one 16-KiB page take 32 KiB each from
/// `0x18000`; one given back leaves a gap the next that fits takes, and a larger one goes past the
/// live ones.
#[test]
fn buffers_take_the_lowest_gap_that_holds_them_and_a_page() {
    let ids = device();
    let page = |ids: &Arc<UniqueIdentifiers>| UniqueIdentifier::buffer(ids, 0x4000).unwrap();
    let at = |offset: u64| INDIRECT_BASE + offset;
    let [a, b, c] = [page(&ids), page(&ids), page(&ids)];
    assert_eq!(
        [a.value(), b.value(), c.value()],
        [at(0x1_8000), at(0x2_0000), at(0x2_8000)]
    );
    assert_eq!(a.value(), INDIRECT_BASE + BUFFER_FIRST);
    drop(b);
    // Two pages and one more take 48 KiB, at a multiple of 32 KiB: 64 KiB, past `c`.
    let large = UniqueIdentifier::buffer(&ids, 0x8000).unwrap();
    assert_eq!(large.value(), at(0x3_0000));
    let gap = page(&ids);
    assert_eq!(gap.value(), at(0x2_0000), "the gap `b` left");
    let past = page(&ids);
    assert_eq!(past.value(), at(0x4_0000), "past `large`");
    // A footprint past the range is refused.
    assert!(UniqueIdentifier::buffer(&ids, BUFFERS_END).is_err());
    assert!(UniqueIdentifier::buffer(&ids, u64::MAX).is_err());
}

/// States made with `supportIndirectCommandBuffers` take their kind's next slot, the last given
/// back first, as Metal's do (the consumer's measurement on an M3 Max); compute and render
/// states and buffers never meet, and plain identifiers lie past all of them.
#[test]
fn indirect_states_take_the_last_released_identifier_first() {
    let ids = device();
    let compute = || UniqueIdentifier::state(&ids, StateKind::Compute, true).unwrap();
    let render = || UniqueIdentifier::state(&ids, StateKind::Render, true).unwrap();
    let [a, b, c] = [compute(), compute(), compute()];
    assert_eq!(b.value() - a.value(), COMPUTE_STEP);
    assert_eq!(c.value() - b.value(), COMPUTE_STEP);
    assert_eq!(a.value(), INDIRECT_BASE + COMPUTE_FIRST);
    let (a_value, c_value) = (a.value(), c.value());
    drop(a);
    drop(c);
    let again = compute();
    assert_eq!(again.value(), c_value, "the last released");
    let r = [render(), render()];
    assert_eq!(r[1].value() - r[0].value(), RENDER_STEP);
    assert!(r[0].value() >= INDIRECT_BASE + BUFFERS_END && r[0].value() > c_value);
    let first = compute();
    assert_eq!(
        first.value(),
        a_value,
        "render states take another allocation"
    );
    let plain = UniqueIdentifier::state(&ids, StateKind::Compute, false).unwrap();
    let other = UniqueIdentifier::state(&device(), StateKind::Render, false).unwrap();
    assert!(plain.value() >= PLAIN_FIRST && other.value() >= PLAIN_FIRST);
    assert!(r[1].value() < PLAIN_FIRST);
    assert_ne!(plain.value(), other.value(), "one count for every device");
}

/// Compute states take Metal's region below the buffers' range, 511 of them from `0xc0`, then go
/// past the buffers' range; none meets a buffer's.
#[test]
fn compute_states_go_past_the_buffers_once_metal_s_region_is_full() {
    let ids = device();
    let fit = (BUFFER_FIRST - COMPUTE_FIRST) / COMPUTE_STEP;
    assert_eq!(fit, 511);
    let held: Vec<UniqueIdentifier> = (0..=fit)
        .map(|_| UniqueIdentifier::state(&ids, StateKind::Compute, true).unwrap())
        .collect();
    let last_in = held[fit as usize - 1].value() - INDIRECT_BASE;
    assert!(last_in + COMPUTE_STEP <= BUFFER_FIRST, "{last_in:#x}");
    assert_eq!(held[fit as usize].value(), INDIRECT_BASE + BUFFERS_END);
    let buffer = UniqueIdentifier::buffer(&ids, 0x4000).unwrap();
    assert_eq!(buffer.value(), INDIRECT_BASE + BUFFER_FIRST);
}

/// Which identifiers are measured to be Metal's number: a buffer made before Metal's own
/// allocations go past the model (a resource state encoder opened, or a render encoder's command
/// buffer scheduled); not one made after, nor any state, each with the kind its answer announces.
#[test]
fn only_buffers_made_before_unmodelled_work_are_metal_s_number() {
    let ids = device();
    let before = UniqueIdentifier::buffer(&ids, 0x4000).unwrap();
    assert_eq!(before.degradation(), None);
    ids.encoder_opened(EncoderKind::ResourceState);
    let after = UniqueIdentifier::buffer(&ids, 0x4000).unwrap();
    let unmeasured = Some(Degradation::UnmeasuredUniqueIdentifier);
    assert_eq!(after.degradation(), unmeasured);
    assert_eq!(before.degradation(), None, "made before");
    let indirect = UniqueIdentifier::state(&ids, StateKind::Render, true).unwrap();
    assert_eq!(indirect.degradation(), unmeasured);
    let plain = UniqueIdentifier::state(&device(), StateKind::Compute, false).unwrap();
    let run_to_run = Some(Degradation::PlainStateUniqueIdentifier);
    assert_eq!(plain.degradation(), run_to_run);
    // A command's answer by its value: none for no state.
    assert_eq!(state_value_degradation(0), None);
    assert_eq!(state_value_degradation(indirect.value()), unmeasured);
    assert_eq!(state_value_degradation(plain.value()), run_to_run);
}

/// Metal's encoders take their pieces of the buffers' range as an M3 Max's do (macOS 27.0.1, the
/// probes' sequences, each on a device of its own; buffers of one page, 32 KiB each): a blit
/// encoder 256 KiB at its opening, a render encoder 384 KiB, the two sharing 192 KiB of it, and
/// the first command buffer scheduled with a dispatch or a blit's command 32 KiB more, each at the
/// lowest gap that holds it, once a device. A render command buffer scheduled leaves the model.
#[test]
fn encoders_take_metal_s_pieces_of_the_buffers_range() {
    let page = |ids: &Arc<UniqueIdentifiers>| {
        UniqueIdentifier::buffer(ids, 0x4000).unwrap().value() - INDIRECT_BASE
    };
    let held = |ids: &Arc<UniqueIdentifiers>| UniqueIdentifier::buffer(ids, 0x4000).unwrap();
    // `n` more buffers, held, by their offsets.
    let pages = |ids: &Arc<UniqueIdentifiers>, n: usize| -> Vec<u64> {
        let made: Vec<UniqueIdentifier> = (0..n).map(|_| held(ids)).collect();
        made.iter().map(|id| id.value() - INDIRECT_BASE).collect()
    };
    let dispatch = ScheduledWork {
        dispatch: true,
        render: false,
    };
    // A compute encoder takes nothing at its opening, its dispatch 32 KiB as it is scheduled,
    // once.
    let ids = device();
    let first = held(&ids);
    ids.encoder_opened(EncoderKind::Compute);
    assert_eq!(page(&ids), 0x2_0000, "an open compute encoder");
    let ids = device();
    let first_too = held(&ids);
    ids.encoder_opened(EncoderKind::Compute);
    ids.scheduled(dispatch);
    ids.scheduled(dispatch);
    assert_eq!(page(&ids), 0x2_8000, "a dispatch scheduled");
    // A blit encoder 256 KiB at its opening; its copy 32 KiB more as it is scheduled.
    let ids = device();
    ids.encoder_opened(EncoderKind::Blit);
    assert_eq!(page(&ids), 0x5_8000, "an open blit encoder");
    let ids = device();
    let _first = held(&ids);
    ids.encoder_opened(EncoderKind::Blit);
    ids.scheduled(dispatch);
    assert_eq!(page(&ids), 0x6_8000, "a blit encoder's copy");
    // A dispatch and a copy share their 32 KiB, either first.
    for blit_first in [false, true] {
        let ids = device();
        if blit_first {
            ids.encoder_opened(EncoderKind::Blit);
        }
        ids.scheduled(dispatch);
        ids.encoder_opened(EncoderKind::Blit);
        ids.scheduled(dispatch);
        assert_eq!(page(&ids), 0x6_0000, "blit first: {blit_first}");
    }
    // A render encoder 384 KiB at its opening, 192 KiB shared with a blit encoder's.
    let ids = device();
    ids.encoder_opened(EncoderKind::Render);
    assert_eq!(page(&ids), 0x7_8000, "an open render encoder");
    let ids = device();
    ids.encoder_opened(EncoderKind::Blit);
    ids.encoder_opened(EncoderKind::Render);
    let after = UniqueIdentifier::buffer(&ids, 0x4000).unwrap();
    assert_eq!(
        after.value() - INDIRECT_BASE,
        0x8_8000,
        "the shared piece once"
    );
    assert_eq!(after.degradation(), None);
    ids.scheduled(ScheduledWork {
        dispatch: false,
        render: true,
    });
    let past = UniqueIdentifier::buffer(&ids, 0x4000).unwrap();
    assert_eq!(
        past.degradation(),
        Some(Degradation::UnmeasuredUniqueIdentifier),
        "a render command buffer scheduled"
    );
    // Pieces take the lowest gap that holds them: of four buffers, the second and third
    // released, a blit encoder's 64 KiB takes their gap, its 192 KiB and its copy's 32 KiB go
    // past the fourth.
    let ids = device();
    let four: Vec<UniqueIdentifier> = (0..4).map(|_| held(&ids)).collect();
    let [kept, second, third, fourth]: [UniqueIdentifier; 4] = four.try_into().unwrap();
    drop((second, third));
    ids.encoder_opened(EncoderKind::Blit);
    ids.scheduled(dispatch);
    assert_eq!(pages(&ids, 3), [0x7_0000, 0x7_8000, 0x8_0000]);
    // Of three buffers, the second released, a dispatch scheduled takes its gap.
    let ids = device();
    let three: Vec<UniqueIdentifier> = (0..3).map(|_| held(&ids)).collect();
    let [one, two, last]: [UniqueIdentifier; 3] = three.try_into().unwrap();
    drop(two);
    ids.scheduled(dispatch);
    assert_eq!(pages(&ids, 2), [0x3_0000, 0x3_8000]);
    drop((first, first_too, kept, fourth, one, last));
}
