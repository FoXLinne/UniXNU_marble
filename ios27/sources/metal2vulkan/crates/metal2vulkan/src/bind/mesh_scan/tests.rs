//! The mesh scan: valid on every committed profile (L3, `G-val`), and, on the CPU interpreter,
//! each object threadgroup's first mesh threadgroup, the draw's `T`, its reservation in the
//! records pool, its dispatch folded into rows and its draw; an indirect draw's grid from Metal's
//! arguments, held to Metal's limit; and a draw the pool cannot hold draws nothing, sets
//! `MESH_RECORDS_OVERFLOW` and names itself, the first such draw only. Slots in the records pool
//! (`work::SLOTS`) scan as slots in the work buffer do. The object arguments pass: valid on every
//! committed profile, and on the interpreter it reserves an indirect draw's slots and writes its
//! object dispatch, or runs nothing and says why.

use super::*;
use crate::bind::status::{ExecutionStatus, MeshRecordsFault, StatusReport};
use crate::spirv::interp::{run, Buffer, Config, Order, Resources};
use crate::spirv::testing::{assert_valid, committed_profiles};

fn words(bytes: &[u8]) -> Vec<u32> {
    bytes
        .as_chunks::<4>()
        .0
        .iter()
        .map(|c| u32::from_le_bytes(*c))
        .collect()
}

fn bytes(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

/// A draw's work buffer before the scan: `grids` of the object threadgroups, slots of
/// `slot_words`, the records of `group_words` words and `group_vertices` vertices per mesh
/// threadgroup, a pool of `pool_words`, draw number 7, rows of `row`, and Metal's arguments at
/// word `arguments - 1` where it is not 0; the slots and offsets at word `pooled` of the pool
/// where it is `Some`, else after the header.
struct Draw {
    grids: Vec<[u32; 3]>,
    slot_words: u32,
    group_words: u32,
    group_vertices: u32,
    pool_words: u32,
    row: u32,
    arguments: u32,
    pooled: Option<u32>,
}

impl Draw {
    fn of(grids: &[[u32; 3]]) -> Draw {
        Draw {
            grids: grids.to_vec(),
            slot_words: 6,
            group_words: 10,
            group_vertices: 3,
            pool_words: 1000,
            row: 4,
            arguments: 0,
            pooled: None,
        }
    }

    /// The slots and the offsets before the scan.
    fn slots(&self) -> Vec<u32> {
        let mut w = Vec::new();
        for g in &self.grids {
            let mut slot = vec![0xAAAA_AAAA; self.slot_words as usize];
            slot[..3].copy_from_slice(g);
            slot[3] = 0;
            w.extend(slot);
        }
        // The offsets, which the scan writes.
        w.extend(std::iter::repeat_n(0xEEEE_EEEE, self.grids.len() + 1));
        w
    }

    /// The pool's words before the scan: `len` words, the slots at `pooled`.
    fn pool(&self, len: usize) -> Vec<u32> {
        let mut w = vec![0x5555_5555; len];
        if let Some(at) = self.pooled {
            let slots = self.slots();
            w[at as usize..][..slots.len()].copy_from_slice(&slots);
        }
        w
    }

    fn words(&self) -> Vec<u32> {
        let o = self.grids.len() as u32;
        let mut w = vec![0u32; work::HEADER as usize];
        w[work::OBJECT_GROUPS as usize] = o;
        w[work::SLOT_WORDS as usize] = self.slot_words;
        w[work::GROUP_WORDS as usize] = self.group_words;
        w[work::GROUP_VERTICES as usize] = self.group_vertices;
        w[work::POOL_WORDS as usize] = self.pool_words;
        w[work::DRAW_ID as usize] = 7;
        w[work::ROW as usize] = self.row;
        w[work::ARGUMENTS as usize] = self.arguments;
        match self.pooled {
            Some(at) => {
                w[work::SLOTS as usize] = at + 1;
                w.push(0xEEEE_EEEE);
            }
            None => w.extend(self.slots()),
        }
        w
    }
}

/// What a scan left: the work words, the status report, the pool's cursor, and the pool.
struct Scanned {
    work: Vec<u32>,
    report: StatusReport,
    cursor: u32,
    pool: Vec<u32>,
}

/// The pool's words in the tests.
const POOL: usize = 4096;

/// Run the scan of `draw` over `arguments` (bound whole) with the pool's cursor at `cursor` and
/// the status bits `bits`, in both invocation orders.
fn scan(draw: &Draw, arguments: &[u32], cursor: u32, bits: u32) -> Scanned {
    let module = scan_typed_module().expect("scan");
    let mut status = [0u32; 8];
    status[StatusWord::Status.index() as usize] = bits;
    status[StatusWord::MeshRecordsCursor.index() as usize] = cursor;
    let mut ran = Vec::new();
    for order in [Order::Forward, Order::Reverse] {
        let res = Resources {
            buffers: vec![
                Buffer {
                    slot: Some(WORK),
                    address: None,
                    bytes: bytes(&draw.words()),
                },
                Buffer {
                    slot: Some(ARGUMENTS),
                    address: None,
                    bytes: bytes(arguments),
                },
                Buffer {
                    slot: Some(STATUS),
                    address: None,
                    bytes: bytes(&status),
                },
                Buffer {
                    slot: Some(RECORDS),
                    address: None,
                    bytes: bytes(&draw.pool(POOL)),
                },
            ],
            push: Vec::new(),
            spec: Default::default(),
        };
        let cfg = Config {
            order,
            ..Config::default()
        };
        ran.push(run(&module, &cfg, res).expect("scan run").buffers);
    }
    assert_eq!(ran[0], ran[1], "order independent");
    let after = &ran[0];
    let status_bytes: [u8; 32] = after[2].as_slice().try_into().expect("status");
    let status = words(&after[2]);
    Scanned {
        work: words(&after[0]),
        report: ExecutionStatus::decode(&status_bytes),
        cursor: status[StatusWord::MeshRecordsCursor.index() as usize],
        pool: words(&after[3]),
    }
}

/// The offsets after `draw`'s slots, where they lie.
fn offsets(draw: &Draw, s: &Scanned) -> Vec<u32> {
    let slots = draw.grids.len() * draw.slot_words as usize;
    match draw.pooled {
        Some(at) => s.pool[at as usize + slots..][..draw.grids.len() + 1].to_vec(),
        None => s.work[work::HEADER as usize + slots..].to_vec(),
    }
}

fn header(work: &[u32], at: u32, n: usize) -> Vec<u32> {
    work[at as usize..at as usize + n].to_vec()
}

#[test]
fn the_scan_is_valid_on_every_committed_profile() {
    for (_, p) in committed_profiles().expect("profiles") {
        assert_valid(scan_typed_module().expect("scan"), &p);
    }
}

/// Each object threadgroup's first mesh threadgroup in object order, and `T`; the records
/// reserved after the cursor; `T` folded into rows of `ROW`; `T · NP · k` vertices. A grid with
/// an axis of 0 launches none, and one past Metal's limits none (65,536 along an axis, or 2^20 in
/// all: the object function holds it so; the scan holds it again).
#[test]
fn the_scan_places_each_object_threadgroups_mesh_threadgroups() {
    let draw = Draw::of(&[
        [2, 1, 1],
        [0, 5, 1],
        [3, 2, 1],
        [1, 1, 1],
        [1 << 20, 2, 1],
        [1, 65536, 1],
        [1024, 1024, 1],
    ]);
    let s = scan(&draw, &[0], 40, 0);
    assert_eq!(offsets(&draw, &s), vec![0, 2, 2, 8, 9, 9, 9, 9]);
    assert_eq!(s.work[work::MESH_GROUPS as usize], 9);
    assert_eq!(s.work[work::RECORDS_BASE as usize], 40);
    assert_eq!(s.cursor, 40 + 9 * 10);
    assert_eq!(header(&s.work, work::DISPATCH, 3), vec![4, 3, 1]);
    assert_eq!(header(&s.work, work::DRAW, 4), vec![27, 1, 0, 0]);
    assert_eq!(s.report, StatusReport::default());
    // The slots are the object function's: the scan leaves them.
    assert_eq!(&s.work[work::HEADER as usize..][..3], &[2, 1, 1]);
}

/// More object threadgroups than invocations: every run of them is placed after the runs
/// before.
#[test]
fn the_scan_places_runs_of_many_object_threadgroups() {
    let grids: Vec<[u32; 3]> = (0..200u32).map(|i| [i % 3, 1, 1]).collect();
    let mut draw = Draw::of(&grids);
    draw.pool_words = 100_000;
    draw.row = 65535;
    let s = scan(&draw, &[0], 0, 0);
    let mut want = Vec::new();
    let mut running = 0;
    for g in &grids {
        want.push(running);
        running += g[0];
    }
    want.push(running);
    assert_eq!(offsets(&draw, &s), want);
    assert_eq!(s.work[work::MESH_GROUPS as usize], running);
    assert_eq!(header(&s.work, work::DISPATCH, 3), vec![running, 1, 1]);
}

/// An indirect draw's grid is Metal's arguments at their word, into slot 0; one past Metal's
/// limit launches nothing; arguments past the bound range read 0.
#[test]
fn an_indirect_scan_reads_metals_arguments() {
    let mut draw = Draw::of(&[[0, 0, 0]]);
    draw.arguments = 1 + 2;
    let s = scan(&draw, &[9, 9, 3, 2, 2], 0, 0);
    assert_eq!(&s.work[work::HEADER as usize..][..3], &[3, 2, 2]);
    assert_eq!(offsets(&draw, &s), vec![0, 12]);
    assert_eq!(s.work[work::MESH_GROUPS as usize], 12);
    assert_eq!(header(&s.work, work::DRAW, 4), vec![36, 1, 0, 0]);
    for past in [
        [1 << 20, 2, 1],
        [65536, 1, 1],
        [1, 1, 65536],
        [1024, 1024, 1],
    ] {
        let s = scan(&draw, &[9, 9, past[0], past[1], past[2]], 0, 0);
        assert_eq!(
            &s.work[work::HEADER as usize..][..3],
            &[0, 0, 0],
            "{past:?}"
        );
        assert_eq!(s.work[work::MESH_GROUPS as usize], 0, "{past:?}");
        assert_eq!(
            header(&s.work, work::DISPATCH, 3),
            vec![0, 0, 1],
            "{past:?}"
        );
    }
    let s = scan(&draw, &[9, 9, 3, 2], 0, 0);
    assert_eq!(s.work[work::MESH_GROUPS as usize], 0);
}

/// A draw whose records the rest of the pool cannot hold draws nothing, keeps the cursor, sets
/// the bit and names itself; a later fault leaves the first one's words.
#[test]
fn a_draw_the_pool_cannot_hold_draws_nothing_and_says_so() {
    let draw = Draw::of(&[[4, 1, 1], [3, 1, 1]]);
    let s = scan(&draw, &[0], 940, 0);
    assert_eq!(s.work[work::MESH_GROUPS as usize], 0);
    assert_eq!(header(&s.work, work::DISPATCH, 3), vec![0, 0, 1]);
    assert_eq!(header(&s.work, work::DRAW, 4), vec![0, 1, 0, 0]);
    assert_eq!(s.cursor, 940);
    assert_eq!(
        s.report.mesh_records_overflow,
        Some(MeshRecordsFault {
            draw: 7,
            object_threadgroups: 2,
            mesh_threadgroups: 7,
        })
    );
    // Exactly the room left: drawn.
    let s = scan(&draw, &[0], 930, 0);
    assert_eq!(s.work[work::MESH_GROUPS as usize], 7);
    assert_eq!(s.cursor, 1000);
    assert_eq!(s.report.mesh_records_overflow, None);
    // A later fault leaves the first one's words (here all zero).
    let s = scan(&draw, &[0], 940, StatusBits::MESH_RECORDS_OVERFLOW.0);
    assert_eq!(
        s.report.mesh_records_overflow,
        Some(MeshRecordsFault::default())
    );
    // Vertices past a `u32` draw nothing either.
    let mut wide = Draw::of(&[[1023, 1025, 1]]);
    wide.pool_words = u32::MAX;
    wide.group_words = 1;
    wide.group_vertices = 1 << 13;
    let s = scan(&wide, &[0], 0, 0);
    assert_eq!(s.work[work::MESH_GROUPS as usize], 0);
    assert_eq!(
        s.report.mesh_records_overflow.map(|f| f.mesh_threadgroups),
        Some(1023 * 1025)
    );
}

/// Slots in the records pool scan as slots in the work buffer do, and the offsets land after
/// them there; the work buffer keeps its own words.
#[test]
fn the_scan_reads_slots_in_the_records_pool() {
    let grids = [[2, 1, 1], [0, 5, 1], [3, 2, 1], [1, 1, 1], [1, 65536, 1]];
    let mut draw = Draw::of(&grids);
    draw.pooled = Some(200);
    let s = scan(&draw, &[0], 300, 0);
    assert_eq!(offsets(&draw, &s), vec![0, 2, 2, 8, 9, 9]);
    assert_eq!(s.work[work::MESH_GROUPS as usize], 9);
    assert_eq!(s.work[work::RECORDS_BASE as usize], 300);
    assert_eq!(s.cursor, 300 + 9 * 10);
    assert_eq!(header(&s.work, work::DISPATCH, 3), vec![4, 3, 1]);
    assert_eq!(s.report, StatusReport::default());
    assert_eq!(&s.work[work::HEADER as usize..], &[0xEEEE_EEEE]);
    assert_eq!(&s.pool[200..][..3], &[2, 1, 1]);
    // Below the slots, the pool is as it was.
    assert!(s.pool[..200].iter().all(|&w| w == 0x5555_5555));
}

/// An object arguments pass: Metal's arguments `arguments` at word `at` (0: none), slots of 6
/// words, a pool of `pool_words` with its cursor at `cursor`, the status bits `bits`, and the
/// device's `compute` limits.
struct Arguments {
    arguments: Vec<u32>,
    at: u32,
    pool_words: u32,
    cursor: u32,
    bits: u32,
    compute: [u32; 3],
}

impl Arguments {
    fn of(grid: [u32; 3]) -> Arguments {
        let mut arguments = vec![9];
        arguments.extend(grid);
        Arguments {
            arguments,
            at: 1,
            pool_words: 1000,
            cursor: 40,
            bits: 0,
            compute: [65535; 3],
        }
    }

    /// The work words before the pass.
    fn work(&self) -> Vec<u32> {
        let mut w = vec![0xAAAA_AAAA; work::HEADER as usize + 1];
        w[work::OBJECT_ARGUMENTS as usize] = self.at + 1;
        w[work::SLOT_WORDS as usize] = 6;
        w[work::POOL_WORDS as usize] = self.pool_words;
        w[work::DRAW_ID as usize] = 7;
        w
    }

    /// Run the pass; the work words, the status report and the pool's cursor after it.
    fn run(&self) -> Scanned {
        let module = object_arguments_typed_module(self.compute).expect("arguments");
        let mut status = [0u32; 8];
        status[StatusWord::Status.index() as usize] = self.bits;
        status[StatusWord::MeshRecordsCursor.index() as usize] = self.cursor;
        let pool = vec![0x5555_5555; 16];
        let res = Resources {
            buffers: vec![
                Buffer {
                    slot: Some(WORK),
                    address: None,
                    bytes: bytes(&self.work()),
                },
                Buffer {
                    slot: Some(ARGUMENTS),
                    address: None,
                    bytes: bytes(&self.arguments),
                },
                Buffer {
                    slot: Some(STATUS),
                    address: None,
                    bytes: bytes(&status),
                },
                Buffer {
                    slot: Some(RECORDS),
                    address: None,
                    bytes: bytes(&pool),
                },
            ],
            push: Vec::new(),
            spec: Default::default(),
        };
        let after = run(&module, &Config::default(), res)
            .expect("arguments run")
            .buffers;
        let status_bytes: [u8; 32] = after[2].as_slice().try_into().expect("status");
        let status = words(&after[2]);
        Scanned {
            work: words(&after[0]),
            report: ExecutionStatus::decode(&status_bytes),
            cursor: status[StatusWord::MeshRecordsCursor.index() as usize],
            pool: words(&after[3]),
        }
    }
}

/// The words a pass that runs nothing leaves: no object threadgroups, no slots, an empty
/// dispatch, and the cursor where it was.
fn runs_nothing(a: &Arguments, s: &Scanned) {
    assert_eq!(s.work[work::OBJECT_GROUPS as usize], 0);
    assert_eq!(s.work[work::SLOTS as usize], 0);
    assert_eq!(header(&s.work, work::OBJECT_DISPATCH, 3), vec![0, 0, 0]);
    assert_eq!(s.cursor, a.cursor);
}

#[test]
fn the_object_arguments_pass_is_valid_on_every_committed_profile() {
    for (_, p) in committed_profiles().expect("profiles") {
        let compute = p.fields().limits.max_compute_work_group_count;
        assert_valid(
            object_arguments_typed_module(compute).expect("arguments"),
            &p,
        );
    }
}

/// A grid that runs: its slots and offsets reserved after the cursor (`n · S + n + 1` words), the
/// slots' first word plus 1, and the grid as the object dispatch; the pool is the scan's to write.
#[test]
fn the_object_arguments_pass_reserves_a_grids_slots() {
    let a = Arguments::of([3, 2, 2]);
    let s = a.run();
    assert_eq!(s.work[work::OBJECT_GROUPS as usize], 12);
    assert_eq!(s.work[work::SLOTS as usize], 41);
    assert_eq!(header(&s.work, work::OBJECT_DISPATCH, 3), vec![3, 2, 2]);
    assert_eq!(s.cursor, 40 + 12 * 6 + 13);
    assert_eq!(s.report, StatusReport::default());
    assert!(s.pool.iter().all(|&w| w == 0x5555_5555));
    // The arguments at another word, and exactly the room left.
    let mut a = Arguments::of([3, 2, 2]);
    a.arguments.insert(0, 1);
    a.at = 2;
    a.cursor = 1000 - 85;
    let s = a.run();
    assert_eq!(s.work[work::OBJECT_GROUPS as usize], 12);
    assert_eq!(s.cursor, 1000);
    assert_eq!(s.report, StatusReport::default());
}

/// A grid with an axis of 0 runs nothing and says nothing, however large its other axes; so do
/// arguments past the bound range.
#[test]
fn the_object_arguments_pass_runs_an_empty_grid_silently() {
    for grid in [[3, 0, 2], [0, 1, 1], [1 << 20, 1 << 20, 0]] {
        let a = Arguments::of(grid);
        let s = a.run();
        runs_nothing(&a, &s);
        assert_eq!(s.report, StatusReport::default(), "{grid:?}");
    }
    let mut a = Arguments::of([3, 2, 2]);
    a.arguments.pop();
    let s = a.run();
    runs_nothing(&a, &s);
    assert_eq!(s.report, StatusReport::default());
}

/// More than 2^29 threadgroups in all (a wrapping product too): Metal discards the command
/// buffer, so the pass runs nothing and sets the discard bit alone; exactly 2^29 runs.
#[test]
fn the_object_arguments_pass_discards_what_metal_discards() {
    for grid in [[1 << 15, 1 << 15, 1], [(1 << 29) + 1, 1, 1], [65535; 3]] {
        let mut a = Arguments::of(grid);
        a.compute = [u32::MAX; 3];
        let s = a.run();
        runs_nothing(&a, &s);
        assert!(s.report.mesh_grid_discarded, "{grid:?}");
        assert!(!s.report.mesh_grid_past_limits, "{grid:?}");
        assert_eq!(s.report.mesh_records_overflow, None, "{grid:?}");
    }
    let mut a = Arguments::of([1 << 29, 1, 1]);
    a.compute = [u32::MAX; 3];
    a.pool_words = u32::MAX;
    a.cursor = 0;
    let s = a.run();
    assert_eq!(s.work[work::OBJECT_GROUPS as usize], 1 << 29);
    assert_eq!(s.cursor, (1 << 29) * 7 + 1);
    assert_eq!(s.report, StatusReport::default());
}

/// An axis past the device's compute limits runs nothing and sets the past-limits bit; a grid
/// whose threads per grid wrap runs whole (the object function reports the wrapped grid).
#[test]
fn the_object_arguments_pass_holds_the_device_limits() {
    for (grid, compute) in [([65536, 1, 1], [65535; 3]), ([1, 1, 3], [65535, 65535, 2])] {
        let mut a = Arguments::of(grid);
        a.compute = compute;
        let s = a.run();
        runs_nothing(&a, &s);
        assert!(s.report.mesh_grid_past_limits, "{grid:?}");
        assert!(!s.report.mesh_grid_discarded, "{grid:?}");
    }
    for grid in [[1 << 22, 1, 1], [1, 1 << 23, 1], [65535, 1, 1]] {
        let mut a = Arguments::of(grid);
        a.compute = [u32::MAX; 3];
        a.pool_words = u32::MAX;
        let s = a.run();
        let n = grid.iter().product::<u32>();
        assert_eq!(s.work[work::OBJECT_GROUPS as usize], n, "{grid:?}");
        assert_eq!(header(&s.work, work::OBJECT_DISPATCH, 3), grid.to_vec());
        assert_eq!(s.report, StatusReport::default(), "{grid:?}");
    }
}

/// Slots the rest of the pool cannot hold: nothing runs, the cursor stays, and the first such
/// draw names itself (no mesh threadgroups yet); a later fault leaves the first one's words.
#[test]
fn the_object_arguments_pass_says_when_the_pool_is_full() {
    let mut a = Arguments::of([3, 2, 2]);
    a.cursor = 1000 - 84;
    let s = a.run();
    runs_nothing(&a, &s);
    assert_eq!(
        s.report.mesh_records_overflow,
        Some(MeshRecordsFault {
            draw: 7,
            object_threadgroups: 12,
            mesh_threadgroups: 0,
        })
    );
    // A cursor already past the pool.
    a.cursor = 5000;
    let s = a.run();
    runs_nothing(&a, &s);
    assert!(s.report.mesh_records_overflow.is_some());
    a.bits = StatusBits::MESH_RECORDS_OVERFLOW.0;
    let s = a.run();
    assert_eq!(
        s.report.mesh_records_overflow,
        Some(MeshRecordsFault::default())
    );
}

/// The pass, the object function's slots and the scan in turn: the scan places the threadgroups
/// of the slots the pass reserved, after them in the pool.
#[test]
fn the_scan_places_the_grids_of_the_slots_the_pass_reserved() {
    let a = Arguments::of([2, 2, 1]);
    let s = a.run();
    let at = s.work[work::SLOTS as usize] - 1;
    let grids = [[2, 1, 1], [0, 5, 1], [3, 2, 1], [1, 1, 1]];
    let mut draw = Draw::of(&grids);
    draw.pooled = Some(at);
    let scanned = scan(&draw, &[0], s.cursor, 0);
    assert_eq!(offsets(&draw, &scanned), vec![0, 2, 2, 8, 9]);
    assert_eq!(scanned.work[work::RECORDS_BASE as usize], s.cursor);
    assert_eq!(scanned.cursor, s.cursor + 9 * 10);
}
