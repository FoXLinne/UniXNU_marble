//! The second Metal oracle: the same case under Metal's own API validation layer.
//!
//! Plain Metal is the behavioural oracle, because that is how a consumer runs the runtime. What
//! Metal's API validation does with the same case is a **second recorded fact**, never compared
//! with our behaviour.
//!
//! # A validated stop is not Metal refusing the call
//!
//! The ruling, and the reason the rest of this module exists: Metal's API validation stopping a
//! call is **not** Metal refusing it. Plain Metal is the oracle, and the owner's standing rule is
//! that what Metal allows we must allow. So where plain Metal answers a call and we return a
//! typed error, that is a gap of ours, whatever its validation layer thinks: the row is
//! [`Verdict::Refused`](crate::verdict::Verdict::Refused), it counts in the suite's own number,
//! and it is never compliant. The suite calling such a row a pass would be the suite laundering a
//! refusal.
//!
//! What the validated fact is for is the record and the report. It may **document** a refusal
//! ([`crate::allowance::Justification::MetalValidation`]) — a refusal Metal's own validation also
//! refuses reads differently from one Metal is happy with, and the first is the better bug report
//! — and it is still held to its evidence, which is what keeps this leg load-bearing rather than
//! decorative. It buys an explanation, never a pass.
//!
//! # Why a child process, and why `nslog` rather than the default
//!
//! Metal reads `MTL_DEBUG_LAYER` as it initialises, so the layer cannot be turned on inside a
//! process that has already made a device: the plain leg and the validated leg cannot share one.
//! More importantly, **Metal's validation reports by assertion**, and its default error mode ends
//! the process. Measured on this Mac (an Apple M3 Max, macOS 27.0.1): a rate-map descriptor of no
//! layers under `MTL_DEBUG_LAYER=1` alone printed
//! `` failed assertion `RasterizationRateMap Layers Validation ...` `` and died with SIGABRT,
//! while the same descriptor under [`ERROR_MODE`] printed the same words through `NSLog` and let
//! the call run. The valid modes are `ignore`, `assert`, `nslog`, `oslog` and `exception`;
//! `nslog` is the one that reports every check without ending anything.
//!
//! So the leg runs in a child process of this same command, under [`ERROR_MODE`] and
//! [`WARNING_MODE`], and the parent reads what the child's Metal said.
//!
//! **This is settled; do not re-litigate it from the letter of the safety rule.** That rule reads
//! that a shape whose Metal leg aborts is a recorded-only row that must never be re-run, and read
//! literally it would forbid re-recording even the *plain* leg of these cases, where Metal answers
//! cleanly and safely. Its purpose is that no case of ours may end Metal's process or fault the
//! device on demand, and this arrangement serves that purpose while keeping both legs
//! re-recordable:
//!
//! - **The suite's own process never has the validation layer on**, so no assertion of Apple's can
//!   end a record run, whatever a future case asks Metal. A child that aborts cannot take the
//!   recorder with it.
//! - **The layer reports instead of asserting**, so in practice nothing aborts at all.
//! - **Nothing of the validated leg reaches the GPU** for the cases that use it: a descriptor is
//!   compiled and queried on the CPU, and no command buffer is committed. A validation complaint
//!   is a CPU-side check, so the leg cannot fault or hang the device.
//!
//! If a child dies anyway, that is reported rather than guessed at: a leg that produced no
//! complaint and no clean exit is [`LegError::Died`], and record mode refuses the case instead of
//! writing a [`Validated`] value nobody measured. There is deliberately no `Validated` variant for
//! "taken and died": refusing the case is the honest answer, and a variant nothing constructs
//! would be dead code.
//!
//! # How a complaint is attributed to a call
//!
//! The child announces each [`Side`](crate::case::Side) call it is about to make on its standard
//! error ([`MARKER`]), so Metal's own output and our markers interleave in one stream in the order
//! they happened. Everything between two markers is the complaint of the call the first marker
//! names. Nothing is matched against Apple's wording, so a new check in a release the suite has
//! not seen is still attributed and still recorded.
//!
//! The message a recording keeps is stripped of `NSLog`'s timestamp and process prefix, because a
//! recording holds nothing host-specific: the prefix carries this run's process id. The same rule
//! takes out the **addresses** Metal prints — a complaint names the object it is about by its
//! pointer (`<AGXG15XFamilyBuffer: 0x7b81438e00>`), which is this run's heap and moves under ASLR
//! on every run, so it is host-specific in the strongest sense. A recording holding one could
//! never be re-recorded byte for byte, and a re-record that changes nothing is how a fingerprint
//! refresh is told from a re-measurement.

use crate::recording::Validated;

/// `MTL_DEBUG_LAYER`: Metal's API validation layer.
pub const DEBUG_LAYER: (&str, &str) = ("MTL_DEBUG_LAYER", "1");

/// `MTL_DEBUG_LAYER_ERROR_MODE`: how the layer reports an error. `nslog` is the one mode that
/// reports every check without ending the process; the default, `assert`, aborts.
pub const ERROR_MODE: (&str, &str) = ("MTL_DEBUG_LAYER_ERROR_MODE", "nslog");

/// `MTL_DEBUG_LAYER_WARNING_MODE`: the same for a warning, so a warning is recorded too rather
/// than dropped.
pub const WARNING_MODE: (&str, &str) = ("MTL_DEBUG_LAYER_WARNING_MODE", "nslog");

/// The verb the child runs: it takes one case's validated leg and says nothing on its standard
/// output. It is not part of the command's documented surface; the parent spawns it.
pub const LEG_VERB: &str = "validated-leg";

/// What the child prints before each call it makes, so the parent can attribute a complaint to the
/// call it belongs to.
pub const MARKER: &str = "[conformance] call ";

/// The line Metal prints as the layer comes up, which is not a complaint about anything.
const BANNER: &str = "Metal API Validation";

/// Why the validated leg produced no answer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LegError {
    /// The child could not be started.
    NotStarted(String),
    /// The child ended other than cleanly and said nothing a complaint could be read out of, so
    /// what Metal's validation did is unknown rather than `Ran`.
    Died { status: String, output: String },
}

impl std::fmt::Display for LegError {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LegError::NotStarted(why) => {
                write!(out, "the validated leg could not be started: {why}")
            }
            LegError::Died { status, output } => write!(
                out,
                "the validated leg ended {status} with no validation message, so what Metal's \
                 API validation did is unknown:\n{output}"
            ),
        }
    }
}

/// What Metal's API validation did with `case`, taken by running the case in a child of this
/// command with the layer on.
#[cfg(target_os = "macos")]
pub fn take(case: &dyn crate::case::Case) -> Result<Validated, LegError> {
    let program = std::env::current_exe()
        .map_err(|error| LegError::NotStarted(format!("this command's own path: {error}")))?;
    let output = std::process::Command::new(&program)
        .arg(LEG_VERB)
        .arg(case.name())
        .env(DEBUG_LAYER.0, DEBUG_LAYER.1)
        .env(ERROR_MODE.0, ERROR_MODE.1)
        .env(WARNING_MODE.0, WARNING_MODE.1)
        .output()
        .map_err(|error| LegError::NotStarted(format!("{}: {error}", program.display())))?;
    let said = String::from_utf8_lossy(&output.stderr).into_owned();
    match read(&said) {
        Some(validated) => Ok(validated),
        None if output.status.success() => Ok(Validated::Ran),
        None => Err(LegError::Died {
            status: format!("{}", output.status),
            output: said,
        }),
    }
}

/// The first complaint `said` holds, as the recording keeps it: the call the marker before it
/// named, and the words Metal used with the host's own prefix stripped.
///
/// `None` where the layer complained about nothing, which is [`Validated::Ran`] for a child that
/// also finished.
pub fn read(said: &str) -> Option<Validated> {
    let mut call = String::new();
    let mut complaint: Vec<String> = Vec::new();
    for line in said.lines() {
        if let Some(named) = line.strip_prefix(MARKER) {
            if !complaint.is_empty() {
                break;
            }
            call = named.trim().to_owned();
            continue;
        }
        let text = strip_prefix(line);
        if text.is_empty() || text.starts_with(BANNER) {
            continue;
        }
        complaint.push(strip_addresses(text));
    }
    (!complaint.is_empty()).then(|| Validated::Stopped {
        call: if call.is_empty() {
            "before the first call".to_owned()
        } else {
            call
        },
        message: complaint.join("\n"),
    })
}

/// `line` without `NSLog`'s `<date> <time> <program>[<pid>:<tid>] ` prefix, which names this host's
/// process and must not reach a recording. A line without one is its own text.
fn strip_prefix(line: &str) -> &str {
    let Some((head, rest)) = line.split_once("] ") else {
        return line.trim_end();
    };
    if head.contains('[')
        && head
            .chars()
            .next()
            .is_some_and(|first| first.is_ascii_digit())
    {
        return rest.trim_end();
    }
    line.trim_end()
}

/// What a recording keeps in place of an address Metal printed.
///
/// Normalised and **not** removed, so the complaint still reads as the sentence Apple wrote and a
/// reader can see that a pointer was there.
const ADDRESS: &str = "0xADDR";

/// The hexadecimal digits that make a `0x` number an address rather than a constant Metal quoted.
///
/// Six, which is the threshold the suite-wide guard `no_recorded_message_holds_an_address` holds
/// the committed recordings to, so the two cannot drift. An arm64 pointer prints ten or eleven digits;
/// a shorter `0x` number in a complaint is left as the number it is.
const ADDRESS_DIGITS: usize = 6;

/// `text` with every address Metal printed replaced by [`ADDRESS`].
///
/// The reason is the module's: an address is this run's heap, so it must not reach a recording, for
/// the same reason `NSLog`'s process prefix must not.
fn strip_addresses(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = String::with_capacity(text.len());
    let mut at = 0;
    while at < bytes.len() {
        // `0x` or `0X` and then enough hexadecimal digits. Only ASCII is stepped over here, so
        // `at` stays on a character boundary for the branch below.
        if bytes[at] == b'0' && at + 1 < bytes.len() && bytes[at + 1] | 0x20 == b'x' {
            let digits = bytes[at + 2..]
                .iter()
                .take_while(|byte| byte.is_ascii_hexdigit())
                .count();
            if digits >= ADDRESS_DIGITS {
                out.push_str(ADDRESS);
                at += 2 + digits;
                continue;
            }
        }
        let next = text[at..]
            .chars()
            .next()
            .expect("at is a character boundary");
        out.push(next);
        at += next.len_utf8();
    }
    out
}

/// A side that announces each call it is about to make, so the parent can attribute Metal's own
/// output to the call that provoked it. It observes nothing and changes nothing else.
///
/// It is [`crate::watch::Watching`] with this watcher: the forwarding lives there, in one impl,
/// shared with the timing wrapper, because two hand-written copies of it would be two places to
/// forget a method that `Side` gives a default to.
pub type Announcing<S> = crate::watch::Watching<S, Announce>;

/// Announce each call on standard error, and nothing else.
#[derive(Clone, Copy, Debug, Default)]
pub struct Announce;

impl crate::watch::Watcher for Announce {
    /// Nothing to do when the call returns: the marker is the whole mechanism, and a marker on
    /// the way out would double every line the parent has to read.
    type Span = ();

    fn enter(&self, call: &'static str) {
        eprintln!("{MARKER}{call}");
    }
}
