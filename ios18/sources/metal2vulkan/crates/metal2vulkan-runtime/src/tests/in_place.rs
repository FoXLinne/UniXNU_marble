//! Textures over host pages in place (`in_place.rs`): which texture is tried as an image in place
//! and why one is a shadow, and the record that keeps the device's check's verdicts.
//!
//! - **The decision.** Under `HostPageTexturePolicy::Shadow` every texture is a shadow, the check
//!   never asked; under `InPlace` a texture whose usage renders into it (`RenderTarget`,
//!   `Unknown`) is one counted as a render target, and one that writes it through a shader and
//!   does not render into it (`ShaderWrite`, `ShaderAtomic`) one counted as a shader write, the
//!   check never asked; and a texture that only samples or reads is tried in place exactly where
//!   its class's check passed, else a shadow naming the check's answer (pending, failed,
//!   unchecked).
//! - **The record.** Its verdicts read back for the identity it was written for, and nothing else
//!   reads: a record cut short or grown, of another check version or runtime, of another identity,
//!   with a byte changed (its checksum), or holding a pending or unknown verdict.
//! - **Its name** changes with every field of the driver's identity.

use crate::in_place::{
    decode, encode, in_place, record_name, DriverIdentity, InPlaceChecks, PagesClass, ShadowCause,
    Verdict, CHECK_VERSION,
};
use metal2vulkan::device::HostPageTexturePolicy;
use metal2vulkan::metal::TextureUsage;
use sha2::{Digest, Sha256};
use std::time::Duration;

const VERDICTS: [Verdict; 4] = [
    Verdict::Pending,
    Verdict::Passed,
    Verdict::Failed,
    Verdict::Unchecked,
];

/// Every usage bit Metal has, each alone.
fn each_usage() -> Vec<TextureUsage> {
    vec![
        TextureUsage::SHADER_READ,
        TextureUsage::SHADER_WRITE,
        TextureUsage::RENDER_TARGET,
        TextureUsage::PIXEL_FORMAT_VIEW,
        TextureUsage::SHADER_ATOMIC,
    ]
}

/// The shadow `usage` takes whatever the check answers: a render target's where it renders
/// into the texture, whatever else it holds, a shader write's where it writes it through a shader
/// alone; `None` where it only samples or reads.
fn written(usage: TextureUsage) -> Option<ShadowCause> {
    if usage.contains(TextureUsage::RENDER_TARGET) {
        Some(ShadowCause::RenderTarget)
    } else if usage.intersects(TextureUsage::SHADER_WRITE | TextureUsage::SHADER_ATOMIC) {
        Some(ShadowCause::ShaderWrite)
    } else {
        None
    }
}

#[test]
fn the_decision_takes_only_a_sampled_texture_whose_class_passed() {
    let mut usages = each_usage();
    usages.push(TextureUsage::SHADER_READ | TextureUsage::PIXEL_FORMAT_VIEW);
    usages.push(TextureUsage::SHADER_READ | TextureUsage::RENDER_TARGET);
    usages.push(TextureUsage::SHADER_READ | TextureUsage::SHADER_WRITE);
    usages.push(TextureUsage::RENDER_TARGET | TextureUsage::SHADER_WRITE);
    usages.push(TextureUsage::RENDER_TARGET | TextureUsage::SHADER_ATOMIC);
    usages.push(TextureUsage::SHADER_WRITE | TextureUsage::SHADER_ATOMIC);
    // An `Unknown` usage, as Metal reports it: every usage.
    usages.push(
        TextureUsage::SHADER_READ
            | TextureUsage::SHADER_WRITE
            | TextureUsage::RENDER_TARGET
            | TextureUsage::PIXEL_FORMAT_VIEW
            | TextureUsage::SHADER_ATOMIC,
    );
    usages.push(TextureUsage::empty());
    for class in PagesClass::ALL {
        for &usage in &usages {
            for verdict in VERDICTS {
                // Shadow: always the exact form, the check never asked.
                let asked = std::cell::Cell::new(false);
                let decided = in_place(HostPageTexturePolicy::Shadow, usage, class, |_| {
                    asked.set(true);
                    verdict
                });
                assert_eq!(decided, Err(ShadowCause::Exact));
                assert!(!asked.get(), "the exact form asks no check");
                // In place: written textures never asked; the rest follow the verdict.
                let asked = std::cell::Cell::new(false);
                let decided = in_place(HostPageTexturePolicy::InPlace, usage, class, |of| {
                    assert_eq!(of, class);
                    asked.set(true);
                    verdict
                });
                let expected = if let Some(cause) = written(usage) {
                    Err(cause)
                } else {
                    match verdict {
                        Verdict::Passed => Ok(()),
                        Verdict::Pending => Err(ShadowCause::CheckPending(class)),
                        Verdict::Failed => Err(ShadowCause::CheckFailed(class)),
                        Verdict::Unchecked => Err(ShadowCause::Unchecked(class)),
                    }
                };
                assert_eq!(decided, expected, "{class:?} {usage:?} {verdict:?}");
                assert_eq!(asked.get(), written(usage).is_none(), "{class:?} {usage:?}");
            }
        }
    }
}

#[test]
fn every_shadow_cause_names_itself() {
    let causes = [
        ShadowCause::Exact,
        ShadowCause::RenderTarget,
        ShadowCause::CheckPending(PagesClass::Pointer),
        ShadowCause::CheckFailed(PagesClass::DmaBuf),
        ShadowCause::Unchecked(PagesClass::Fresh),
    ];
    let words: Vec<String> = causes.iter().map(ShadowCause::words).collect();
    for (at, word) in words.iter().enumerate() {
        assert!(!word.is_empty());
        assert!(
            words.iter().skip(at + 1).all(|other| other != word),
            "{word}"
        );
    }
    assert!(words[0].contains("HostPageTexturePolicy::Shadow"));
    assert!(words[2].contains("not answered"));
    assert!(words[3].contains("dma-bufs"));
    // A shader write's shadow announces what a render target's does: the notice is unchanged.
    assert_eq!(ShadowCause::ShaderWrite.words(), words[1]);
}

/// A driver identity, every field set.
fn identity() -> DriverIdentity {
    DriverIdentity {
        vendor_id: 0x1002,
        device_id: 0x15bf,
        driver_version: 0x0068_2004,
        driver_id: 3,
        driver_name: "radv".to_owned(),
        driver_info: "Mesa 26.2.4".to_owned(),
        device_uuid: [7; 16],
    }
}

/// `bytes` with their checksum (the last 32 bytes) made again over the rest.
fn rechecked(mut bytes: Vec<u8>) -> Vec<u8> {
    let body = bytes.len() - 32;
    let checksum: [u8; 32] = Sha256::digest(&bytes[..body]).into();
    bytes[body..].copy_from_slice(&checksum);
    bytes
}

#[test]
fn a_record_reads_back_for_its_identity_alone() {
    let identity = identity();
    let verdicts = [Verdict::Passed, Verdict::Unchecked, Verdict::Failed];
    let bytes = encode(&identity, verdicts);
    assert_eq!(decode(&identity, &bytes), Some(verdicts));

    // Cut short at every length, or grown.
    for length in 0..bytes.len() {
        assert_eq!(decode(&identity, &bytes[..length]), None, "{length} bytes");
    }
    let mut grown = bytes.clone();
    grown.push(0);
    assert_eq!(decode(&identity, &grown), None);

    // Any byte changed: the checksum fails.
    for at in 0..bytes.len() {
        let mut changed = bytes.clone();
        changed[at] ^= 0x01;
        assert_eq!(decode(&identity, &changed), None, "byte {at}");
    }

    // Another check version, or another runtime, with a checksum that holds.
    let mut version = bytes.clone();
    version[8..12].copy_from_slice(&(CHECK_VERSION + 1).to_le_bytes());
    assert_eq!(decode(&identity, &rechecked(version)), None);
    let mut runtime = bytes.clone();
    let other = metal2vulkan::cache::RUNTIME_VERSION.wrapping_add(1);
    runtime[12..16].copy_from_slice(&other.to_le_bytes());
    assert_eq!(decode(&identity, &rechecked(runtime)), None);

    // A pending or unknown verdict, with a checksum that holds.
    for byte in [0u8, 4, 0xff] {
        let mut verdict = bytes.clone();
        verdict[48] = byte;
        assert_eq!(
            decode(&identity, &rechecked(verdict)),
            None,
            "verdict {byte}"
        );
    }

    // Another identity.
    let mut other = identity.clone();
    other.driver_version += 1;
    assert_eq!(decode(&other, &bytes), None);
}

#[test]
fn a_record_s_name_follows_every_field_of_the_identity() {
    let base = identity();
    let name = record_name(&base);
    assert_eq!(name.len(), 64);
    let changes: [fn(&mut DriverIdentity); 7] = [
        |id| id.vendor_id += 1,
        |id| id.device_id += 1,
        |id| id.driver_version += 1,
        |id| id.driver_id += 1,
        |id| id.driver_name.push('x'),
        |id| id.driver_info.push('x'),
        |id| id.device_uuid[15] ^= 1,
    ];
    let mut names = vec![name];
    for change in changes {
        let mut changed = base.clone();
        change(&mut changed);
        let name = record_name(&changed);
        assert!(!names.contains(&name), "{changed:?}");
        names.push(name);
    }
    // Text fields are framed by their lengths: moving a byte between them changes the name.
    let mut moved = base.clone();
    moved.driver_name = "radvM".to_owned();
    moved.driver_info = "esa 26.2.4".to_owned();
    assert_ne!(record_name(&moved), record_name(&base));
}

#[test]
fn the_checks_state_settles_once_and_wakes_its_waiters() {
    let checks = InPlaceChecks::new(false);
    assert!(!checks.forced_failure());
    assert_eq!(checks.verdicts(), [Verdict::Pending; 3]);
    assert!(!checks.wait(Duration::from_millis(1)));
    assert!(checks.start());
    assert!(!checks.start(), "the check starts once");
    checks.set(PagesClass::Fresh, Verdict::Passed);
    assert_eq!(checks.verdict(PagesClass::Fresh), Verdict::Passed);
    assert_eq!(checks.verdict(PagesClass::Pointer), Verdict::Pending);
    std::thread::scope(|scope| {
        let waiter = scope.spawn(|| checks.wait(Duration::from_secs(30)));
        checks.settle(Some(Duration::from_millis(5)));
        assert!(waiter.join().unwrap_or(false));
    });
    assert_eq!(checks.took(), Some(Duration::from_millis(5)));
    assert!(InPlaceChecks::new(true).forced_failure());
}
