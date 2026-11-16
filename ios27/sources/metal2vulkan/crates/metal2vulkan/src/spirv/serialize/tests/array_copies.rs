//! A materialized array is copied element by element. Where the profile takes
//! `DriverWorkaround::LoadsForwardedIntoLoops` (MoltenVK), a `uvec4[2]` push field loaded before a
//! loop that reads it is materialized (an in-place YCbCr texture's words, `PushField::YCbCrInPlace`,
//! are such a field, read by a sample in a loop). SPIRV-Cross's MSL initializes the
//! `OpCopyObject` of a loaded array with the load's expression, a `spvUnsafeArray` from the push
//! block's plain C array, which Metal's compiler refuses (`no viable conversion from 'const
//! constant uint4[2]'`); each element's copy and the construct of the copies compile.

use super::{cval, elem_ptr, finish_compute, gid_x, int, ssbo, vec};
use crate::pipeline::sym::PushField;
use crate::pipeline::testing::{moltenvk_msl_compile, AssembleError};
use crate::spirv::builder::constructs::{ArmEnd, LoopCtx, LoopSpec};
use crate::spirv::builder::{FunctionBuilder, ModuleBuilder};
use crate::spirv::ir::{BinOp, ConstBits, Constant, MemAccess, Type, TypedModule};
use crate::spirv::testing::{assert_valid, committed_profiles, is_moltenvk_capture};
use crate::spirv_binary::Instruction;
use crate::spv::Op;

/// `words = push.words` (a `uvec4[2]` at offset 16 of the push block, after a `u32` count at 0)
/// before a loop of `push.count` iterations, each of which adds `words[0].x + words[1].y` into
/// `out[gid]`.
fn kernel() -> TypedModule {
    let mut b = ModuleBuilder::new();
    let u32_ = int(&mut b, 32, false);
    let uvec4 = vec(&mut b, u32_, 4);
    let two = b
        .constant(Constant::Scalar {
            ty: u32_,
            bits: ConstBits(2),
        })
        .expect("2");
    let words_ty = b
        .ty(Type::Array {
            elem: uvec4,
            len: two,
            stride: Some(16),
        })
        .expect("uvec4[2]");
    let count = b
        .push_member(PushField::ViewportCount, u32_, 0)
        .expect("count member");
    let words = b
        .push_member(PushField::DispatchRegion, words_ty, 16)
        .expect("words member");
    let push = b.push_global().expect("push block");
    let out = ssbo(&mut b, 0, u32_, 4);
    let main = super::compute(&mut b, |f| {
        let i = gid_x(f);
        let at = elem_ptr(f, out, i);
        let base = f.module().global_value(push).expect("push");
        let member = |f: &mut FunctionBuilder<'_>, index: u32| {
            let index = cval(f, u32_, u64::from(index));
            f.access_chain(base, &[index]).expect("member")
        };
        let p = member(f, count.index);
        let n = f.load(p, MemAccess::default()).expect("count");
        let p = member(f, words.index);
        let w = f.load(p, MemAccess::default()).expect("words");
        let counter = f.local_var(u32_, None).expect("counter");
        let counter = f.var_ptr(counter).expect("counter pointer");
        let zero = cval(f, u32_, 0);
        f.store(counter, zero, MemAccess::default())
            .expect("counter = 0");
        let one = cval(f, u32_, 1);
        f.loop_(
            &mut (),
            LoopSpec::default(),
            Box::new(
                move |fb: &mut FunctionBuilder<'_>, ctx: &mut (), lc: &LoopCtx| {
                    let k = fb.load(counter, MemAccess::default())?;
                    let done = fb.binary(BinOp::UGreaterThanEqual, k, n)?;
                    fb.if_(
                        ctx,
                        done,
                        Box::new(|fb: &mut FunctionBuilder<'_>, _: &mut ()| {
                            lc.break_(fb)?;
                            Ok(ArmEnd::Exits)
                        }),
                        None,
                    )?;
                    let k1 = fb.binary(BinOp::IAdd, k, one)?;
                    fb.store(counter, k1, MemAccess::default())?;
                    let low = fb.composite_extract(w, &[0])?;
                    let high = fb.composite_extract(w, &[1])?;
                    let x = fb.composite_extract(low, &[0])?;
                    let y = fb.composite_extract(high, &[1])?;
                    let sum = fb.binary(BinOp::IAdd, x, y)?;
                    let had = fb.load(at, MemAccess::default())?;
                    let sum = fb.binary(BinOp::IAdd, had, sum)?;
                    fb.store(at, sum, MemAccess::default())
                },
            ),
        )
        .expect("loop");
    });
    finish_compute(b, main)
}

fn parse(words: &[u32]) -> Vec<Instruction> {
    let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    crate::spirv_binary::parse_bytes(&bytes)
        .expect("parse")
        .instructions
}

/// Whether some `OpCopyObject` of `words` copies an array: its result type is an `OpTypeArray`.
fn copies_an_array(words: &[u32]) -> bool {
    let insts = parse(words);
    let arrays: Vec<u32> = insts
        .iter()
        .filter(|i| i.opcode == Op::TypeArray)
        .filter_map(|i| i.result_id)
        .collect();
    insts
        .iter()
        .filter(|i| i.opcode == Op::CopyObject)
        .any(|i| i.result_type.is_some_and(|ty| arrays.contains(&ty)))
}

/// On the MoltenVK captures the loaded `uvec4[2]` is materialized as each element's copy and the
/// construct of the copies, with no `OpCopyObject` of an array; the module passes L3 on every
/// profile, and on macOS it compiles as MoltenVK's MSL, which the array's own `OpCopyObject` did
/// not. `spirv-cross` and the Metal compiler are external; without them the compile skips
/// visibly.
#[test]
fn a_loaded_array_a_loop_reads_is_copied_element_by_element_and_compiles_on_moltenvk() {
    let profiles = committed_profiles().expect("committed profiles");
    let mut compiled = 0;
    for (stem, profile) in &profiles {
        let words = assert_valid(kernel(), profile);
        assert!(!copies_an_array(&words), "{stem}: an array's OpCopyObject");
        if !is_moltenvk_capture(stem) {
            continue;
        }
        let insts = parse(&words);
        assert!(
            insts.iter().any(|i| i.opcode == Op::CopyObject),
            "{stem}: the loaded words are materialized"
        );
        match moltenvk_msl_compile(&words) {
            Ok(()) => compiled += 1,
            Err(AssembleError::Missing(why)) => eprintln!("SKIPPED ({stem}): {why}"),
            Err(AssembleError::Rejected(why)) => panic!("{stem}: MoltenVK's MSL: {why}"),
        }
    }
    eprintln!("compiled as MoltenVK's MSL on {compiled} profile(s)");
}
