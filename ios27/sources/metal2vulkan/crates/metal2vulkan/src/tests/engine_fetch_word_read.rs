//! A fragment function's color input of a packed 16-bit attachment the device renders only
//! through the view of its word (`metal::format::WordAttachment::Packed16`,
//! `lower::fragment::fetch`).
//!
//! The function reads the attachment's word out of its `R16_UINT` storage image in the ordered
//! pixel interlock and decodes it into the value Metal's programmable blending hands it: each
//! channel's code over its maximum code rounded to `half`, nearest even
//! (`stage_io::word_color::decode`, the same decode a blend of such an attachment reads its
//! destination through). The value is Metal's, measured on an Apple M3 Max under macOS 27.0.1 by
//! the probe `conformance/probes/packed16_word_fetch.m`: a `float4` and a `half4` color input of
//! `BGRA4Unorm`, `ABGR4Unorm` and `B5G6R5Unorm`, stored raw into an `RGBA32Float` attachment,
//! read every code of each 4-bit channel as that `half`, while an `RGBA8Unorm` control read the
//! `f32` quotient through a `float4` input, so the run tells the two rules apart. The sixteen
//! `f32` patterns are `metal::format::word`'s table.
//!
//! On every committed profile that orders fetch through the pixel interlock alone, the fixture's
//! `half4` and `float4` readers (`engine/fbl_fetch_blend8`, whose color 0 is read and written)
//! compile for each such attachment with color 0 in the storage form of `R16_UINT`
//! (`RuntimeFact::InterlockedFramebufferFetch`), and their modules pass L3. MoltenVK renders
//! `BGRA4Unorm` through such a word alone, the RTX 5080 `A1BGR5Unorm`, `ABGR4Unorm` and
//! `BGRA4Unorm`. Before the decode, each was refused `NotImplemented(Format)` at color 0.
//!
//! The other two word kinds' reads are still not built: an 8-bit sRGB word's decode of its bytes
//! and an `RGB9E5Float` word's of its shared exponent are `NotImplemented(Format)` at the
//! attachment. No GPU is used.
//!
//! `llvm-as` assembles the fixture; a missing tool skips visibly.

use crate::device::DeviceProfile;
use crate::error::{Error, Needs};
use crate::library::FunctionArtifact;
use crate::metal::format::WordAttachment;
use crate::metal::{ColorWriteMask, PixelFormat};
use crate::pipeline::descriptor::ColorAttachmentDescriptor;
use crate::pipeline::facts::RuntimeFact;
use crate::pipeline::link::RenderLinkInputs;
use crate::pipeline::lower_inputs::{FramebufferFetchInputs, LoweringInputs};
use crate::pipeline::testing::{
    compile_with, engine_dir, fragment_inputs, link_and_validate, AssembleError,
};
use crate::spirv::testing::committed_profiles;
use crate::vk;

/// The fixture's two readers: a `half4` color input and a `float4` one.
const ENTRIES: [&str; 2] = ["fbl8_over_loaded", "fbl8_over_loaded_float"];

/// Fixture `stem` assembled; `None` (skipped) without `llvm-as`.
fn assembled(stem: &str) -> Option<Vec<u8>> {
    let text =
        std::fs::read_to_string(engine_dir().join(format!("{stem}.ll"))).expect("the fixture");
    match metal2vulkan_devtools::llvm_assemble_text(&text) {
        Ok(b) => Some(b),
        Err(AssembleError::Missing(why)) => {
            eprintln!("SKIPPED: {why}");
            None
        }
        Err(AssembleError::Rejected(why)) => panic!("llvm-as rejected {stem}: {why}"),
    }
}

/// Color 0 of `pf`, written whole and unblended, beside color 1 of mask none (the fixture's
/// second output).
fn attachments(pf: PixelFormat) -> RenderLinkInputs {
    let mut written = ColorAttachmentDescriptor::new(pf);
    written.write_mask = ColorWriteMask::ALL;
    let mut unwritten = ColorAttachmentDescriptor::new(PixelFormat::RGBA16Float);
    unwritten.write_mask = ColorWriteMask::NONE;
    let mut r = RenderLinkInputs::new();
    r.color_attachments[0] = Some(written);
    r.color_attachments[1] = Some(unwritten);
    r
}

/// The lowering inputs of a function that reads color 0 in [`attachments`] on `p`.
fn state(p: &DeviceProfile, pf: PixelFormat) -> LoweringInputs {
    let r = attachments(pf);
    let mut inputs = fragment_inputs(&r, p);
    inputs.framebuffer_fetch = Some(FramebufferFetchInputs::of(
        [0],
        &r.color_attachments,
        false,
        false,
        1,
    ));
    inputs
}

/// Color 0's storage form in `a`: its Vulkan format, whether it writes, and its blend.
fn fetched(a: &FunctionArtifact) -> Option<(vk::Format, bool, bool)> {
    a.facts.iter().find_map(|f| match f {
        RuntimeFact::InterlockedFramebufferFetch {
            color: 0,
            format,
            writes,
            blend,
            ..
        } => Some((*format, *writes, blend.is_some())),
        _ => None,
    })
}

/// Whether `e` refuses a capability the compile layer does not implement.
fn not_implemented(e: &Error) -> bool {
    matches!(e, Error::Unsupported(u) if u.needs == Needs::NotImplemented)
}

#[test]
fn a_packed_sixteen_word_fetch_decodes_the_word() {
    let Some(bc) = assembled("fbl_fetch_blend8") else {
        return;
    };
    let mut checked: Vec<(String, PixelFormat)> = Vec::new();
    for (stem, p) in committed_profiles().expect("profiles") {
        if !crate::lower::fragment::interlock_orders(&p) {
            continue;
        }
        for &pf in PixelFormat::ALL {
            if !matches!(
                WordAttachment::of(pf, &p),
                Some(WordAttachment::Packed16 { .. })
            ) {
                continue;
            }
            let inputs = state(&p, pf);
            for entry in ENTRIES {
                let what = format!("{stem} {pf:?} {entry}");
                let a =
                    compile_with(&bc, entry, &p, &inputs).unwrap_or_else(|e| panic!("{what}: {e}"));
                assert_eq!(
                    fetched(&a),
                    Some((vk::Format::R16_UINT, true, false)),
                    "{what}: {:?}",
                    a.facts
                );
                link_and_validate(&a, &p).unwrap_or_else(|e| panic!("{what}: L3: {e}"));
            }
            checked.push((stem.clone(), pf));
        }
    }
    // MoltenVK renders `BGRA4Unorm` through a packed 16-bit word; the 5080 three such formats.
    {
        let stem = "moltenvk-m3";
        let formats: Vec<PixelFormat> = (checked.iter())
            .filter(|(s, _)| s == stem)
            .map(|&(_, pf)| pf)
            .collect();
        assert_eq!(formats, [PixelFormat::BGRA4Unorm], "{stem}: {checked:?}");
    }
    let nvidia: Vec<PixelFormat> = (checked.iter())
        .filter(|(s, _)| s == "nvidia-rtx5080-laptop")
        .map(|&(_, pf)| pf)
        .collect();
    assert_eq!(
        nvidia,
        [
            PixelFormat::A1BGR5Unorm,
            PixelFormat::ABGR4Unorm,
            PixelFormat::BGRA4Unorm
        ],
        "{checked:?}"
    );
}

/// An 8-bit sRGB word's read and an `RGB9E5Float` word's are not built: the attachment refuses
/// the pipeline `NotImplemented(Format)` where the function reads it (the module docs).
#[test]
fn srgb_and_shared_exponent_word_fetches_are_not_built() {
    let Some(bc) = assembled("fbl_fetch_blend8") else {
        return;
    };
    let mut refused = 0;
    for (stem, p) in committed_profiles().expect("profiles") {
        if !crate::lower::fragment::interlock_orders(&p) {
            continue;
        }
        for &pf in PixelFormat::ALL {
            if !matches!(
                WordAttachment::of(pf, &p),
                Some(WordAttachment::Srgb8 { .. } | WordAttachment::Rgb9e5)
            ) {
                continue;
            }
            let inputs = state(&p, pf);
            for entry in ENTRIES {
                let what = format!("{stem} {pf:?} {entry}");
                match compile_with(&bc, entry, &p, &inputs) {
                    Ok(a) => panic!("{what}: compiled: {:?}", a.facts),
                    Err(e) => {
                        assert!(not_implemented(&e), "{what}: {e}");
                        refused += 1;
                    }
                }
            }
        }
    }
    assert!(
        refused > 0,
        "no profile renders an 8-bit sRGB or shared-exponent format through a word"
    );
}
