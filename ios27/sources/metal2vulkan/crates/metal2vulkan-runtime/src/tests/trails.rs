//! `trails.rs`: a wait for the trails below a mark waits for those opened before the mark alone,
//! and returns as the last of them closes.

use crate::trails::{Ended, Trails};
use std::sync::Arc;
use std::time::{Duration, Instant};

/// The longest a test waits for another thread.
const WAIT: Duration = Duration::from_secs(60);

/// A trail opened before the mark holds a wait at the mark; one opened after does not, and the
/// mark is the number the next trail takes.
#[test]
fn a_wait_waits_for_the_trails_opened_before_its_mark_alone() {
    let trails = Arc::new(Trails::default());
    assert_eq!(trails.mark(), 0);
    assert_eq!(
        trails.wait_below(0, Some(Instant::now())),
        Ok(false),
        "a mark of 0 waits for none"
    );
    let before = trails.open();
    let mark = trails.mark();
    assert_eq!(mark, 1);
    let after = trails.open();
    assert_eq!(trails.open_now(), 2);
    assert_eq!(
        trails.wait_below(mark, Some(Instant::now())),
        Err(Ended::TimedOut),
        "the trail opened before the mark holds the wait"
    );
    drop(before);
    assert_eq!(
        trails.wait_below(mark, Some(Instant::now())),
        Ok(false),
        "the trail opened after the mark does not"
    );
    drop(after);
    assert_eq!(trails.open_now(), 0);
}

/// A blocked wait returns once the last trail below its mark closes, on another thread.
#[test]
fn a_blocked_wait_returns_as_the_trail_closes() {
    let trails = Arc::new(Trails::default());
    let trail = trails.open();
    let mark = trails.mark();
    let waiter = {
        let trails = Arc::clone(&trails);
        std::thread::spawn(move || trails.wait_below(mark, Some(Instant::now() + WAIT)))
    };
    // The trail closes only once the waiter is genuinely inside the wait: the count rises while
    // the wait holds the registry's lock, which it gives up only to block on the trail closing.
    // A wait that found the trail already closed would never raise it, and the spin would fail.
    let since = Instant::now();
    while trails.blocked_waits() == 0 {
        assert!(
            since.elapsed() < WAIT,
            "the waiter never blocked on the open trail"
        );
        std::thread::sleep(Duration::from_micros(100));
    }
    drop(trail);
    assert_eq!(
        waiter.join().unwrap(),
        Ok(true),
        "the wait that blocked on the trail says it did"
    );
}
