//! The three clocks the per-call cost table reads, and what each one is good for.
//!
//! The suite times both legs at the same boundary ([`crate::watch`]), so the clocks must be cheap
//! enough that reading them does not dominate a 2 us call, and fine enough that a 2 us call is not
//! quantised away. Those two demands are what decided the attribution split, so the measurements
//! are written down here rather than left as a feeling. Taken on this Apple M3 Max, macOS 27.0.1
//! (26A434), with an optimised C loop of 200,000 reads each:
//!
//! | clock | read | smallest step it reports |
//! |---|---:|---:|
//! | `CLOCK_MONOTONIC` (through [`std::time::Instant`]) | 16 ns | nanoseconds |
//! | `CLOCK_THREAD_CPUTIME_ID` | 95 ns | 125 ns |
//! | `CLOCK_PROCESS_CPUTIME_ID` | 230 ns | **1000 ns** |
//!
//! So the first two are read **per call** and the third is read **per case**. A microsecond step is
//! most of a short call: reading the process clock around a 2 us `setBytes:` would answer 0 or
//! 1 us, and a median of those is a number about the clock rather than about the call. Over a whole
//! case — thousands of calls — the same clock is accurate to a part in a thousand.
//!
//! That is why "our own threads' time" is a per-case attribution and the other two are per-call.
//! It is a property of the instrument, not a choice about what matters.
//!
//! # The instrument's own cost is reported, never subtracted
//!
//! Two reads at each end of a call come to about 220 ns, which is real against a Metal call of a
//! couple of microseconds. It is paid **identically on both legs**, because one wrapper times both,
//! so a ratio carries it in numerator and denominator alike. The `times` command measures the span
//! machinery's cost on the host it runs on and prints it beside the table
//! ([`crate::times::instrument_nanos`]). It is never subtracted from a figure: a measured overhead
//! a reader can see is honest, and an estimate quietly removed from every row is not.

/// The calling thread's CPU time, user and system, in nanoseconds, or `None` where the host does
/// not answer the clock.
///
/// This is the thread the case runs on, so a call's reading is what **the caller** burned. The
/// difference between a call's elapsed time and this is the time the caller was not computing: it
/// was waiting.
pub fn thread_cpu_nanos() -> Option<u64> {
    read(libc::CLOCK_THREAD_CPUTIME_ID)
}

/// The whole process's CPU time, every thread of it, in nanoseconds, or `None` where the host does
/// not answer the clock.
///
/// Read once at each end of a case, never per call: its step is a microsecond
/// ([module docs](self)). Minus the calling thread's own, it is the CPU the implementation spent
/// **off** the calling thread — the handoff the plan says must not read as a win.
///
/// It is the whole process, so on our leg it covers our runtime's threads *and* the driver's, and
/// on the Metal leg it covers Metal's own. That is the honest denominator: the suite compares two
/// whole stacks and has no way to cut one into layers ([`crate::times`]).
pub fn process_cpu_nanos() -> Option<u64> {
    read(libc::CLOCK_PROCESS_CPUTIME_ID)
}

/// One POSIX clock, in nanoseconds. `None` where the call failed, which is how a host that does
/// not answer a clock reaches the report as a missing attribution rather than as a zero.
fn read(clock: libc::clockid_t) -> Option<u64> {
    let mut when = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: `clock_gettime` writes one `timespec`, which is what it is given, and reads nothing
    // else. A clock the host does not know answers -1 and leaves the value alone.
    let answered = unsafe { libc::clock_gettime(clock, &mut when) };
    if answered != 0 {
        return None;
    }
    let seconds = u64::try_from(when.tv_sec).ok()?;
    let nanos = u64::try_from(when.tv_nsec).ok()?;
    seconds.checked_mul(1_000_000_000)?.checked_add(nanos)
}

/// The host's one-minute load average, or `None` where the host does not answer it.
///
/// **Why a timing report reads this.** The two legs are not loaded alike: ours spends its cost on
/// the calling thread's CPU where Metal's largest calls sit blocked on the GPU, so host load does
/// not cancel in a ratio -- it moves ours and leaves Metal's. Measured on this M3 Max over one
/// identical case set, our `render_pipeline` total went 220 ms to 999 ms (4.5x) between a quiet
/// run and one taken against four `rustc` and another suite's binary, while Metal's went 8.1 ms
/// to 13.3 ms (1.6x), and `replace_region`, which is four fifths blocked rather than CPU, moved
/// the *other* way, 60.9x to 12.1x. A ratio read on a busy host is not a ratio of the two stacks.
pub fn host_load() -> Option<f64> {
    let mut averages = [0.0f64; 3];
    // SAFETY: `getloadavg` writes at most the number of entries it is given into the pointer it
    // is given, and reads nothing else. It answers -1 where the host keeps no load average, and
    // the count it answers is how many entries it actually wrote.
    let written = unsafe { libc::getloadavg(averages.as_mut_ptr(), 3) };
    if written < 1 {
        return None;
    }
    Some(averages[0])
}
