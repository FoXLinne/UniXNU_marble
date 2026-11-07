//! The index gather pass: its modules valid on every committed profile (L3, `G-val`); on the CPU
//! interpreter, each draw of a run gets the list the index rewrite pass writes for it
//! ([`RewriteProgram::list`] of its fetched positions in the run's order), every program that
//! gathers, either index size, offsets and counts reaching past the buffer's whole indices, and
//! a dispatch of fewer workgroups than the run's; an indirect run's plan places each draw's list
//! in the pool and writes its command from Metal's arguments (first indices past the buffer, no
//! instance, a pool too small for a list); the positions' list of fewer positions is the start of
//! the list of more for exactly the programs that gather, and its closed form is the list; and a
//! run refuses what it cannot take.

use super::plan::{plan_typed_module, IndexGatherPlanModule};
use super::*;
use crate::bind::index_bounds::IndexBoundsPass;
use crate::bind::index_rewrite::{RewriteOrder, STATUS};
use crate::bind::status::ExecutionStatus;
use crate::spirv::interp::{run, Buffer, Config, Order, Resources};
use crate::spirv::testing::{assert_valid, committed_profiles};

/// The storage buffer offset alignment of these tests.
const ALIGNMENT: u64 = 16;

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

#[test]
fn the_modules_are_valid_on_every_committed_profile() {
    for (_, p) in committed_profiles().expect("profiles") {
        assert_valid(gather_typed_module().expect("module"), &p);
        assert_valid(plan_typed_module().expect("plan"), &p);
    }
}

/// Every program but a line loop: its pattern's positions are its positions' list, and its
/// capacity terms its capacity, for every count to 80 and a few past a `u32`'s room.
#[test]
fn patterns_and_terms_are_the_lists() {
    let orders = [
        RewriteOrder::First,
        RewriteOrder::Last,
        RewriteOrder::LastFirst,
    ];
    for program in RewriteProgram::ALL {
        let terms = program.capacity_terms();
        assert_eq!(terms.is_some(), program.gathers_without_indices());
        let Some((lead, step, per)) = terms else {
            assert_eq!(program.gather_pattern(RewriteOrder::First), None);
            continue;
        };
        for n in (0..80u64).chain([u64::from(u32::MAX) - 7, u64::from(u32::MAX)]) {
            let units = n.saturating_sub(u64::from(lead)) / u64::from(step);
            assert_eq!(
                u64::from(per) * units,
                program.capacity(n),
                "{program:?} {n}"
            );
        }
        for order in orders {
            let (period, mult, add) = program.gather_pattern(order).expect("a pattern");
            for n in 0..80 {
                let list = program.sequential_list(n, order);
                assert_eq!(list.len() as u64, program.capacity(u64::from(n)));
                let formed: Vec<u32> = (0..list.len() as u32)
                    .map(|o| {
                        let r = (o % period) as usize;
                        mult[r] * (o / period) + add[r]
                    })
                    .collect();
                assert_eq!(formed, list, "{program:?} {order:?} {n}");
            }
        }
    }
}

/// The programs whose positions' list for fewer positions is the start of the list for more:
/// those that gather, and not a line loop or the strips.
#[test]
fn the_gathering_programs_lists_start_alike() {
    for program in RewriteProgram::ALL {
        for order in [
            RewriteOrder::First,
            RewriteOrder::Last,
            RewriteOrder::LastFirst,
        ] {
            let most = program.sequential_list(40, order);
            let starts = (0..40).all(|n| {
                let list = program.sequential_list(n, order);
                most.starts_with(&list)
            });
            // The strips start alike too, but restart where an index is `k'`: they never gather.
            let expected = starts && !program.restarts();
            assert_eq!(program.gathers(), expected, "{program:?} {order:?}");
        }
    }
}

/// One draw of a run: Metal's count, its offset and its index size.
struct Draw {
    count: u32,
    offset: u64,
    wide: bool,
}

/// Where the Metal buffer starts in the `VkBuffer` of these tests, after bytes no position may
/// read.
const BASE: u64 = 48;

/// The run of `draws` of `program` in `order` over one index buffer of `buffer` bytes (at
/// [`BASE`] in its `VkBuffer`), each draw's fetch stopping at `fetch_end` of them, as the
/// interpreter runs its dispatch, in both workgroup orders, and over two workgroups where the run
/// has more: each draw's list.
fn gathered(
    program: RewriteProgram,
    order: RewriteOrder,
    (buffer, fetch_end): (&[u8], u64),
    draws: &[Draw],
) -> Vec<Vec<u32>> {
    let output_words = 4096;
    let end = BASE + buffer.len() as u64;
    let fetch_end = BASE + fetch_end;
    let mut run_of = IndexGather::new(
        (program, order),
        (BASE, end),
        (ALIGNMENT, u32::MAX),
        output_words,
    )
    .expect("the program gathers");
    let firsts: Vec<(u32, u32)> = draws
        .iter()
        .map(|d| {
            let capacity = program.capacity(u64::from(d.count)) as u32;
            let first = run_of
                .push(GatherDraw {
                    count: d.count,
                    index_offset: BASE + d.offset,
                    index_end: fetch_end,
                    wide: d.wide,
                })
                .expect("the run takes the draw");
            (first, capacity)
        })
        .collect();
    let pattern = run_of.pattern();
    // The `VkBuffer`: bytes before the Metal buffer, which runs on to a multiple of 16 bytes (the
    // padding), of bytes no position may read as an index.
    let mut padded = vec![0xAB; BASE as usize];
    padded.extend_from_slice(buffer);
    padded.resize(padded.len().next_multiple_of(16).max(16), 0xCC);
    let indices = match run_of.indices_range() {
        Some(range) => {
            padded[range.offset as usize..(range.offset + range.range) as usize].to_vec()
        }
        None => vec![0xCC; 4],
    };
    let module = gather_typed_module().expect("module");
    let mut ran = Vec::new();
    let dispatches = [
        (Order::Forward, run_of.groups()),
        (Order::Reverse, run_of.groups()),
        (Order::Forward, run_of.groups().min(2)),
    ];
    for (order, workgroups) in dispatches {
        let buffers = [
            (TABLE, run_of.table_bytes()),
            (PATTERN, bytes(&pattern)),
            (INDICES, indices.clone()),
            (OUTPUT, bytes(&vec![0xDDDD_DDDD; output_words as usize])),
        ];
        let res = Resources {
            buffers: buffers
                .into_iter()
                .map(|(slot, bytes)| Buffer {
                    slot: Some(slot),
                    address: None,
                    bytes,
                })
                .collect(),
            push: Vec::new(),
            spec: Default::default(),
        };
        let cfg = Config {
            order,
            workgroups: [workgroups, 1, 1],
            ..Config::default()
        };
        ran.push(run(&module, &cfg, res).expect("gather run").buffers);
    }
    assert_eq!(ran[0], ran[1], "order independent");
    assert_eq!(ran[0], ran[2], "fewer workgroups");
    let out = words(&ran[0][3]);
    // Nothing past the lists is written.
    assert!(
        out[run_of.written() as usize..]
            .iter()
            .all(|&w| w == 0xDDDD_DDDD),
        "past the lists"
    );
    firsts
        .into_iter()
        .map(|(first, capacity)| out[first as usize..(first + capacity) as usize].to_vec())
        .collect()
}

/// What the rewrite pass writes for `draw` over `buffer`: [`RewriteProgram::list`] of its
/// positions, each its index where it lies inside the buffer's whole indices, else 0.
fn expected(program: RewriteProgram, order: RewriteOrder, buffer: &[u8], draw: &Draw) -> Vec<u32> {
    let size = if draw.wide { 4 } else { 2 };
    let values: Vec<u32> = (0..u64::from(draw.count))
        .map(|q| {
            let at = draw.offset + q * size;
            match at + size <= buffer.len() as u64 {
                true if draw.wide => {
                    let b = &buffer[at as usize..at as usize + 4];
                    u32::from_le_bytes(b.try_into().expect("4 bytes"))
                }
                true => {
                    let b = &buffer[at as usize..at as usize + 2];
                    u32::from(u16::from_le_bytes(b.try_into().expect("2 bytes")))
                }
                false => 0,
            }
        })
        .collect();
    program.list(&values, u32::MAX, order)
}

/// Every program that gathers, in either order it is drawn in, over 16- and 32-bit indices: draws
/// of counts from a whole primitive to two workgroups, with positions past a whole primitive, at
/// offsets in the buffer, reaching past it and wholly past it, each list the rewrite pass's.
#[test]
fn each_draw_gets_the_rewrite_pass_s_list() {
    // An index buffer whose index at byte b is distinct: 300 words.
    let buffer: Vec<u8> = (0..1200u32).map(|b| (b * 37 + 11) as u8).collect();
    let programs = RewriteProgram::ALL.into_iter().filter(|p| p.gathers());
    for program in programs {
        // A gathering program's order at mode 1 is its order at mode 0 (only a triangle strip's
        // odd triangle differs).
        for order in [RewriteOrder::First, RewriteOrder::LastFirst] {
            for wide in [false, true] {
                let size = if wide { 4 } else { 2 };
                let mut draws = Vec::new();
                for (i, count) in [3, 4, 7, 13, 140].into_iter().enumerate() {
                    let offset = (i as u64 * 52) / size * size;
                    if program.capacity(u64::from(count)) > 0 {
                        draws.push(Draw {
                            count,
                            offset,
                            wide,
                        });
                    }
                }
                // Reaching past the buffer, and wholly past it.
                for (count, offset) in [(24, 1188), (8, 4000)] {
                    draws.push(Draw {
                        count,
                        offset: offset / size * size,
                        wide,
                    });
                }
                let lists = gathered(program, order, (&buffer, buffer.len() as u64), &draws);
                for (draw, list) in draws.iter().zip(&lists) {
                    assert_eq!(
                        list,
                        &expected(program, order, &buffer, draw),
                        "{program:?} {order:?} wide {wide} count {} offset {}",
                        draw.count,
                        draw.offset
                    );
                }
            }
        }
    }
}

/// Draws of either index size share a run over one buffer.
#[test]
fn index_sizes_share_a_run() {
    let buffer: Vec<u8> = (0..256u32).map(|b| (b * 5 + 3) as u8).collect();
    let program = RewriteProgram::TriangleList;
    let order = RewriteOrder::LastFirst;
    let draws = [
        Draw {
            count: 6,
            offset: 2,
            wide: false,
        },
        Draw {
            count: 9,
            offset: 40,
            wide: true,
        },
        Draw {
            count: 3,
            offset: 250,
            wide: false,
        },
    ];
    let lists = gathered(program, order, (&buffer, buffer.len() as u64), &draws);
    for (draw, list) in draws.iter().zip(&lists) {
        assert_eq!(list, &expected(program, order, &buffer, draw));
    }
}

/// A run refuses a draw of no primitive, an offset not a multiple of the index size, indices
/// outside its bytes, a list past its output, and workgroups past the most a dispatch takes, and
/// is unchanged after; a program that does not gather, or bytes past the device's range, have no
/// run.
#[test]
fn a_run_refuses_what_it_cannot_take() {
    let draw = |count, index_offset, wide| GatherDraw {
        count,
        index_offset,
        index_end: 4096,
        wide,
    };
    let program = (RewriteProgram::TriangleList, RewriteOrder::LastFirst);
    for other in RewriteProgram::ALL.into_iter().filter(|p| !p.gathers()) {
        let run = IndexGather::new(
            (other, RewriteOrder::First),
            (0, 64),
            (ALIGNMENT, 1 << 16),
            1000,
        );
        assert_eq!(run, None, "{other:?}");
    }
    let past = IndexGather::new(program, (0, 1 << 17), (ALIGNMENT, 1 << 16), 1000);
    assert_eq!(past, None, "past the range");
    let mut run = IndexGather::new(program, (64, 4096), (ALIGNMENT, 1 << 16), 1000).expect("run");
    assert_eq!(run.push(draw(2, 64, false)), None, "no triangle");
    assert_eq!(run.push(draw(3, 65, false)), None, "odd offset");
    assert_eq!(
        run.push(draw(3, 66, true)),
        None,
        "offset not a multiple of 4"
    );
    assert_eq!(run.push(draw(3, 32, true)), None, "before the bytes");
    assert_eq!(run.push(draw(600, 64, false)), Some(0));
    assert_eq!(run.push(draw(600, 64, false)), None, "past the output");
    assert_eq!(run.push(draw(300, 128, true)), Some(600));
    assert_eq!((run.len(), run.written(), run.groups()), (2, 900, 5 + 3));
    // Wholly past the bytes: reads no index, so it lies nowhere in them.
    assert_eq!(run.push(draw(3, 8192, true)), Some(900));
    let range = run.indices_range().expect("a range");
    assert_eq!((range.offset, range.range), (64, 4032));
    let table = run.table();
    let record = |i: usize| {
        let at =
            (IndexGatherModule::HEADER_WORDS + IndexGatherModule::RECORD_WORDS * i as u32) as usize;
        table[at..at + 8].to_vec()
    };
    assert_eq!(table[..3], [3, 0, 9]);
    assert_eq!(record(0), [0, 600, 0, 0, 600, 0, 0, 0]);
    assert_eq!(record(1), [5, 300, 600, 64, 300, 1, 0, 0]);
    assert_eq!(record(2), [8, 3, 900, 0, 0, 1, 0, 0]);
    let most = IndexGatherModule::MAX_GROUPS * IndexGatherModule::LOCAL_SIZE;
    let mut wide = IndexGather::new(program, (0, 0), (ALIGNMENT, u32::MAX), u32::MAX).expect("run");
    assert_eq!(wide.indices_range(), None, "no bytes");
    assert_eq!(wide.push(draw(most, 8192, true)), Some(0));
    assert_eq!(
        wide.push(draw(3, 8192, true)),
        None,
        "workgroups past the most"
    );
    // A direct run takes no indirect draw.
    let indirect = IndirectGatherDraw {
        arguments_offset: 0,
        index_offset: 64,
        index_end: 4096,
        wide: false,
    };
    assert_eq!(wide.push_indirect(indirect), None);
}

/// An indirect run refuses a program that does not gather draws of its kind, arguments outside
/// its bytes or at an offset not a multiple of 4, an index offset not a multiple of the index
/// size, indices outside its bytes, a direct draw, and draws past the plan's most.
#[test]
fn an_indirect_run_refuses_what_it_cannot_take() {
    let limits = (ALIGNMENT, 1 << 16);
    let indexed = Some((64, 4096));
    for program in RewriteProgram::ALL {
        let order = RewriteOrder::LastFirst;
        let with = IndexGather::indirect((program, order), (0, 400), indexed, limits, 1024);
        assert_eq!(with.is_some(), program.gathers(), "{program:?}");
        let without = IndexGather::indirect((program, order), (0, 400), None, limits, 1024);
        assert_eq!(
            without.is_some(),
            program.gathers_without_indices(),
            "{program:?}"
        );
    }
    let program = (RewriteProgram::TriangleList, RewriteOrder::LastFirst);
    let mut run = IndexGather::indirect(program, (16, 400), indexed, limits, 1024).expect("run");
    assert!(run.is_indirect());
    let draw = |arguments_offset, index_offset, wide| IndirectGatherDraw {
        arguments_offset,
        index_offset,
        index_end: 4096,
        wide,
    };
    assert_eq!(
        run.push_indirect(draw(18, 64, false)),
        None,
        "arguments' offset"
    );
    assert_eq!(
        run.push_indirect(draw(8, 64, false)),
        None,
        "before the arguments"
    );
    assert_eq!(
        run.push_indirect(draw(384, 64, false)),
        None,
        "past the arguments"
    );
    assert_eq!(
        run.push_indirect(draw(16, 65, false)),
        None,
        "odd index offset"
    );
    assert_eq!(
        run.push_indirect(draw(16, 32, true)),
        None,
        "before the indices"
    );
    assert_eq!(run.push_indirect(draw(16, 64, true)), Some(0));
    assert_eq!(run.push_indirect(draw(36, 66, false)), Some(1));
    // Wholly past the index bytes: reads no index.
    assert_eq!(run.push_indirect(draw(56, 8192, true)), Some(2));
    let direct = GatherDraw {
        count: 3,
        index_offset: 64,
        index_end: 4096,
        wide: false,
    };
    assert_eq!(run.push(direct), None, "a direct draw");
    let range = run.arguments_range().expect("arguments");
    assert_eq!((range.offset, range.range), (16, 384));
    let table = run.table();
    assert_eq!(table[..10], [3, 0, 0, 1024, 0, 0, 0, 0, 3, 3]);
    let record = |i: usize| {
        let at =
            (IndexGatherModule::HEADER_WORDS + IndexGatherModule::RECORD_WORDS * i as u32) as usize;
        table[at..at + 8].to_vec()
    };
    assert_eq!(record(0), [0, 0, 0, 0, 1008, 1, 0, 0]);
    assert_eq!(record(1), [0, 0, 0, 2, 2015, 0, 0, 5]);
    assert_eq!(record(2), [0, 0, 0, 0, 0, 1, 0, 10]);
    let mut full = IndexGather::indirect(program, (0, 1 << 15), None, limits, 1024).expect("run");
    for i in 0..IndexGatherPlanModule::MAX_DRAWS {
        assert_eq!(
            full.push_indirect(draw(u64::from(i) * 16, 0, false)),
            Some(i)
        );
    }
    assert_eq!(full.push_indirect(draw(0, 0, false)), None, "past the most");
}

/// A 16-bit buffer of an odd number of indices: Metal's fetch reads its partial last index from
/// the bytes up to the next whole word (`IndexBufferRef::fetch_end`), which the run binds.
#[test]
fn a_partial_last_index_reads_its_word() {
    let buffer: Vec<u8> = (0..10u8).map(|b| b * 17 + 1).collect();
    let program = RewriteProgram::TriangleList;
    let draw = Draw {
        count: 6,
        offset: 0,
        wide: false,
    };
    let lists = gathered(program, RewriteOrder::First, (&buffer, 12), &[draw]);
    // The sixth index is the padding's two bytes past the length.
    let mut padded = buffer.clone();
    padded.extend([0xCC, 0xCC]);
    let draw = Draw {
        count: 6,
        offset: 0,
        wide: false,
    };
    assert_eq!(
        lists[0],
        expected(program, RewriteOrder::First, &padded, &draw)
    );
    assert_eq!(lists[0][5], 0xCCCC);
}

/// One indirect draw of a test: its Metal arguments (four words without indices), and an indexed
/// draw's index offset in the buffer and index size.
#[derive(Clone, Debug)]
struct Indirect {
    arguments: Vec<u32>,
    offset: u64,
    wide: bool,
}

/// What an indirect run's plan and gather left: each draw's command and list, the pool's counter,
/// and whether the plan reported an overflow.
struct Planned {
    draws: Vec<([u32; 5], Vec<u32>)>,
    counter: u32,
    overflow: bool,
}

/// Where the arguments start in their `VkBuffer`, and each draw's arguments' stride.
const ARGUMENTS_BASE: u64 = 32;
const ARGUMENTS_STRIDE: u64 = 24;

/// The indirect run of `draws` of `program` in `order` (over `buffer`, an index buffer at [`BASE`]
/// of its `VkBuffer`, or without indices) into a pool of `pool_bytes` index bytes whose counter
/// starts at `counter`, as the interpreter runs the plan and then the gather over the plan's
/// dispatch, in both workgroup orders.
fn planned(
    (program, order): (RewriteProgram, RewriteOrder),
    buffer: Option<&[u8]>,
    draws: &[Indirect],
    (pool_bytes, counter): (u32, u32),
) -> Planned {
    let end = ARGUMENTS_BASE + ARGUMENTS_STRIDE * draws.len() as u64;
    let indices = buffer.map(|b| (BASE, BASE + b.len() as u64));
    let mut run_of = IndexGather::indirect(
        (program, order),
        (ARGUMENTS_BASE, end),
        indices,
        (ALIGNMENT, u32::MAX),
        pool_bytes,
    )
    .expect("the program gathers");
    let mut arguments = vec![0xEEEE_EEEEu32; (end / 4) as usize];
    for (i, draw) in draws.iter().enumerate() {
        let at = (ARGUMENTS_BASE + ARGUMENTS_STRIDE * i as u64) as usize / 4;
        arguments[at..at + draw.arguments.len()].copy_from_slice(&draw.arguments);
        let place = run_of
            .push_indirect(IndirectGatherDraw {
                arguments_offset: (at * 4) as u64,
                index_offset: BASE + draw.offset,
                index_end: indices.map_or(0, |(_, end)| end),
                wide: draw.wide,
            })
            .expect("the run takes the draw");
        assert_eq!(place, i as u32);
    }
    let range = run_of.arguments_range().expect("arguments");
    let arguments =
        bytes(&arguments)[range.offset as usize..(range.offset + range.range) as usize].to_vec();
    let indices = match (buffer, run_of.indices_range()) {
        (Some(buffer), Some(range)) => {
            let mut padded = vec![0xAB; BASE as usize];
            padded.extend_from_slice(buffer);
            padded.resize(padded.len().next_multiple_of(16).max(16), 0xCC);
            padded[range.offset as usize..(range.offset + range.range) as usize].to_vec()
        }
        _ => vec![0xCC; 4],
    };
    let header = (IndexBoundsPass::POOL_HEADER_BYTES / 4) as usize;
    let mut pool = vec![0xDDDD_DDDD; header + (pool_bytes / 4) as usize];
    pool[0] = counter;
    let draw_words = (IndexGatherPlanModule::DRAW_BYTES / 4) as usize;
    let commands = vec![0xFFFF_FFFFu32; draw_words * draws.len()];
    let plan = plan_typed_module().expect("plan");
    let gather = gather_typed_module().expect("gather");
    let buffer_of = |slot, bytes| Buffer {
        slot: Some(slot),
        address: None,
        bytes,
    };
    let mut ran = Vec::new();
    for order in [Order::Forward, Order::Reverse] {
        let res = Resources {
            buffers: vec![
                buffer_of(WORDS, run_of.table_bytes()),
                buffer_of(plan::ARGUMENTS, arguments.clone()),
                buffer_of(plan::POOL, bytes(&pool)),
                buffer_of(STATUS, ExecutionStatus::initial().to_vec()),
                buffer_of(plan::DRAWS, bytes(&commands)),
            ],
            push: Vec::new(),
            spec: Default::default(),
        };
        let cfg = Config {
            order,
            workgroups: [1, 1, 1],
            ..Config::default()
        };
        let after = run(&plan, &cfg, res).expect("plan run").buffers;
        let table = words(&after[0]);
        let at = (IndexGather::DISPATCH_OFFSET / 4) as usize;
        let dispatch: [u32; 3] = table[at..at + 3].try_into().expect("dispatch");
        assert_eq!(dispatch[1..], [1, 1]);
        assert_eq!(dispatch[0], table[2].min(IndexGatherModule::MAX_GROUPS));
        let res = Resources {
            buffers: vec![
                buffer_of(TABLE, after[0].clone()),
                buffer_of(PATTERN, bytes(&run_of.pattern())),
                buffer_of(INDICES, indices.clone()),
                buffer_of(OUTPUT, after[2].clone()),
            ],
            push: Vec::new(),
            spec: Default::default(),
        };
        let cfg = Config {
            order,
            workgroups: dispatch,
            ..Config::default()
        };
        let gathered = run(&gather, &cfg, res).expect("gather run").buffers;
        ran.push((after, gathered));
    }
    assert_eq!(ran[0], ran[1], "order independent");
    let (after, gathered) = &ran[0];
    let status: [u8; 32] = after[3].as_slice().try_into().expect("status");
    let pool = words(&gathered[3]);
    let commands = words(&after[4]);
    let lists = (0..draws.len())
        .map(|i| {
            let command: [u32; 5] = commands[i * draw_words..(i + 1) * draw_words]
                .try_into()
                .expect("command");
            let first = header + command[2] as usize;
            let list = pool[first..first + command[0] as usize].to_vec();
            (command, list)
        })
        .collect();
    Planned {
        draws: lists,
        counter: pool[0],
        overflow: ExecutionStatus::decode(&status).index_pool_overflow,
    }
}

/// Each indexed indirect draw of a gathering program gets the rewrite pass's list from its
/// arguments (first indices inside, reaching past and wholly past the buffer's whole indices, no
/// instance), placed one after another from the pool's counter, and its command its list and
/// Metal's other arguments; nothing overflows.
#[test]
fn indexed_indirect_draws_get_the_rewrite_pass_s_lists() {
    let buffer: Vec<u8> = (0..1200u32).map(|b| (b * 37 + 11) as u8).collect();
    for program in RewriteProgram::ALL.into_iter().filter(|p| p.gathers()) {
        for order in [RewriteOrder::First, RewriteOrder::LastFirst] {
            for wide in [false, true] {
                let size: u64 = if wide { 4 } else { 2 };
                // count, instances, first index, base vertex, base instance; the offset.
                let cases = [
                    ([7, 1, 0, 0, 0], 0),
                    ([140, 2, 3, 5, 1], 52),
                    ([13, 1, 280 / size as u32, u32::MAX, 0], 8),
                    ([24, 1, 0, 0, 0], 1188),
                    ([8, 1, 2, 0, 0], 4000),
                    ([9, 0, 0, 0, 0], 0),
                    ([2, 1, 0, 0, 0], 0),
                ];
                let draws: Vec<Indirect> = cases
                    .iter()
                    .map(|&(arguments, offset)| Indirect {
                        arguments: arguments.to_vec(),
                        offset: offset / size * size,
                        wide,
                    })
                    .collect();
                let counter = 40;
                let p = planned((program, order), Some(&buffer), &draws, (1 << 14, counter));
                assert!(!p.overflow);
                let mut next = counter / 4;
                for (draw, (command, list)) in draws.iter().zip(&p.draws) {
                    let a = &draw.arguments;
                    let values: Vec<u32> = (0..a[0])
                        .map(|q| {
                            let at = draw.offset + u64::from(a[2] + q) * size;
                            let inside = at + size <= buffer.len() as u64;
                            match (inside, wide) {
                                (false, _) => 0,
                                (true, true) => u32::from_le_bytes(
                                    buffer[at as usize..at as usize + 4].try_into().expect("4"),
                                ),
                                (true, false) => u32::from(u16::from_le_bytes(
                                    buffer[at as usize..at as usize + 2].try_into().expect("2"),
                                )),
                            }
                        })
                        .collect();
                    let expected = match a[1] {
                        0 => Vec::new(),
                        _ => program.list(&values, u32::MAX, order),
                    };
                    let what = format!("{program:?} {order:?} wide {wide} {a:?}");
                    assert_eq!(list, &expected, "{what}");
                    let n = expected.len() as u32;
                    assert_eq!(command, &[n, a[1], next, a[3], a[4]], "{what}");
                    next += n;
                }
                assert_eq!(p.counter, next * 4);
            }
        }
    }
}

/// Each indirect draw without indices of every program but a line loop gets its positions' list
/// from its count, drawn from its vertex start as base vertex.
#[test]
fn indirect_draws_without_indices_get_their_positions_lists() {
    for program in RewriteProgram::ALL
        .into_iter()
        .filter(|p| p.gathers_without_indices())
    {
        for order in [
            RewriteOrder::First,
            RewriteOrder::Last,
            RewriteOrder::LastFirst,
        ] {
            // count, instances, vertex start, base instance.
            let cases = [[9, 1, 0, 0], [300, 3, 17, 2], [1, 1, 5, 0], [6, 0, 0, 0]];
            let draws: Vec<Indirect> = cases
                .iter()
                .map(|a| Indirect {
                    arguments: a.to_vec(),
                    offset: 0,
                    wide: false,
                })
                .collect();
            let p = planned((program, order), None, &draws, (1 << 14, 0));
            assert!(!p.overflow);
            let mut next = 0;
            for (a, (command, list)) in cases.iter().zip(&p.draws) {
                let expected = match a[1] {
                    0 => Vec::new(),
                    _ => program.sequential_list(a[0], order),
                };
                let what = format!("{program:?} {order:?} {a:?}");
                assert_eq!(list, &expected, "{what}");
                let n = expected.len() as u32;
                assert_eq!(command, &[n, a[1], next, a[2], a[3]], "{what}");
                next += n;
            }
        }
    }
}

/// A list the pool's room cannot hold is none, and the plan reports the overflow; the lists
/// before and after it that fit are placed.
#[test]
fn a_list_past_the_pool_s_room_is_none_and_reported() {
    let program = (RewriteProgram::TriangleList, RewriteOrder::LastFirst);
    let draws: Vec<Indirect> = [[30, 1, 0, 0], [3000, 1, 0, 0], [6, 1, 0, 0]]
        .iter()
        .map(|a| Indirect {
            arguments: a.to_vec(),
            offset: 0,
            wide: false,
        })
        .collect();
    // 256 bytes of room past the counter's 64: 48 indices.
    let p = planned(program, None, &draws, (320, 64));
    assert!(p.overflow);
    let counts: Vec<u32> = p.draws.iter().map(|(command, _)| command[0]).collect();
    assert_eq!(counts, [30, 0, 6]);
    assert_eq!(p.draws[2].0[2], 16 + 30);
    assert_eq!(p.counter, 64 + 36 * 4);
}
