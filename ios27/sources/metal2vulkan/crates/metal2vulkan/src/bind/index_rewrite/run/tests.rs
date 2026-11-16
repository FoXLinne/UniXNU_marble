//! A run of the index rewrite pass: its module valid on every committed profile (L3, `G-val`);
//! on the CPU interpreter, one dispatch of a run gives each draw the list Metal draws
//! ([`RewriteProgram::list`] of its fetched positions in the run's order), in its own region of
//! the run's output, and its command Metal's instances and bases, for every program, either
//! index size, restarts, offsets and counts reaching past the buffer's whole indices, in either
//! workgroup order; and a run refuses what it cannot take.

use super::super::{rewrite_run_typed_module, ARGUMENTS, INDICES, OUTPUT, STATUS, WORDS};
use super::*;
use crate::bind::status::ExecutionStatus;
use crate::spirv::interp::{run, Buffer, Config, Order, Resources};
use crate::spirv::testing::{assert_valid, committed_profiles};

/// The storage buffer offset alignment of these tests.
const ALIGNMENT: u64 = 16;
/// Bytes of 0xAB before the indices.
const PREFIX: u64 = 4;

fn words(bytes: &[u8]) -> Vec<u32> {
    bytes
        .as_chunks::<4>()
        .0
        .iter()
        .map(|c| u32::from_le_bytes(*c))
        .collect()
}

/// The index buffer of `indices` after [`PREFIX`] bytes of 0xAB, 4 bytes each where `wide`,
/// else 2, padded to 16 bytes with bytes no position may read.
fn buffer(indices: &[u32], wide: bool) -> (Vec<u8>, u64) {
    let mut b = vec![0xAB; PREFIX as usize];
    for &i in indices {
        match wide {
            true => b.extend(i.to_le_bytes()),
            false => b.extend((i as u16).to_le_bytes()),
        }
    }
    let end = b.len() as u64;
    b.resize(b.len().next_multiple_of(16), 0xCC);
    (b, end)
}

/// The draws of `program` in `order` restarting at `restart` over `indices`, each from its first
/// index for its count, run as one run: each draw's list (its command's indices of the run's
/// output) and command.
fn ran(
    (program, order, restart): (RewriteProgram, RewriteOrder, u32),
    (indices, wide): (&[u32], bool),
    draws: &[(u32, u32)],
) -> Vec<(Vec<u32>, [u32; 5])> {
    let size = if wide { 4 } else { 2 };
    let (bytes, end) = buffer(indices, wide);
    let mut run_ = IndexRewriteRun::new(
        (program, order, restart),
        (0, end),
        (ALIGNMENT, u32::MAX),
        1 << 16,
    )
    .expect("run");
    for (i, &(first, count)) in draws.iter().enumerate() {
        let i = i as u32;
        let slot = run_
            .push(RewriteRunDraw {
                count,
                instances: 1 + i,
                base_vertex: -(i as i32),
                base_instance: 7 * i,
                index_offset: PREFIX + u64::from(first) * size,
                index_end: end,
                wide,
            })
            .expect("draw");
        assert_eq!(slot, i);
    }
    let range = run_.indices_range().expect("indices");
    let bound = bytes[range.offset as usize..(range.offset + range.range) as usize].to_vec();
    let module = rewrite_run_typed_module(program).expect("module");
    let mut after = Vec::new();
    for order in [Order::Forward, Order::Reverse] {
        let buffers = [
            run_.words(),
            vec![0xCC; 4],
            bound.clone(),
            vec![0xDD; run_.written().max(1) as usize * 4],
            ExecutionStatus::initial().to_vec(),
        ];
        let res = Resources {
            buffers: [WORDS, ARGUMENTS, INDICES, OUTPUT, STATUS]
                .into_iter()
                .zip(buffers)
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
            workgroups: [run_.workgroups(), 1, 1],
            ..Config::default()
        };
        after.push(run(&module, &cfg, res).expect("run").buffers);
    }
    assert_eq!(after[0], after[1], "order independent");
    let words_after = words(&after[0][0]);
    let output = words(&after[0][3]);
    (0..draws.len() as u32)
        .map(|slot| {
            let at = (IndexRewriteRun::draw_offset(slot) / 4) as usize;
            let command: [u32; 5] = words_after[at..at + 5].try_into().expect("command");
            let (first, n) = (command[2] as usize, command[0] as usize);
            assert!(first + n <= output.len(), "the draw reads past the output");
            (output[first..first + n].to_vec(), command)
        })
        .collect()
}

/// Metal's list of the draw of `program` from `first` for `count` over `indices` (positions past
/// them read 0), restarting at `restart`, in `order`.
fn metal(
    (program, order, restart): (RewriteProgram, RewriteOrder, u32),
    indices: &[u32],
    (first, count): (u32, u32),
) -> Vec<u32> {
    let values: Vec<u32> = (first..first.saturating_add(count))
        .map(|p| indices.get(p as usize).copied().unwrap_or(0))
        .collect();
    program.list(&values, restart, order)
}

#[test]
fn the_module_is_valid_on_every_committed_profile() {
    for (_, p) in committed_profiles().expect("profiles") {
        for program in RewriteProgram::ALL {
            assert_valid(rewrite_run_typed_module(program).expect("module"), &p);
        }
    }
}

/// Each draw of a run gets Metal's list in a region of its own, after the regions before it, and
/// Metal's instances and bases.
#[test]
fn each_draw_of_a_run_gets_metal_s_list() {
    let orders = [
        RewriteOrder::First,
        RewriteOrder::Last,
        RewriteOrder::LastFirst,
    ];
    for program in RewriteProgram::ALL {
        for wide in [false, true] {
            let restart = if wide { 0xFFFF_FFFF } else { 0xFFFF };
            // Pieces between restarts, a repeated index, and an all-ones index of the other size.
            let indices: Vec<u32> = [
                0, 1, 2, 3, restart, 4, 5, 6, 7, 8, restart, restart, 9, 10, 0xFFFF, 11, 12, 13,
                14, restart, 15, 16, 17, 18, 19, 20, 21,
            ]
            .into_iter()
            .map(|i| if wide { i } else { i & 0xFFFF })
            .collect();
            let n = indices.len() as u32;
            // Every draw from 0, one inside, one past the buffer's whole indices, an empty one,
            // and one of several positions per invocation.
            let draws = [(0, n), (3, 9), (1, 2), (20, 12), (5, 0), (n, 3), (2, 700)];
            for order in orders {
                let terms = (program, order, restart);
                let lists = ran(terms, (&indices, wide), &draws);
                let mut next = 0;
                for (i, (&draw, (list, command))) in draws.iter().zip(&lists).enumerate() {
                    let what = format!("{program:?} wide {wide} {order:?} draw {draw:?}");
                    assert_eq!(*list, metal(terms, &indices, draw), "{what}");
                    let i = i as u32;
                    assert_eq!(
                        command[1..],
                        [1 + i, next, (-(i as i32)) as u32, 7 * i],
                        "{what}"
                    );
                    next += program.capacity(u64::from(draw.1)) as u32;
                }
            }
        }
    }
}

/// A run takes no draw whose walk takes more than one group, whose indices lie outside its bound
/// bytes or at an offset not a multiple of their size, or whose list is past its output.
#[test]
fn a_run_refuses_what_it_cannot_take() {
    let terms = (
        RewriteProgram::TriangleStrip,
        RewriteOrder::LastFirst,
        0xFFFF,
    );
    let draw = |count: u32, index_offset: u64| RewriteRunDraw {
        count,
        instances: 1,
        base_vertex: 0,
        base_instance: 0,
        index_offset,
        index_end: 1 << 20,
        wide: false,
    };
    let mut run_ =
        IndexRewriteRun::new(terms, (0, 1 << 20), (ALIGNMENT, u32::MAX), 1 << 16).expect("run");
    let one_group = IndexRewritePass::GROUP_POSITIONS as u32 - 4;
    assert_eq!(run_.push(draw(one_group, 0)), Some(0));
    assert_eq!(run_.push(draw(one_group + 5, 0)), None, "two groups");
    assert_eq!(run_.push(draw(3, 1)), None, "an odd offset");
    let mut small = IndexRewriteRun::new(terms, (64, 128), (ALIGNMENT, u32::MAX), 6).expect("run");
    let inside = |count, index_offset| RewriteRunDraw {
        index_end: 128,
        ..draw(count, index_offset)
    };
    assert_eq!(small.push(inside(3, 32)), None, "before the bound bytes");
    assert_eq!(small.push(draw(64, 64)), None, "past the bound bytes");
    assert_eq!(small.push(inside(4, 64)), Some(0));
    assert_eq!(small.push(inside(3, 64)), None, "past the output");
    assert_eq!(small.written(), 6);
}
