//! Which side holds the cost of a `render_pipeline` call: our translation, the driver's compile,
//! or the rest of the call.
//!
//! `known-slow-calls.tsv`'s `render_pipeline` row is the one row in that file whose **per-case**
//! figures go the wrong way as well as its per-call ones, so for this family the call really is
//! the case. The row localises the cost to "the per-pipeline work with a module already in hand"
//! without reading the counter that would say whether a module was in hand, and it names no side.
//!
//! This runs a case's own calls, with the case's own descriptors, on our runtime, and reads the
//! counters that tell the sides apart **around each call**:
//!
//! | counter | what a `+1` says |
//! |---|---|
//! | `descriptor_keys_built` | we digested a descriptor's `Debug` form into a `PipelineKey` |
//! | `linked_use_questions_asked` | a creation check decoded a stage rather than reading its answer |
//! | `stage_entries_specialized` | we decoded and specialized a stage's AIR |
//! | `functions_translated` | we lowered a stage's function to SPIR-V on this call |
//! | `functions_shared` | we did not: the memo, an archive or the cache gave it |
//! | `shader_modules_made` | we made a `VkShaderModule` of it |
//! | `variant_pipelines_made` | we made a `VkPipeline`, so the driver was asked for one |
//! | `creation_render_pipelines_made` | that `VkPipeline` was the creation's predicted one |
//! | `flights_followed` | the making waited for a twin's making in the air to land |
//! | `driver_pipeline_hits` | the driver says it found the pipeline, or every stage of it |
//! | `driver_pipeline_misses` | the driver says it compiled rather than read its cache |
//! | `hits` / `misses` | artifacts the cache directory answered, or did not |
//!
//! # The cells, and how to get them
//!
//! The shader cache keeps our translations under `<root>/v1/<build>/` and the driver's own
//! pipeline cache data under `<root>/v1/<content>/` (`shader_cache/store.rs`), two directories a
//! run can clear apart. So one process per cell, over one cache directory:
//!
//! - **both** — a cache directory nothing has written. We translate and the driver compiles.
//! - **driver only** — the same directory with the content directory removed. We read our
//!   translation back and the driver compiles again.
//! - **neither** — the same directory untouched. We read ours back and the driver reads its own.
//! - **no memo** — the same directory with `ShaderCacheOptions::memory_capacity` at one byte, so
//!   the device's in-process memo of compiled pipelines and functions keeps nothing and every
//!   creation goes to the directory (`pipeline/memo.rs`). A capacity of **zero** is refused at
//!   device creation (`Problem::ShaderCacheCapacity`), so one byte is how the memo is turned off,
//!   as `runtime_lane/cache.rs`'s own memo-off case turns it off.
//!
//! The difference of the first two is our translation's share; of the second and third, the
//! driver's; of the third and fourth, the memo's.
//!
//! Metal underneath MoltenVK is warm whatever this does: `MTLCompilerService` keys compiled output
//! on the AIR and keeps it across processes, and no public call empties it. That is the same
//! footing the suite's own leg runs on, so these cells are of the suite's warm driver, never of a
//! cold machine.
//!
//! # Running it
//!
//! ```text
//! render-pipeline-split <case> <cache-directory> [--no-memo]
//! ```
//!
//! A reading, not a gate: it prints a row per call and exits 0 unless the case itself failed.

use metal2vulkan_conformance::device::Notices;
use metal2vulkan_conformance::runtime_side::RuntimeSide;
use metal2vulkan_conformance::transcript::Sites;
use metal2vulkan_conformance::watch::{Watcher, Watching};
use metal2vulkan_runtime::{Degradation, Device, DeviceKind, DeviceOptions, ShaderCacheOptions};
use std::cell::RefCell;
use std::rc::Rc;
use std::time::Instant;

/// The call this reads. Every other call is forwarded untimed and uncounted, so no other call's
/// pair of counter reads lands inside this one's window.
const CALL: &str = "render_pipeline";

/// The counters as they stand, of both halves.
#[derive(Clone, Copy, Debug, Default)]
struct Readings {
    keys: u64,
    questions: u64,
    entries: u64,
    translated: u64,
    functions_shared: u64,
    modules_made: u64,
    modules_shared: u64,
    layouts_made: u64,
    pipelines_made: u64,
    creation_pipelines: u64,
    flights: u64,
    driver_hits: u64,
    driver_misses: u64,
    cache_hits: u64,
    cache_misses: u64,
    writes_on_thread: u64,
}

/// What one call did, as the counters and the clocks read it.
#[derive(Clone, Copy, Debug, Default)]
struct Row {
    wall: u64,
    on_cpu: u64,
    counts: Readings,
}

fn readings(device: &Device) -> Readings {
    let counts = device.work_counts();
    let cache = device.shader_cache_statistics();
    Readings {
        keys: counts.descriptor_keys_built,
        questions: counts.linked_use_questions_asked,
        entries: counts.stage_entries_specialized,
        translated: counts.functions_translated,
        functions_shared: counts.functions_shared,
        modules_made: counts.shader_modules_made,
        modules_shared: counts.shader_modules_shared,
        layouts_made: counts.pipeline_layouts_made,
        pipelines_made: counts.variant_pipelines_made,
        creation_pipelines: counts.creation_render_pipelines_made,
        flights: counts.flights_followed,
        driver_hits: cache.driver_pipeline_hits,
        driver_misses: cache.driver_pipeline_misses,
        cache_hits: cache.hits,
        cache_misses: cache.misses,
        writes_on_thread: cache.writes_on_thread,
    }
}

impl Readings {
    /// Every field of `self` and `other` under `each`: the two readings' difference around a
    /// call, or their sum over a run's calls.
    fn zip(self, other: Readings, each: impl Fn(u64, u64) -> u64) -> Readings {
        Readings {
            keys: each(self.keys, other.keys),
            questions: each(self.questions, other.questions),
            entries: each(self.entries, other.entries),
            translated: each(self.translated, other.translated),
            functions_shared: each(self.functions_shared, other.functions_shared),
            modules_made: each(self.modules_made, other.modules_made),
            modules_shared: each(self.modules_shared, other.modules_shared),
            layouts_made: each(self.layouts_made, other.layouts_made),
            pipelines_made: each(self.pipelines_made, other.pipelines_made),
            creation_pipelines: each(self.creation_pipelines, other.creation_pipelines),
            flights: each(self.flights, other.flights),
            driver_hits: each(self.driver_hits, other.driver_hits),
            driver_misses: each(self.driver_misses, other.driver_misses),
            cache_hits: each(self.cache_hits, other.cache_hits),
            cache_misses: each(self.cache_misses, other.cache_misses),
            writes_on_thread: each(self.writes_on_thread, other.writes_on_thread),
        }
    }

    /// What happened between these two readings.
    fn since(self, before: Readings) -> Readings {
        self.zip(before, |after, before| after - before)
    }

    /// These counts and `other`'s together.
    fn and(self, other: Readings) -> Readings {
        self.zip(other, |one, two| one + two)
    }
}

/// The device and the log a span writes into. It owns a clone of the device (a retain) so a span
/// borrows nothing: [`Watcher::Span`] has no lifetime of its own.
struct Shared {
    device: Device,
    log: RefCell<Vec<Row>>,
}

/// The watcher: it reads the counters and the clocks around [`CALL`] and forwards the rest.
#[derive(Clone)]
struct Split(Rc<Shared>);

impl Watcher for Split {
    type Span = Span;

    fn enter(&self, call: &'static str) -> Span {
        if call != CALL {
            return Span {
                shared: None,
                before: Readings::default(),
                on_cpu: None,
                started: Instant::now(),
            };
        }
        // The counters first and the wall clock last, so neither counter read falls inside the
        // call's own window; the reverse at the other end, for the same reason.
        let before = readings(&self.0.device);
        let on_cpu = metal2vulkan_conformance::clocks::thread_cpu_nanos();
        Span {
            shared: Some(Rc::clone(&self.0)),
            before,
            on_cpu,
            started: Instant::now(),
        }
    }
}

/// One call's reading, open until the call returns.
struct Span {
    shared: Option<Rc<Shared>>,
    before: Readings,
    on_cpu: Option<u64>,
    started: Instant,
}

impl Drop for Span {
    fn drop(&mut self) {
        let Some(shared) = self.shared.take() else {
            return;
        };
        let wall = u64::try_from(self.started.elapsed().as_nanos()).unwrap_or(u64::MAX);
        let on_cpu = match (
            self.on_cpu,
            metal2vulkan_conformance::clocks::thread_cpu_nanos(),
        ) {
            (Some(before), Some(after)) => after.saturating_sub(before),
            _ => 0,
        };
        let after = readings(&shared.device);
        shared.log.borrow_mut().push(Row {
            wall,
            on_cpu,
            counts: after.since(self.before),
        });
    }
}

/// The suite's device options, with the shader cache in `directory` rather than a scratch
/// directory the run owns: the cells are processes over one directory. `memo` false bounds the
/// device's memo to nothing.
fn options(notices: &Notices, directory: &str, memo: bool) -> DeviceOptions {
    let mut options = DeviceOptions::default();
    options.policy = options.policy.exact_answers();
    options.announce_each = Degradation::ALL.iter().copied().collect();
    options.shader_cache = ShaderCacheOptions::in_directory(std::path::Path::new(directory));
    if !memo {
        // Zero is refused (`shader_cache::CacheSetup::of`); one byte keeps nothing.
        options.shader_cache.memory_capacity = 1;
    }
    options.messages = Some(notices.sink());
    options.kinds = vec![
        DeviceKind::Discrete,
        DeviceKind::Integrated,
        DeviceKind::Virtual,
    ];
    options
}

fn main() -> std::process::ExitCode {
    match run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(why) => {
            eprintln!("render-pipeline-split: {why}");
            std::process::ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let mut words = std::env::args().skip(1);
    let name = words
        .next()
        .ok_or("usage: render-pipeline-split <case> <cache-directory> [--no-memo]")?;
    let directory = words
        .next()
        .ok_or("usage: render-pipeline-split <case> <cache-directory> [--no-memo]")?;
    let memo = !words.any(|word| word == "--no-memo");
    let case = metal2vulkan_conformance::cases::named(&name).ok_or(format!("no case {name}"))?;
    let mut notices = Notices::default();
    let device = Device::system_default_with(&options(&notices, &directory, memo))
        .map_err(|error| format!("no device: {error}"))?;
    notices.venue();
    let shared = Rc::new(Shared {
        device: device.clone(),
        log: RefCell::new(Vec::new()),
    });
    let watcher = Split(Rc::clone(&shared));
    let mut sites = Sites::new();
    let side = RuntimeSide::new(&device, notices.open(case.name()));
    let mut watching = Watching::with(side, watcher);
    let outcome = case.run(&mut watching, &mut sites);
    drop(watching);
    let rows = shared.log.borrow().clone();
    println!(
        "case {name} venue {} dir {directory} memo {}",
        device.name(),
        match memo {
            true => "on",
            false => "off",
        }
    );
    let head = [
        "call", "wall us", "cpu us", "keys", "qstn", "entr", "xlat", "fshr", "modm", "mods",
        "laym", "pipm", "crea", "flig", "dhit", "dmis", "chit", "cmis", "wthr",
    ];
    println!(
        "{:>4}  {:>10} {:>10} {}",
        head[0],
        head[1],
        head[2],
        head[3..]
            .iter()
            .map(|word| format!("{word:>5}"))
            .collect::<String>()
    );
    let line = |at: String, row: &Row| {
        let c = row.counts;
        println!(
            "{at:>4}  {:>10.1} {:>10.1} {}",
            row.wall as f64 / 1000.0,
            row.on_cpu as f64 / 1000.0,
            [
                c.keys,
                c.questions,
                c.entries,
                c.translated,
                c.functions_shared,
                c.modules_made,
                c.modules_shared,
                c.layouts_made,
                c.pipelines_made,
                c.creation_pipelines,
                c.flights,
                c.driver_hits,
                c.driver_misses,
                c.cache_hits,
                c.cache_misses,
                c.writes_on_thread,
            ]
            .iter()
            .map(|count| format!("{count:>5}"))
            .collect::<String>()
        );
    };
    for (at, row) in rows.iter().enumerate() {
        line(at.to_string(), row);
    }
    let mut total = Row::default();
    for row in &rows {
        total.wall += row.wall;
        total.on_cpu += row.on_cpu;
        total.counts = total.counts.and(row.counts);
    }
    line("SUM".to_owned(), &total);
    // The first call of a process is a warm-up and not a call: the medians leave it out.
    let after_first = rows.get(1..).unwrap_or_default();
    let median = |pick: fn(&Row) -> u64| {
        let mut all: Vec<u64> = after_first.iter().map(pick).collect();
        all.sort_unstable();
        all.get(all.len() / 2).copied().unwrap_or_default() as f64 / 1000.0
    };
    println!(
        "CALLS {} first {:.1} us  median-after-first wall {:.1} us cpu {:.1} us  total wall {:.1} us",
        rows.len(),
        rows.first().map_or(0.0, |row| row.wall as f64 / 1000.0),
        median(|row| row.wall),
        median(|row| row.on_cpu),
        total.wall as f64 / 1000.0,
    );
    if let Err(abort) = outcome {
        return Err(format!(
            "the case aborted at {}: {:?}",
            abort.at, abort.failure
        ));
    }
    Ok(())
}
