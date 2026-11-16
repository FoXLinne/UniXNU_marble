//! Record mode: take a case's Metal answer and write its recording.
//!
//! Record mode runs only where Metal does. It refuses, before anything is encoded, a case whose
//! Metal leg is not safe to run ([`crate::case::ReRecord::Forbidden`]): those cases carry an answer
//! already written down, and a re-record of a whole selection must not walk into one and fault the
//! GPU or end the process.
//!
//! It also refuses to write a recording on a host that is not the oracle the suite names. A
//! recording is keyed by chip, macOS release and macOS build, and a file written under one build
//! and compared under another proves nothing, so the host is checked at the one moment it can be:
//! as the answer is taken.

use crate::case::{Case, Side};
use crate::recording::{self, Answer, Outcome, Provenance, Recording, Validated};
use crate::replay::NOTICES_SITE;
use crate::transcript::{Sites, Transcript};

/// Why an answer could not be recorded.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RecordError {
    /// The case forbids re-recording: running its Metal leg is not safe. The reason is the case's
    /// own.
    Forbidden { case: String, reason: String },
    /// The host is not the oracle the suite names, so a recording taken here would be keyed
    /// wrongly.
    HostMoved { found: String, named: String },
    /// There is no Metal here.
    NoMetal,
    /// The case could not state its own inputs or shaders.
    Case { case: String, why: String },
    /// No run produced an answer.
    NoRuns { case: String },
    /// Metal's own run of the case **stopped** at a site, so the transcript holds part of the case
    /// and not its answer. The site and the refusal, in the words a ledger row uses.
    Refused {
        case: String,
        at: String,
        refusal: String,
    },
}

impl std::fmt::Display for RecordError {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            RecordError::Forbidden { case, reason } => write!(
                out,
                "{case}: its Metal answer is carried and must not be re-recorded: {reason}"
            ),
            RecordError::HostMoved { found, named } => write!(
                out,
                "this host is {found}, and the suite records on {named}: re-point ORACLE_CHIP, \
                 ORACLE_OS_VERSION and ORACLE_OS_BUILD, and re-record every case"
            ),
            RecordError::NoMetal => out.write_str("there is no Metal on this host"),
            RecordError::Case { case, why } => write!(out, "{case}: {why}"),
            RecordError::NoRuns { case } => write!(out, "{case}: no run produced an answer"),
            RecordError::Refused { case, at, refusal } => write!(
                out,
                "{case}: Metal's own run stopped at {at} ({refusal}), so a recording taken here \
                 would hold part of the case and not its answer. Fix what the Metal leg cannot \
                 do, or state the answer by hand if Metal does not come back from the call"
            ),
        }
    }
}

/// Take `case`'s answer by running it `runs` times on `side`, with
/// `validated` what Metal's API validation did with the same case.
///
/// `side` must be a Metal side. The two Metal facts stay apart: the transcripts are plain Metal's,
/// and `validated` is recorded beside them without ever being compared with our behaviour.
///
/// A run that **stops** is refused rather than written down. [`crate::case::call`] aborts the whole
/// case, so the sites after it are never emitted, and a recording of a truncated Metal run is not a
/// recording of the case: on replay every site past the stop reads as one only our side observed,
/// which is a divergence the case never asked about. Worse, a recorded Metal `Err` our side happens
/// to match compares **exact**, so the case would report as passing where neither side did the
/// work. So the recording is refused, naming the site, and the author either fixes what the Metal
/// leg cannot do or states the answer by hand ([`never_returned`]).
///
/// A refusal a [`crate::case::probe`] sweep records at its own site is the deliberate complement:
/// it emits every other site, so the transcript is the whole case with one query Metal answered nil
/// for, which is Metal's answer and compares as one. Only a stop truncates.
pub fn record(
    case: &dyn Case,
    side: &mut dyn Side,
    runs: u32,
    validated: Validated,
) -> Result<Answer, RecordError> {
    if let Some(reason) = case.metal_source().forbidden() {
        return Err(RecordError::Forbidden {
            case: case.name().to_owned(),
            reason: reason.to_owned(),
        });
    }
    check_host()?;
    let inputs = case.inputs().map_err(|why| RecordError::Case {
        case: case.name().to_owned(),
        why,
    })?;
    let mut outcomes: Vec<Outcome> = Vec::new();
    for _ in 0..runs {
        let mut sites = Sites::new();
        sites.push(crate::case::inputs_node(&inputs));
        if let Err(stopped) = case.run(side, &mut sites) {
            return Err(RecordError::Refused {
                case: case.name().to_owned(),
                at: stopped.at,
                refusal: crate::diff::print(&stopped.failure.recorded()),
            });
        }
        let announced = side.announced();
        sites.notices(NOTICES_SITE, &announced);
        add(&mut outcomes, sites.finish());
    }
    if outcomes.is_empty() {
        return Err(RecordError::NoRuns {
            case: case.name().to_owned(),
        });
    }
    let venue = side.venue();
    Ok(Answer {
        key: recording::OracleKey::current(),
        provenance: Provenance::MetalRun,
        device: venue.device,
        runs,
        outcomes,
        validated,
    })
}

/// The answer a case carries from a measurement this tree already holds: one outcome, the runs the
/// measurement stands on, where it came from, and the chip it was taken on.
pub fn carried(
    from: &str,
    measured_on: &str,
    device: &str,
    runs: u32,
    transcript: Transcript,
    validated: Validated,
) -> Answer {
    Answer {
        key: recording::OracleKey::current(),
        provenance: Provenance::Carried {
            from: from.to_owned(),
            measured_on: measured_on.to_owned(),
        },
        device: device.to_owned(),
        runs,
        outcomes: vec![Outcome { runs, transcript }],
        validated,
    }
}

/// The answer a row has where **Metal did not return** from the call: one outcome, which the case
/// states by hand from the probe that died, and a provenance naming the call.
///
/// Nothing here runs anything. This is the one answer in the suite that cannot be taken by running
/// its own shape -- making the call is what ends Metal's process -- so [`record`] refuses such a
/// row ([`crate::case::MetalSource::forbidden`]) and this is the only way one is built. The
/// validated oracle is [`Validated::NotRun`] for the same reason: Metal's API validation layer was
/// never asked either, and the file says so rather than implying it was content.
pub fn never_returned(
    from: &str,
    call: &str,
    device: &str,
    runs: u32,
    transcript: Transcript,
) -> Answer {
    Answer {
        key: recording::OracleKey::current(),
        provenance: Provenance::NeverReturned {
            from: from.to_owned(),
            call: call.to_owned(),
        },
        device: device.to_owned(),
        runs,
        outcomes: vec![Outcome { runs, transcript }],
        validated: Validated::NotRun,
    }
}

/// Write `answers` as `case`'s recording.
///
/// Everything a recording already written holds that Metal does not measure is **kept**
/// ([`Recording::carrying_the_answers_of`]): the cost half, and any other half another leg
/// measures on another machine. Re-taking Metal's answer neither refreshes nor invalidates them,
/// and dropping one here would silently empty its gate every time a case was re-recorded.
///
/// The keeping is a default rather than a list, so a half added later is carried without anyone
/// remembering to add it.
pub fn write(case: &dyn Case, answers: Vec<Answer>) -> Result<std::path::PathBuf, String> {
    let shaders = case.shaders()?;
    let fresh = Recording::new(case.name(), shaders, answers);
    let recording = match recording::read(case.name()) {
        Ok(held) => held.carrying_the_answers_of(fresh),
        Err(_) => fresh,
    };
    recording::write(&recording)
}

/// Count `transcript` among `outcomes`, as a new outcome or as another run of one already held.
fn add(outcomes: &mut Vec<Outcome>, transcript: Transcript) {
    match outcomes
        .iter_mut()
        .find(|outcome| outcome.transcript == transcript)
    {
        Some(held) => held.runs += 1,
        None => outcomes.push(Outcome {
            runs: 1,
            transcript,
        }),
    }
}

/// Refuse to record on a host that is not the oracle the suite names.
#[cfg(target_os = "macos")]
fn check_host() -> Result<(), RecordError> {
    let read = |flag: &str| {
        std::process::Command::new("/usr/bin/sw_vers")
            .arg(flag)
            .output()
            .ok()
            .filter(|output| output.status.success())
            .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
            .unwrap_or_default()
    };
    use crate::recording::{ORACLE_CHIP, ORACLE_OS_BUILD, ORACLE_OS_VERSION};
    let version = read("-productVersion");
    let build = read("-buildVersion");
    if version == ORACLE_OS_VERSION && build == ORACLE_OS_BUILD {
        return Ok(());
    }
    Err(RecordError::HostMoved {
        found: format!("macOS {version} ({build})"),
        named: format!("{ORACLE_CHIP} on macOS {ORACLE_OS_VERSION} ({ORACLE_OS_BUILD})"),
    })
}

/// There is no Metal to record from off macOS.
#[cfg(not(target_os = "macos"))]
fn check_host() -> Result<(), RecordError> {
    Err(RecordError::NoMetal)
}
