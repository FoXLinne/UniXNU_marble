//! A shared event listener's thread (`event.rs`): none for a listener never asked a notification,
//! started by the first, and the notifications run on it one after another in the order posted.

use crate::event::SharedEventListener;
use std::sync::mpsc;
use std::time::Duration;

/// The longest a notification may take to run.
const WAIT: Duration = Duration::from_secs(10);

/// Listeners made, cloned and dropped without a notification start no thread.
#[test]
fn a_listener_never_asked_starts_no_thread() {
    let listeners: Vec<SharedEventListener> = (0..64).map(|_| SharedEventListener::new()).collect();
    let clones = listeners.clone();
    assert!(listeners.iter().all(|listener| !listener.started()));
    drop(clones);
    assert!(listeners.iter().all(|listener| !listener.started()));
}

/// The first notification posted starts the thread; it and the later ones run there, off the
/// posting thread, one thread for them all, in the order posted.
#[test]
fn the_first_notification_starts_the_thread() {
    let listener = SharedEventListener::new();
    let (send, receive) = mpsc::channel();
    for n in 0..8 {
        let send = send.clone();
        listener.post(Box::new(move || {
            let _ = send.send((n, std::thread::current().id()));
        }));
        assert!(listener.started(), "posted {n}");
    }
    let ran: Vec<_> = (0..8)
        .map(|_| receive.recv_timeout(WAIT).unwrap())
        .collect();
    let order: Vec<u32> = ran.iter().map(|&(n, _)| n).collect();
    assert_eq!(order, (0..8).collect::<Vec<_>>());
    let thread = ran[0].1;
    assert_ne!(thread, std::thread::current().id());
    assert!(ran.iter().all(|&(_, id)| id == thread), "one thread");
}
