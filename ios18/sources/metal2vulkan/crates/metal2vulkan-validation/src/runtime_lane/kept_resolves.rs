//! Device tests of the crate-owned color resolve's kept objects (the runtime's `color_resolve.rs`,
//! `KeptResolves`) on the device the cfg selects, under the validation layer, through the resolve
//! lane's fixture and helpers (`resolves.rs`): a pass resolving the same views as the last takes the
//! descriptor sets and framebuffers that pass made, and still resolves its own samples; a pass of a
//! multisampled texture made after the last one dropped makes its own, whatever handles the driver
//! gives the new texture's views.
//!
//! The pass resolves an `RGBA8Unorm` attachment into a texture of its format's sRGB twin, which the
//! crate-owned resolve draws on every device (no subpass resolve writes the twin's encoding), in
//! the resolve texture's encoding (`resolves.rs`'s
//! `a_resolve_into_the_srgb_twin_resolves_in_the_resolve_texture_s_encoding`). Where the device's
//! multisampled image takes no input attachment usage for free, the resolve reads a copy of the
//! samples through views a pass makes for itself, and no pass is kept.

use super::buffer_with;
use super::resolves::{color_pass, lane, quiet, twin_resolved, Lane, FULL, MASKED};
use metal2vulkan::metal::PixelFormat;
use metal2vulkan_runtime::{BlitOption, ClearColor, StoreAction, Texture, WorkCounts};

const RGBA8: PixelFormat = PixelFormat::RGBA8Unorm;

/// Its sRGB twin, the resolve texture's format.
const TWIN: PixelFormat = PixelFormat::RGBA8UnormSRGB;

/// Resolve `msaa` cleared to `clear` (every channel), samples 0 and 1 then drawn white, into
/// `target`; the device's counts after.
fn resolve(lane: &Lane, msaa: &Texture, target: &Texture, clear: f64) -> WorkCounts {
    let pipeline = lane.pipeline((FULL, MASKED), &[RGBA8], (None, None), 4);
    let mask = buffer_with(&lane.device, &3u32.to_le_bytes()).unwrap();
    let mut pass = color_pass(msaa, Some(target), StoreAction::MultisampleResolve, (0, 0));
    pass.color_attachments[0].clear_color = ClearColor::new(clear, clear, clear, clear);
    lane.run(&pass, |l, e, h| {
        e.set_fragment_buffer(Some(&mask), 0, 0).unwrap();
        l.draw(e, h, &pipeline, 0.5);
    })
    .unwrap();
    lane.device.work_counts()
}

/// Hold every texel of `target` to Metal's resolve into the twin of samples 0 and 1 white over a
/// clear to `clear` (0 or 1): each color channel's codes decoded, averaged in linear space and
/// encoded again, alpha's averaged as the linear channel it is (`bcbcbc80` over a clear to zero),
/// and white where every sample is. The decode and encode are the device's own, so each color
/// channel is held within the codes `device-rounded-resolve` states (`resolves.rs`'s
/// `twin_resolved`).
fn resolved(lane: &Lane, target: &Texture, clear: f64, what: &str) {
    let texel: [u8; 4] = match clear == 0.0 {
        true => [0xbc, 0xbc, 0xbc, 0x80],
        false => [0xff; 4],
    };
    twin_resolved(
        &lane.read(target, (0, 0), 4, BlitOption::None),
        texel,
        true,
        what,
    );
}

#[test]
fn a_pass_of_the_same_views_takes_the_kept_resolve_objects_and_a_new_texture_s_makes_its_own() {
    let (messages, _cache, lane) = lane();
    let target = lane.single(TWIN);
    let msaa = lane.msaa(RGBA8);
    let before = lane.device.work_counts();
    let first = resolve(&lane, &msaa, &target, 0.0);
    resolved(&lane, &target, 0.0, "the first pass");
    assert_eq!(
        first.color_resolve_passes_made - before.color_resolve_passes_made,
        1,
        "the twin's resolve is the crate-owned one on every device"
    );
    let second = resolve(&lane, &msaa, &target, 1.0);
    resolved(&lane, &target, 1.0, "the second pass");
    let kept = second.color_resolve_passes_kept - first.color_resolve_passes_kept;
    let made = second.color_resolve_passes_made - first.color_resolve_passes_made;
    assert_eq!(kept + made, 1);
    if kept == 0 {
        // The device's 4x image takes no input attachment usage without a cost to its memory, so
        // the resolve reads a copy of its samples, through views a pass makes for itself: no pass
        // is kept.
        drop(msaa);
        quiet(lane, &messages);
        return;
    }
    drop(msaa);
    let msaa = lane.msaa(RGBA8);
    let third = resolve(&lane, &msaa, &target, 0.0);
    resolved(&lane, &target, 0.0, "the new texture's pass");
    assert_eq!(
        (
            third.color_resolve_passes_made - second.color_resolve_passes_made,
            third.color_resolve_passes_kept - second.color_resolve_passes_kept,
        ),
        (1, 0),
        "the new texture's pass took the dropped texture's objects"
    );
    drop(msaa);
    quiet(lane, &messages);
}
