//! The process's allocator, counting each thread's allocations while [`counting`] runs: the
//! calls workload's `*_allocs` metrics. Outside it each allocation pays one relaxed load more
//! than the system allocator's own work.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::sync::atomic::{AtomicBool, Ordering};

/// Allocations are counted.
static ON: AtomicBool = AtomicBool::new(false);

struct Counting;

#[global_allocator]
static ALLOCATOR: Counting = Counting;

thread_local! {
    /// Allocations (and reallocations) this thread made.
    static MADE: Cell<u64> = const { Cell::new(0) };
}

fn note() {
    if ON.load(Ordering::Relaxed) {
        // A thread being torn down no longer counts.
        let _ = MADE.try_with(|made| made.set(made.get() + 1));
    }
}

/// Run `f` with allocations counted.
pub fn counting<T>(f: impl FnOnce() -> T) -> T {
    ON.store(true, Ordering::Relaxed);
    let out = f();
    ON.store(false, Ordering::Relaxed);
    out
}

/// The allocations this thread made so far.
pub fn this_thread() -> u64 {
    MADE.try_with(Cell::get).unwrap_or(0)
}

// SAFETY: every call goes to the system allocator as it came.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        note();
        // SAFETY: the caller's contract, passed on.
        unsafe { System.alloc(layout) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        note();
        // SAFETY: as `alloc`.
        unsafe { System.alloc_zeroed(layout) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // SAFETY: as `alloc`.
        unsafe { System.dealloc(ptr, layout) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        note();
        // SAFETY: as `alloc`.
        unsafe { System.realloc(ptr, layout, new_size) }
    }
}
