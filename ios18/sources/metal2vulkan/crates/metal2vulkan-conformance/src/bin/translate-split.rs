//! Where one AIR-to-SPIR-V translation's milliseconds go.
//!
//! `known-slow-calls.tsv`'s `compute_pipeline_bindings` row is attributed: the call's cost is the
//! **one** translation of the kernel, 66-88% of a cold creation, 1.0-5.3 ms of 1.4-8.8 ms, paid
//! once per shader (`functions_translated` 1 with `functions_shared` 0 on the first creation and
//! 0 with 1 on the second). This probe does not re-attribute that. It splits the translation
//! itself.
//!
//! # Why a loop and not the case
//!
//! `Device::compile_function` is the whole translation and the translator keeps no memo of it
//! ("the caller caches the result under its `FunctionCompileKey`"), so calling it again translates
//! again: decode, specialize, analyze, select, structurize, emit, tidy, verify, serialize. That
//! makes the cold cost a **marginal** cost, measurable as a median over a long loop rather than as
//! a process's first call, which has cost 1.0-13.7 ms on this host and moved an order of magnitude
//! between runs (`stage-input-split`).
//!
//! # Running it
//!
//! ```text
//! translate-split list
//! translate-split once  <cache-directory> [--default-policy]
//! translate-split loop  <fixture-name> <iterations> <cache-directory> [--default-policy]
//! translate-split parts <iterations> <cache-directory> [--default-policy]
//! translate-split digest <cache-directory> [--default-policy]
//! translate-split library <fixture-name> <iterations> <cache-directory> [--default-policy]
//! ```
//!
//! `once` translates every `function-bindings-*` shader once in one process and prints the
//! per-shader wall and thread CPU with the compile's own work units and ledger peak, which are
//! deterministic and so immune to host load. `loop` repeats one shader's translation, which is the
//! shape to record an Instruments Time Profiler over (`scripts/xctrace-run`).
//!
//! `library` translates **every** entry of one fixture's library once a round, twice over: with
//! one `StageEntries` shared between the entries, as the runtime's device gives one object to
//! every creation of its pipelines, and with an object an entry, which is what a consumer that
//! shares nothing does. Nothing in a module's decode depends on which entry is being compiled, so
//! the shared round reads one decode for the whole library and the apart round one an entry; the
//! two rounds specialize and compile the same six entries from scratch, so what they differ by is
//! five decodes. A round and not a loop: a second ask for one entry finds its specialized entry
//! and compiles nothing at all, which is a different question (`parts`).
//!
//! The suite's device takes `DevicePolicy::exact_answers()`, so that is the default here and the
//! figures line up with the ledger's; `--default-policy` reads the policy a consumer gets.
//!
//! A reading, not a gate: it prints and exits 0 unless the run itself failed.

use metal2vulkan::pipeline::{FunctionDescriptor, LoweringInputs, StageEntries};
use metal2vulkan::Budget;
use metal2vulkan_conformance::clocks;
use metal2vulkan_conformance::device::Notices;
use metal2vulkan_conformance::fixture::Fixture;
use metal2vulkan_runtime::{Device, DeviceKind, DeviceOptions, ShaderCacheOptions};

/// One `function-bindings-*` shader: the fixture and the entry the case names, as
/// `cases/function_bindings.rs` lists them. Kept here rather than reached for, because the case
/// set's entries are private to it; a drift would show as a parse failure rather than quietly.
const SHADERS: &[(&str, &str)] = &[
    ("px_samplers", "px_sample_argument"),
    ("xr_samples", "xr_sample_runtime"),
    ("a27_ob1_threadgroup", "ob1_threadgroup"),
    ("visible_table_caller", "visible_table_dispatch"),
    ("a4_imageblock_extent", "a4_imageblock_extent"),
    ("reflection/encoder_types", "encoder_types_plain_0"),
    ("reflection/encoder_types", "encoder_types_plain_1"),
    ("reflection/encoder_types", "encoder_types_plain_2"),
    ("reflection/encoder_types", "encoder_types_tail_0"),
    ("reflection/encoder_types", "encoder_types_tail_1"),
    ("reflection/encoder_types", "encoder_types_tail_2"),
    ("reflection/argument_buffers", "reflection_argument_buffers"),
    ("reflection/encoder_arrays", "encoder_arrays"),
    ("reflection/textures", "reflection_textures"),
    (
        "reflection/tables_in_arguments",
        "reflection_tables_in_arguments",
    ),
];

fn main() -> std::process::ExitCode {
    match run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(why) => {
            eprintln!("translate-split: {why}");
            std::process::ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let flag = |name: &str| args.iter().any(|a| a == name);
    let positional: Vec<&String> = args.iter().filter(|a| !a.starts_with("--")).collect();
    let exact = !flag("--default-policy");
    match positional.first().map(|s| s.as_str()) {
        Some("list") => {
            for (fixture, entry) in SHADERS {
                println!("{fixture} {entry}");
            }
            Ok(())
        }
        Some("once") => {
            let directory = positional.get(1).ok_or("usage: once <cache-directory>")?;
            once_mode(directory, exact)
        }
        Some("digest") => {
            let directory = positional.get(1).ok_or("usage: digest <cache-directory>")?;
            digest_mode(directory, exact)
        }
        Some("parts") => {
            let times: usize = positional
                .get(1)
                .ok_or("usage: parts <iterations> <cache-directory>")?
                .parse()
                .map_err(|_| "iterations is not a number".to_string())?;
            let directory = positional
                .get(2)
                .ok_or("usage: parts <iterations> <cache-directory>")?;
            parts_mode(times, directory, exact)
        }
        Some("library") => {
            let name = positional
                .get(1)
                .ok_or("usage: library <fixture-name> <iterations> <cache-directory>")?;
            let times: usize = positional
                .get(2)
                .ok_or("usage: library <fixture-name> <iterations> <cache-directory>")?
                .parse()
                .map_err(|_| "iterations is not a number".to_string())?;
            let directory = positional
                .get(3)
                .ok_or("usage: library <fixture-name> <iterations> <cache-directory>")?;
            library_mode(name, times, directory, exact)
        }
        Some("loop") => {
            let name = positional
                .get(1)
                .ok_or("usage: loop <fixture-name> <iterations> <cache-directory>")?;
            let times: usize = positional
                .get(2)
                .ok_or("usage: loop <fixture-name> <iterations> <cache-directory>")?
                .parse()
                .map_err(|_| "iterations is not a number".to_string())?;
            let directory = positional
                .get(3)
                .ok_or("usage: loop <fixture-name> <iterations> <cache-directory>")?;
            loop_mode(name, times, directory, exact)
        }
        _ => Err(
            "usage: list | once <dir> | loop <fixture> <times> <dir> | parts <times> <dir> \
             | digest <dir>"
                .to_string(),
        ),
    }
}

/// The suite's device options, with the shader cache in `directory`. The cache is never read on
/// this path -- `compile_function` does not consult it -- but a device needs one.
fn options(notices: &Notices, directory: &str, exact: bool) -> DeviceOptions {
    let mut options = DeviceOptions::default();
    if exact {
        options.policy = options.policy.exact_answers();
    }
    options.shader_cache = ShaderCacheOptions::in_directory(std::path::Path::new(directory));
    options.messages = Some(notices.sink());
    options.kinds = vec![
        DeviceKind::Discrete,
        DeviceKind::Integrated,
        DeviceKind::Virtual,
    ];
    options
}

/// The stage descriptor of entry `entry` of fixture `fixture`, through the runtime's own library
/// path, as a case's own pipeline creation builds it.
fn descriptor(
    device: &Device,
    fixture: &'static str,
    entry: &'static str,
) -> Result<FunctionDescriptor, String> {
    let bitcode = Fixture {
        name: fixture,
        entry,
    }
    .bitcode()?;
    let library = metal2vulkan::library::Library::parse(&bitcode)
        .map_err(|error| format!("{fixture} did not parse: {error}"))?;
    let library = device.new_library_with_compile_library(library);
    let function = library
        .new_function_with_name(entry)
        .map_err(|error| format!("no function {entry} in {fixture}: {error}"))?;
    Ok(function.function_descriptor())
}

fn nanos_since(started: std::time::Instant) -> u64 {
    u64::try_from(started.elapsed().as_nanos()).unwrap_or(u64::MAX)
}

fn cpu_delta(before: Option<u64>) -> u64 {
    match (before, clocks::thread_cpu_nanos()) {
        (Some(a), Some(b)) => b.saturating_sub(a),
        _ => 0,
    }
}

fn load() -> f64 {
    clocks::host_load().unwrap_or(f64::NAN)
}

/// Every shader translated once, in one process. The first row of the run is a process warm-up and
/// is printed as such.
fn once_mode(directory: &str, exact: bool) -> Result<(), String> {
    let notices = Notices::default();
    let device = Device::system_default_with(&options(&notices, directory, exact))
        .map_err(|error| format!("no device: {error}"))?;
    let compile = device.compile_device();
    let budget = Budget::standard();
    let inputs = LoweringInputs::new();
    println!(
        "once venue {} policy {} load {:.2}",
        device.name(),
        if exact { "exact" } else { "default" },
        load()
    );
    println!(
        "{:<46} {:>9} {:>9} {:>11} {:>11} {:>7} {:>7} {:>7}",
        "shader", "wall us", "cpu us", "work", "ledger", "funcs", "blocks", "instrs"
    );
    for &(fixture, entry) in SHADERS {
        let f = descriptor(&device, fixture, entry)?;
        let cpu_before = clocks::thread_cpu_nanos();
        let started = std::time::Instant::now();
        let (compiled, report) = compile.compile_function_with_report(&f, &inputs, &budget);
        let wall = nanos_since(started);
        let cpu = cpu_delta(cpu_before);
        let stats = compiled.as_ref().ok().map(|c| c.module_stats());
        println!(
            "{:<46} {:>9.1} {:>9.1} {:>11} {:>11} {:>7} {:>7} {:>7}",
            format!("{fixture}:{entry}"),
            wall as f64 / 1000.0,
            cpu as f64 / 1000.0,
            report.work_units,
            report.ledger_peak,
            stats.map_or(0, |s| s.functions),
            stats.map_or(0, |s| s.blocks),
            stats.map_or(0, |s| s.instructions),
        );
        if let Err(error) = compiled {
            println!("    refused: {error}");
        }
    }
    println!("load after {:.2}", load());
    Ok(())
}

/// One shader's translation repeated `times`, the median reported: the shape to profile.
fn loop_mode(name: &str, times: usize, directory: &str, exact: bool) -> Result<(), String> {
    let &(fixture, entry) = SHADERS
        .iter()
        .find(|(f, e)| *f == name || *e == name || format!("{f}:{e}") == name)
        .ok_or(format!("no shader {name}; try `list`"))?;
    let notices = Notices::default();
    let device = Device::system_default_with(&options(&notices, directory, exact))
        .map_err(|error| format!("no device: {error}"))?;
    let compile = device.compile_device();
    let budget = Budget::standard();
    let inputs = LoweringInputs::new();
    let f = descriptor(&device, fixture, entry)?;
    let before = load();
    let warming = (times / 16).max(4);
    for _ in 0..warming {
        compile
            .compile_function(&f, &inputs, &budget)
            .map_err(|error| format!("the translation refused: {error}"))?;
    }
    let mut all: Vec<u64> = Vec::with_capacity(times);
    let cpu_before = clocks::thread_cpu_nanos();
    let whole = std::time::Instant::now();
    for _ in 0..times {
        let started = std::time::Instant::now();
        let compiled = compile.compile_function(&f, &inputs, &budget);
        all.push(nanos_since(started));
        compiled.map_err(|error| format!("the translation refused: {error}"))?;
    }
    let total = nanos_since(whole);
    let cpu = cpu_delta(cpu_before);
    all.sort_unstable();
    let at = |p: f64| all[((all.len() - 1) as f64 * p).round() as usize] as f64 / 1000.0;
    println!(
        "loop {fixture}:{entry} times {times} policy {} venue {}",
        if exact { "exact" } else { "default" },
        device.name()
    );
    println!("load before {:.2} after {:.2}", before, load());
    println!(
        "median {:.1} us  p10 {:.1}  p90 {:.1}  min {:.1}  whole {:.1} ms  cpu {:.1} ms \
         ({:.0}% of wall)",
        at(0.5),
        at(0.10),
        at(0.90),
        at(0.0),
        total as f64 / 1_000_000.0,
        cpu as f64 / 1_000_000.0,
        100.0 * cpu as f64 / total as f64
    );
    Ok(())
}

/// Every shader's translation split in two by the public API: the whole of it, and the same
/// compile with the decode and the specialization already paid.
fn parts_mode(times: usize, directory: &str, exact: bool) -> Result<(), String> {
    let notices = Notices::default();
    let device = Device::system_default_with(&options(&notices, directory, exact))
        .map_err(|error| format!("no device: {error}"))?;
    let compile = device.compile_device();
    let budget = Budget::standard();
    let inputs = LoweringInputs::new();
    let warming = (times / 16).max(4);
    println!(
        "parts {times} venue {} policy {} load {:.2}",
        device.name(),
        if exact { "exact" } else { "default" },
        load()
    );
    println!(
        "{:<46} {:>9} {:>9} {:>9} {:>6} {:>13}",
        "shader", "whole us", "kept us", "decode+spec", "share", "entries m/f"
    );
    for &(fixture, entry) in SHADERS {
        let f = descriptor(&device, fixture, entry)?;
        let whole = median_of(times, warming, &mut || {
            let started = std::time::Instant::now();
            let made = compile.compile_function(&f, &inputs, &budget);
            let took = nanos_since(started);
            made.map(|_| took)
        })?;
        // One object across the loop: the first ask decodes and specializes, every later one
        // finds that entry, so what the loop times is the compile past those two stages.
        let entries = StageEntries::new();
        let kept = median_of(times, warming, &mut || {
            let started = std::time::Instant::now();
            let made = compile.compile_function_from_entries(&f, &inputs, &budget, &entries);
            let took = nanos_since(started);
            made.map(|_| took)
        })?;
        let (made, found) = entries.counts();
        println!(
            "{:<46} {:>9.1} {:>9.1} {:>11.1} {:>5.0}% {:>7}/{:<5}",
            format!("{fixture}:{entry}"),
            whole,
            kept,
            whole - kept,
            100.0 * (whole - kept) / whole,
            made,
            found,
        );
    }
    println!("load after {:.2}", load());
    Ok(())
}

/// Every entry of one fixture's library translated, with one object shared between the entries
/// and then with an object each: what a library of several entries pays for its module's decode.
fn library_mode(name: &str, times: usize, directory: &str, exact: bool) -> Result<(), String> {
    let &(fixture, _) = SHADERS
        .iter()
        .find(|(f, e)| *f == name || *e == name)
        .ok_or(format!("no shader {name}; try `list`"))?;
    let notices = Notices::default();
    let device = Device::system_default_with(&options(&notices, directory, exact))
        .map_err(|error| format!("no device: {error}"))?;
    let compile = device.compile_device();
    let budget = Budget::standard();
    let inputs = LoweringInputs::new();
    let bitcode = Fixture {
        name: fixture,
        entry: "",
    }
    .bitcode()?;
    let parsed = metal2vulkan::library::Library::parse(&bitcode)
        .map_err(|error| format!("{fixture} did not parse: {error}"))?;
    let entries: Vec<String> = parsed
        .functions()
        .filter(|i| {
            !matches!(
                i.stage,
                metal2vulkan::library::FunctionStage::Visible
                    | metal2vulkan::library::FunctionStage::Exported
            )
        })
        .map(|i| i.name.to_owned())
        .collect();
    let library = device.new_library_with_compile_library(parsed);
    let stages: Vec<FunctionDescriptor> = entries
        .iter()
        .map(|name| {
            library
                .new_function_with_name(name)
                .map(|f| f.function_descriptor())
                .map_err(|error| format!("no function {name}: {error}"))
        })
        .collect::<Result<_, _>>()?;
    println!(
        "library {fixture} entries {} rounds {times} venue {} policy {} load {:.2}",
        stages.len(),
        device.name(),
        if exact { "exact" } else { "default" },
        load()
    );
    // Each round compiles every entry of the library ONCE, so what it times is a library's
    // entries met one after another and not a loop over one of them: with the entry memo warm a
    // later ask would compile nothing at all. Shared: one object for the round, so the entries
    // read one decode between them. Apart: an object an entry, so each reads its own.
    let round = |shared: bool| -> Result<u64, String> {
        let own = StageEntries::new();
        let mut took = 0u64;
        for f in &stages {
            let entries = if shared {
                None
            } else {
                Some(StageEntries::new())
            };
            let use_it = entries.as_ref().unwrap_or(&own);
            let started = std::time::Instant::now();
            let made = compile.compile_function_from_entries(f, &inputs, &budget, use_it);
            took += nanos_since(started);
            made.map_err(|error| format!("the translation refused: {error}"))?;
        }
        Ok(took)
    };
    let median = |shared: bool| -> Result<f64, String> {
        for _ in 0..(times / 16).max(4) {
            round(shared)?;
        }
        let mut all = Vec::with_capacity(times);
        for _ in 0..times {
            all.push(round(shared)?);
        }
        all.sort_unstable();
        Ok(all[all.len() / 2] as f64 / 1000.0)
    };
    // The apart rounds first, so neither ordering can be said to have warmed the other.
    let apart = median(false)?;
    let shared = median(true)?;
    println!(
        "a round of {} entries: {:.1} us sharing one decode against {:.1} us reading one each, \
         {:.1} us less ({:.0}% of it)",
        stages.len(),
        shared,
        apart,
        apart - shared,
        100.0 * (apart - shared) / apart,
    );
    // One round on a fresh object, to report what it read: one decode for the library, and an
    // entry made for each of its entries with none found.
    let fresh = StageEntries::new();
    for f in &stages {
        compile
            .compile_function_from_entries(f, &inputs, &budget, &fresh)
            .map_err(|error| format!("the translation refused: {error}"))?;
    }
    println!("one round's entries m/f {:?}", fresh.counts());
    println!("load after {:.2}", load());
    Ok(())
}

/// The median of `times` calls of `each`, after `warming` discarded, in microseconds.
fn median_of(
    times: usize,
    warming: usize,
    each: &mut impl FnMut() -> Result<u64, metal2vulkan::error::Error>,
) -> Result<f64, String> {
    for _ in 0..warming {
        each().map_err(|error| format!("the translation refused: {error}"))?;
    }
    let mut all: Vec<u64> = Vec::with_capacity(times);
    for _ in 0..times {
        all.push(each().map_err(|error| format!("the translation refused: {error}"))?);
    }
    all.sort_unstable();
    Ok(all[all.len() / 2] as f64 / 1000.0)
}

/// Every shader's compiled artifact by its digest: what a speed change must leave alone.
///
/// `CompiledFunction::to_bytes` is the artifact the shader cache stores, so its digest covers the
/// serialized SPIR-V words, the relocations, the requirements and every plain fact the compile
/// records beside them. Two runs over one fixture set print the same lines or the change moved
/// semantics.
///
/// Each shader is translated twice: on its own (`compile_function`, which shares nothing) and
/// reading one `StageEntries` the whole run shares, as the runtime's device gives one object to
/// every creation of its pipelines. The two digests must agree with each other and with the
/// previous run's, so the line carries one digest and `shared` or the two digests where they
/// differ.
fn digest_mode(directory: &str, exact: bool) -> Result<(), String> {
    let notices = Notices::default();
    let device = Device::system_default_with(&options(&notices, directory, exact))
        .map_err(|error| format!("no device: {error}"))?;
    let compile = device.compile_device();
    let budget = Budget::standard();
    let inputs = LoweringInputs::new();
    println!(
        "digest venue {} policy {}",
        device.name(),
        if exact { "exact" } else { "default" }
    );
    // One object the whole run shares, so a shader whose module another shader of the set also
    // uses reads its decode rather than its own (`reflection/encoder_types`' six entries).
    let shared = StageEntries::new();
    for &(fixture, entry) in SHADERS {
        let f = descriptor(&device, fixture, entry)?;
        let digest = |made: Result<_, metal2vulkan::error::Error>| match made {
            Ok(compiled) => {
                let bytes = metal2vulkan::library::CompiledFunction::to_bytes(&compiled);
                format!(
                    "{} bytes {}",
                    metal2vulkan_conformance::sha256_hex(&bytes),
                    bytes.len()
                )
            }
            Err(error) => format!("refused {error}"),
        };
        let alone = digest(compile.compile_function(&f, &inputs, &budget));
        let from_entries =
            digest(compile.compile_function_from_entries(&f, &inputs, &budget, &shared));
        let line = if alone == from_entries {
            format!("{alone} shared")
        } else {
            format!("{alone} SHARED MOVED IT TO {from_entries}")
        };
        println!("{:<46} {line}", format!("{fixture}:{entry}"));
    }
    println!("entries m/f {:?}", shared.counts());
    Ok(())
}
