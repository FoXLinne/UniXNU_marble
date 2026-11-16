//! The run a kill test kills (`tests/killed_scratch.rs`).
//!
//! It takes a scratch directory the way a side of the suite takes one, writes a file in it, prints
//! its path and then waits. A test can then kill it with `SIGKILL`, see what the kill left behind,
//! start another one and see that run sweep the dead directory and leave this one's alone. Nothing
//! else runs it: it reaches no device and needs no Metal.
//!
//! The wait is bounded, so a holder nobody kills exits on its own -- and then its drop removes its
//! directory, which is the other half of the property.

use metal2vulkan_conformance::archives::Scratch;
use std::io::Write;

/// How long a holder waits to be killed. Longer than any test needs and short enough that a
/// holder a failed test left behind is gone by the time anyone looks.
const WAIT: std::time::Duration = std::time::Duration::from_secs(120);

fn main() {
    let scratch = Scratch::new().expect("a scratch directory");
    std::fs::write(scratch.dir().join("0.metallib"), b"bitcode").expect("the file it holds");
    let mut out = std::io::stdout();
    writeln!(out, "{}", scratch.dir().display()).expect("the path written");
    out.flush().expect("the path flushed");
    std::thread::sleep(WAIT);
}
