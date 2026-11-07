//! The device's room keeper: one thread of the device's that keeps room in the pools its encodings
//! and commits place their own resources in (`memory::keep_room`), so the creation call that asks
//! for room returns at once.
//!
//! - **Asked by the creation calls.** [`Room::ask`] marks that room is wanted and wakes the
//!   thread, which it starts unless it runs; asks that come while the thread works are one more
//!   pass, not one each.
//! - **Why a thread.** Making a block is a `vkAllocateMemory` that a driver may make wait: on an
//!   iPadOS guest whose GPU takes the system's pages, each 64 MiB block was wired page by page as it
//!   was made, 30-110 ms in the texture creation that asked for it (Core Animation's surfaces,
//!   the widget renderer's textures). A placement that finds no room before the block is there
//!   makes its own, and the keeper's block then goes at once (`memory::keep_room`).
//! - **Holds nothing of the device.** The thread holds a weak handle only while it waits, and the
//!   device itself only for a pass. Where that pass held its last handle, the teardown runs here,
//!   and its [`Room::stop`] does not join this thread.

use super::{lock, Device, WeakDevice};
use crate::exit_guard;
use std::sync::{Arc, Condvar, Mutex, PoisonError};
use std::thread::JoinHandle;

/// The device's room keeper: its thread, once a creation call started it.
#[derive(Default)]
pub(crate) struct Room {
    shared: Arc<Shared>,
    thread: Mutex<Option<JoinHandle<()>>>,
}

#[derive(Default)]
struct Shared {
    state: Mutex<State>,
    /// Room was asked for, or the thread is asked to stop.
    work: Condvar,
}

#[derive(Default)]
struct State {
    asked: bool,
    stopping: bool,
}

impl Room {
    /// Ask for room in `device`'s pools: the thread keeps it, started unless it runs. Where the
    /// host cannot start a thread, the room is kept here, as before there was one.
    pub(crate) fn ask(&self, device: &Device) {
        {
            let mut state = lock(&self.shared.state);
            if state.stopping {
                return;
            }
            state.asked = true;
        }
        self.shared.work.notify_all();
        let mut thread = lock(&self.thread);
        if thread.is_some() {
            return;
        }
        let shared = Arc::clone(&self.shared);
        let weak = device.downgrade();
        match exit_guard::spawn(
            std::thread::Builder::new().name("metal2vulkan-room".into()),
            move || run(&shared, &weak),
        ) {
            Ok(handle) => *thread = Some(handle),
            Err(_) => {
                drop(thread);
                lock(&self.shared.state).asked = false;
                keep(device);
            }
        }
    }

    /// The device's teardown: the thread ends, and is joined unless this is it.
    pub(crate) fn stop(&self) {
        lock(&self.shared.state).stopping = true;
        self.shared.work.notify_all();
        let mut thread = lock(&self.thread);
        let own = thread
            .as_ref()
            .is_some_and(|handle| handle.thread().id() == std::thread::current().id());
        if !own {
            if let Some(handle) = thread.take() {
                let _ = handle.join();
            }
        }
    }
}

/// One pass: room kept in `device`'s pools, in a section of the exit guard.
fn keep(device: &Device) {
    let Some(_busy) = exit_guard::enter() else {
        return;
    };
    let shared = device.shared();
    crate::memory::age_spares(&shared.memory, shared.vk());
    crate::memory::keep_room(&shared.memory, shared.vk(), &shared.counts);
}

/// The thread's loop: a pass for each time room was asked since the last, until stopped.
fn run(shared: &Shared, weak: &WeakDevice) {
    loop {
        let requested = {
            let mut state = lock(&shared.state);
            while !state.asked && !state.stopping {
                let (next, timeout) = shared
                    .work
                    .wait_timeout(state, std::time::Duration::from_millis(250))
                    .unwrap_or_else(PoisonError::into_inner);
                state = next;
                if timeout.timed_out() {
                    break;
                }
            }
            if state.stopping {
                return;
            }
            let requested = state.asked;
            state.asked = false;
            requested
        };
        let Some(device) = weak.upgrade() else {
            return;
        };
        if requested {
            keep(&device);
        } else {
            let parts = device.shared();
            crate::memory::age_spares(&parts.memory, parts.vk());
        }
        // Outside the state's lock: it may be the device's last handle, whose teardown stops
        // this thread.
        drop(device);
    }
}
