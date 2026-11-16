//! Blits between a buffer and a texture at the buffer's edge, and host waits of the longest
//! timeouts, with what a guest can pass: any `u64` offset, `bytesPerRow` and `bytesPerImage`, and
//! Metal's `UINT64_MAX` milliseconds (the reims-vgpu consumer's report: a 4 KiB `Shared` buffer, a
//! 16 x 16 `BGRA8Unorm` texture, 4 x 4 texels from the origin).
//!
//! A copy whose last byte is the buffer's last byte copies the right bytes, both ways, on Metal's
//! rules (one Vulkan region) and off them (staged, the rows where plain Metal places them), the
//! buffer's other bytes left as they are; a layout one row's byte further, or whose rows pass
//! `u64::MAX`, is refused at the call (`Invalid`, `CopyProblem::BufferRange`), the encoder and its
//! command buffer running the rest. So is a multisampled texture's copy. A wait of
//! `Duration::MAX` waits until the value or the completion comes.
//!
//! Device tests: compiled only under the validation crate's GPU safety cfgs (`m2v_gpu`,
//! `m2v_cpu_icd`), on the kind of device the cfg selects.

use super::blit_holds::{device, Scratch, WAIT};
use crate::{
    BlitOption, Buffer, CommandBufferStatus, CommandQueue, CopyProblem, Device, Error, Invalid,
    Origin, Problem, Region, ResourceOptions, RuntimeGap, Size, Texture, TextureDescriptor,
};
use metal2vulkan::metal::{PixelFormat, TextureType, TextureUsage};
use std::time::{Duration, Instant};

/// The buffers' length.
const LENGTH: u64 = 4096;

/// The copies' `bytesPerRow`, and the offset whose 4 x 4 region of 4-byte texels ends at the
/// buffer's last byte (3 rows of 64 bytes, then one of 16).
const PITCH: u64 = 64;
const LAST: u64 = LENGTH - (3 * PITCH + 16);

/// A `bytesPerRow` whose 4 rows span `0x30` bytes once wrapped past `u64::MAX`.
const WRAPS: u64 = 0x5555_5555_5555_5560;

/// `length` bytes that name their place and `seed`.
fn pattern(seed: u8, length: u64) -> Vec<u8> {
    (0..length)
        .map(|i| (i as u8) ^ (i >> 8) as u8 ^ seed)
        .collect()
}

/// A buffer's bytes, read back with no command buffer using it.
fn bytes(buffer: &Buffer) -> Vec<u8> {
    let contents = buffer.contents().expect("a shared buffer");
    // SAFETY: the buffer holds `length` bytes at `contents`, alive with it; no command buffer
    // uses it now.
    unsafe { std::slice::from_raw_parts(contents.as_ptr(), buffer.length() as usize) }.to_vec()
}

/// Commit `blit`'s command buffer and wait for it to complete without an error.
fn run(command_buffer: &crate::CommandBuffer) {
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    assert_eq!(command_buffer.status(), CommandBufferStatus::Completed);
    assert_eq!(command_buffer.error(), None);
}

/// The refusal of a layout past the buffer.
fn past(call: &'static str) -> Result<(), Error> {
    Err(Error::Invalid(Invalid::new(
        call,
        Problem::Copy(CopyProblem::BufferRange),
    )))
}

/// Layouts past the 4 KiB buffer, as `(offset, bytesPerRow)`: the report's three, the span that
/// wraps into the buffer, and a row's byte past the last byte on Metal's rules and off them.
const PAST: [(u64, u64); 6] = [
    (u64::MAX, PITCH),
    (0, u64::MAX),
    (0, 1 << 63),
    (0, WRAPS),
    (LAST + 4, PITCH),
    (LAST + 5, PITCH),
];

/// The 16 x 16 `BGRA8Unorm` texture, its texels zero.
fn texture(device: &Device) -> Texture {
    let mut descriptor = TextureDescriptor::texture_2d(PixelFormat::BGRA8Unorm, 16, 16, false);
    descriptor.resource_options = ResourceOptions::shared();
    descriptor.usage = TextureUsage::SHADER_READ;
    let texture = device.new_texture(&descriptor).unwrap();
    texture
        .replace_region(Region::new_2d(0, 0, 16, 16), 0, 0, &[0; 16 * 16 * 4], 64, 0)
        .unwrap();
    texture
}

/// The 4 x 4 texels at `(x, 0)`, packed.
fn texels(texture: &Texture, x: u64) -> Vec<u8> {
    let mut read = vec![0u8; 4 * 4 * 4];
    texture
        .get_bytes(&mut read, 16, 0, Region::new_2d(x, 0, 4, 4), 0, 0)
        .unwrap();
    read
}

/// The 4 rows of 16 bytes at `start`, `PITCH` apart, of `bytes`, packed.
fn rows(bytes: &[u8], start: u64) -> Vec<u8> {
    (0..4)
        .flat_map(|y| {
            let at = (start + y * PITCH) as usize;
            bytes[at..at + 16].to_vec()
        })
        .collect()
}

#[test]
fn copies_reach_the_buffer_s_last_byte_and_layouts_past_it_are_refused() {
    let scratch = Scratch::new("copy-bounds");
    let device = device(&scratch.0);
    let queue = device.new_command_queue().unwrap();
    let texture = texture(&device);
    let source_bytes = pattern(1, LENGTH);
    let source = (device.new_buffer_with_bytes(&source_bytes, ResourceOptions::shared())).unwrap();
    // Into the texture: at `LAST` on Metal's rules, and from `LAST + 1` and `LAST + 3`, off the
    // texel, each row from its offset rounded down to the texel (plain Metal's placement), at
    // `x` 0, 4 and 8. The layouts past the buffer are refused in the same encoder.
    let placed = [(LAST, 0), (LAST + 1, 4), (LAST + 3, 8)];
    let command_buffer = queue.command_buffer().unwrap();
    let mut blit = command_buffer.blit_command_encoder().unwrap();
    let into = |blit: &mut crate::BlitCommandEncoder, offset, pitch, x| {
        blit.copy_from_buffer_to_texture(
            &source,
            offset,
            pitch,
            0,
            Size::new(4, 4, 1),
            &texture,
            0,
            0,
            Origin::new(x, 0, 0),
            BlitOption::None,
        )
    };
    for (offset, pitch) in PAST {
        let refused = into(&mut blit, offset, pitch, 12);
        assert_eq!(
            refused,
            past("copy_from_buffer_to_texture"),
            "{offset:#x} {pitch:#x}"
        );
    }
    for (offset, x) in placed {
        into(&mut blit, offset, PITCH, x).unwrap();
    }
    blit.end_encoding().unwrap();
    run(&command_buffer);
    for (offset, x) in placed {
        let start = offset - offset % 4;
        assert_eq!(texels(&texture, x), rows(&source_bytes, start), "{offset}");
    }
    // The refused copies wrote nothing.
    assert_eq!(texels(&texture, 12), vec![0; 64]);

    // Out of the texture into a buffer of other bytes: its rows at the same places, every other
    // byte as it was.
    for (offset, x) in placed {
        let before = pattern(2, LENGTH);
        let destination =
            (device.new_buffer_with_bytes(&before, ResourceOptions::shared())).unwrap();
        let command_buffer = queue.command_buffer().unwrap();
        let mut blit = command_buffer.blit_command_encoder().unwrap();
        let out = |blit: &mut crate::BlitCommandEncoder, offset, pitch| {
            blit.copy_from_texture_to_buffer(
                &texture,
                0,
                0,
                Origin::new(x, 0, 0),
                Size::new(4, 4, 1),
                &destination,
                offset,
                pitch,
                0,
                BlitOption::None,
            )
        };
        for (offset, pitch) in PAST {
            assert_eq!(
                out(&mut blit, offset, pitch),
                past("copy_from_texture_to_buffer"),
                "{offset:#x} {pitch:#x}"
            );
        }
        out(&mut blit, offset, PITCH).unwrap();
        blit.end_encoding().unwrap();
        run(&command_buffer);
        let start = offset - offset % 4;
        let mut want = before.clone();
        for y in 0..4 {
            let at = (start + y * PITCH) as usize;
            let from = (start + y * PITCH) as usize;
            want[at..at + 16].copy_from_slice(&source_bytes[from..from + 16]);
        }
        assert_eq!(bytes(&destination), want, "{offset}");
    }
    drop(texture);
    drop(device);
}

/// A copy between a buffer and a 4-sample `R32Uint` texture whose last row's samples end at the
/// buffer's last byte round-trips; a layout a byte further, or whose rows pass `u64::MAX`, is
/// refused (`BufferRange`) where the device runs the emulation; where it does not, the copy is
/// its typed gap (`NotImplemented(MultisampledBufferCopy)`), and the case says so.
#[test]
fn multisampled_copies_reach_the_buffer_s_last_byte_and_no_further() {
    let scratch = Scratch::new("copy-bounds-ms");
    let device = device(&scratch.0);
    let queue = device.new_command_queue().unwrap();
    let mut descriptor = TextureDescriptor::texture_2d(PixelFormat::R32Uint, 4, 4, false);
    descriptor.texture_type = TextureType::Type2DMultisample;
    descriptor.sample_count = 4;
    descriptor.usage = TextureUsage::RENDER_TARGET | TextureUsage::SHADER_READ;
    descriptor.resource_options = ResourceOptions::private();
    let Ok(texture) = device.new_texture(&descriptor) else {
        eprintln!("COPY-BOUNDS-MS: skipped: no 4-sample R32Uint texture here");
        return;
    };
    // 4 rows of 4 texels at 4 samples of 4 bytes: 64 bytes a row.
    let row = 64;
    let last = LENGTH - (3 * row + row);
    let source_bytes = pattern(3, LENGTH);
    let source = (device.new_buffer_with_bytes(&source_bytes, ResourceOptions::shared())).unwrap();
    let destination = device
        .new_buffer_with_bytes(&pattern(4, LENGTH), ResourceOptions::shared())
        .unwrap();
    let size = Size::new(4, 4, 1);
    let command_buffer = queue.command_buffer().unwrap();
    let mut blit = command_buffer.blit_command_encoder().unwrap();
    let into = |blit: &mut crate::BlitCommandEncoder, offset, pitch| {
        blit.copy_from_buffer_to_texture(
            &source,
            offset,
            pitch,
            0,
            size,
            &texture,
            0,
            0,
            Origin::new(0, 0, 0),
            BlitOption::None,
        )
    };
    match into(&mut blit, last, row) {
        Ok(()) => {}
        Err(Error::NotImplemented(RuntimeGap::MultisampledBufferCopy)) => {
            eprintln!("COPY-BOUNDS-MS: skipped: the device runs no multisampled buffer copy");
            return;
        }
        Err(other) => panic!("{other}"),
    }
    for (offset, pitch) in [
        (last + 4, row),
        (u64::MAX - 3, row),
        (0, 1 << 63),
        (0, WRAPS),
    ] {
        assert_eq!(
            into(&mut blit, offset, pitch),
            past("copy_from_buffer_to_texture"),
            "{offset:#x} {pitch:#x}"
        );
        let out = blit.copy_from_texture_to_buffer(
            &texture,
            0,
            0,
            Origin::new(0, 0, 0),
            size,
            &destination,
            offset,
            pitch,
            0,
            BlitOption::None,
        );
        assert_eq!(
            out,
            past("copy_from_texture_to_buffer"),
            "{offset:#x} {pitch:#x}"
        );
    }
    blit.copy_from_texture_to_buffer(
        &texture,
        0,
        0,
        Origin::new(0, 0, 0),
        size,
        &destination,
        last,
        row,
        0,
        BlitOption::None,
    )
    .unwrap();
    blit.end_encoding().unwrap();
    run(&command_buffer);
    let mut want = pattern(4, LENGTH);
    want[last as usize..].copy_from_slice(&source_bytes[last as usize..]);
    assert_eq!(bytes(&destination), want);
    eprintln!("COPY-BOUNDS-MS: ran");
}

/// Metal's longest wait, `UINT64_MAX` milliseconds, and `Duration::MAX`, past what the host's
/// clock holds: a shared event's wait answers at once for a value already reached and waits until
/// a later one comes, and a command buffer's wait until its completion.
#[test]
fn the_longest_waits_wait_until_the_value_or_completion_comes() {
    let scratch = Scratch::new("copy-bounds-wait");
    let device = device(&scratch.0);
    let event = device.new_shared_event().unwrap();
    event.set_signaled_value(5);
    let longest = [Duration::MAX, Duration::from_millis(u64::MAX)];
    for timeout in longest {
        assert!(event.wait_until_signaled_value(5, timeout));
    }
    for (value, timeout) in [6, 7].into_iter().zip(longest) {
        let later = event.clone();
        let start = Instant::now();
        let setter = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(50));
            later.set_signaled_value(value);
        });
        assert!(event.wait_until_signaled_value(value, timeout));
        assert!(start.elapsed() >= Duration::from_millis(50), "{timeout:?}");
        setter.join().unwrap();
    }
    let queue: CommandQueue = device.new_command_queue().unwrap();
    let command_buffer = queue.command_buffer().unwrap();
    command_buffer.commit().unwrap();
    command_buffer
        .wait_until_completed_for(Duration::MAX)
        .unwrap();
    assert_eq!(command_buffer.status(), CommandBufferStatus::Completed);
}
