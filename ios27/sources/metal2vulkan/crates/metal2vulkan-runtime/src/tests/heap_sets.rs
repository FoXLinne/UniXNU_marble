//! The device's heap (`heap_sets.rs`, residency part C): an image retired from it waits for the
//! command buffers that may read it, those submitted before it went (committed, for what a rebind
//! or a detach replaced); a sampler table's two windows map the IDs of a plain device and of one
//! past a reservation, and keep the table sorted.

use crate::heap_sets::{high_window_base, sampler_entry, Retirement};

/// With no command buffer out, a retired image goes at once.
#[test]
fn an_image_retired_with_no_command_buffer_out_goes_at_once() {
    let mut retirement = Retirement::default();
    assert_eq!(retirement.retire('a'), Some('a'));
}

/// An image waits for every ticket taken before it was retired, and for none taken after.
#[test]
fn an_image_waits_for_the_command_buffers_committed_before_it_went() {
    let mut retirement = Retirement::default();
    let one = retirement.take();
    assert_eq!(retirement.retire('a'), None, "one may read it");
    let two = retirement.take();
    assert_eq!(retirement.retire('b'), None, "one and two may read it");
    let three = retirement.take();
    assert_eq!(
        retirement.give(two),
        Vec::<char>::new(),
        "one may still read both"
    );
    assert_eq!(
        retirement.give(one),
        ['a', 'b'],
        "three was committed after both went"
    );
    assert_eq!(retirement.retire('c'), None, "three may read it");
    assert_eq!(retirement.give(three), ['c']);
    assert_eq!(retirement.retire('d'), Some('d'), "no ticket is out");
}

/// Two tickets of one epoch: the image waits for both.
#[test]
fn tickets_of_one_epoch_are_counted() {
    let mut retirement = Retirement::default();
    let first = retirement.take();
    let second = retirement.take();
    assert_eq!(first, second);
    assert_eq!(retirement.retire('a'), None);
    assert_eq!(retirement.give(first), Vec::<char>::new());
    assert_eq!(retirement.give(second), ['a']);
}

/// The low window maps sampler IDs 0 to 32767 to their own entries, whatever the high window; an
/// ID past it maps into the high window from its base, 32768 on (none taken: IDs 32768 to 65535),
/// and an ID in neither window maps nowhere.
#[test]
fn sampler_ids_map_into_the_table_s_two_windows() {
    for high in [None, Some(32768), Some(98304)] {
        assert_eq!(sampler_entry(0, high), Some(0));
        assert_eq!(sampler_entry(16380, high), Some(16380));
        assert_eq!(sampler_entry(32767, high), Some(32767));
    }
    assert_eq!(sampler_entry(32768, None), Some(32768));
    assert_eq!(sampler_entry(65535, None), Some(65535));
    assert_eq!(sampler_entry(65536, None), None);
    assert_eq!(sampler_entry(98303, Some(98304)), None);
    assert_eq!(sampler_entry(98304, Some(98304)), Some(32768));
    assert_eq!(sampler_entry(100001, Some(98304)), Some(32768 + 1697));
    assert_eq!(sampler_entry(131071, Some(98304)), Some(65535));
    assert_eq!(sampler_entry(131072, Some(98304)), None);
    assert_eq!(sampler_entry(40000, Some(98304)), None);
}

/// The first sampler ID past the low window opens the high window: within one window of it, at
/// 32768 (the IDs a plain device hands out past 32767 live holders); past that, at least 1024 and
/// less than 2048 below it, on a multiple of 1024, so the IDs just below a reservation's first
/// plain one, which other holders took first, lie in it, and the window's IDs stay above the low
/// window's.
#[test]
fn the_high_window_opens_below_the_first_id_past_the_low_one() {
    assert_eq!(high_window_base(32768), 32768);
    assert_eq!(high_window_base(65535), 32768);
    assert_eq!(high_window_base(65536), 64512);
    assert_eq!(high_window_base(100001), 98304);
    assert_eq!(high_window_base(20001 + 65536), 83968);
    for id in [65536, 65537, 100001, 1 << 20, (1 << 28) - 1] {
        let base = high_window_base(id);
        assert!(base >= 32768 && base.is_multiple_of(1024), "{id}: {base}");
        assert!(id - base >= 1024 && id - base < 2048, "{id}: {base}");
        assert_eq!(sampler_entry(id, Some(base)), Some(32768 + (id - base)));
    }
}
