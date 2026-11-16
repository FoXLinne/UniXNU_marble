//! Protection options (`CommandBuffer::set_protection_options`), on the device the cfg selects, as
//! Metal's GPU answers them (measured by a consumer on an M3 Max, macOS 26.7): an encoder made
//! under options other than 0 runs nothing, nor does any after it, and the command buffer fails
//! with code 16 below 0x10 and 1 from 0x10 up, no event signal firing; options from 0x10 up fail
//! it so with no encoder made under them, every encoder running; under 1, 3 and 7 a compute or
//! render encoder's creation, and a blit encoder's first command, end Metal's process (refused,
//! `Invalid`).

use super::host_memory::on_device;
use super::{buffer_with, read_buffer, WAIT};
use metal2vulkan_runtime::{
    AppleOnly, Buffer, CommandBuffer, CommandBufferError, CommandBufferStatus, CommandQueue, Error,
    EventRef, Problem, SharedEvent,
};

/// A blit of `command_buffer` that fills `buffer`'s 16 bytes with `value`.
fn fill(command_buffer: &CommandBuffer, buffer: &Buffer, value: u8) {
    let mut blit = command_buffer.blit_command_encoder().unwrap();
    blit.fill_buffer(buffer, 0..16, value).unwrap();
    blit.end_encoding().unwrap();
}

/// A command buffer of `queue` that fills `before` with 0x11, sets `options`, fills `after`
/// with 0x22 where it is given, and signals `event` to 1 last; committed and completed.
fn protected(
    queue: &CommandQueue,
    options: u64,
    (before, after): (&Buffer, Option<&Buffer>),
    event: &SharedEvent,
) -> CommandBuffer {
    let command_buffer = queue.command_buffer().unwrap();
    fill(&command_buffer, before, 0x11);
    command_buffer.set_protection_options(options).unwrap();
    if let Some(after) = after {
        fill(&command_buffer, after, 0x22);
    }
    command_buffer
        .encode_signal_event(EventRef::from(event), 1)
        .unwrap();
    command_buffer.commit().unwrap();
    command_buffer.wait_until_completed_for(WAIT).unwrap();
    command_buffer
}

/// An encoder made under options below 0x10 (but 1, 3, 7) runs nothing, an encoder made before
/// them runs, and the command buffer fails with code 16, its signal dropped; from 0x10 up, code
/// 1, and so with no encoder made under them, every encoder running.
#[test]
fn encoders_made_under_protection_options_fail_the_command_buffer_as_metal_s() {
    on_device(|device| {
        let queue = device.new_command_queue().unwrap();
        for (options, code, made_after) in [
            (2, 16, true),
            (0xf, 16, true),
            (0x10, 1, true),
            (u64::MAX, 1, true),
            (0x10, 1, false),
        ] {
            let name = format!("{options:#x}, an encoder after: {made_after}");
            let (before, after) = (
                buffer_with(device, &[0; 16]).unwrap(),
                buffer_with(device, &[0; 16]).unwrap(),
            );
            let event = device.new_shared_event().unwrap();
            let buffers = (&before, made_after.then_some(&after));
            let command_buffer = protected(&queue, options, buffers, &event);
            assert_eq!(
                command_buffer.status(),
                CommandBufferStatus::Error,
                "{name}"
            );
            let error = command_buffer.error().unwrap();
            assert_eq!(
                error,
                CommandBufferError::ProtectedEncoder(options),
                "{name}"
            );
            assert_eq!(error.metal_code(), code, "{name}");
            assert_eq!(read_buffer(&before, 0, 16).unwrap(), [0x11; 16], "{name}");
            assert_eq!(read_buffer(&after, 0, 16).unwrap(), [0; 16], "{name}");
            assert_eq!(event.signaled_value(), 0, "{name}: its signal");
        }
        // Options 0 again, before any encoder under others: a plain command buffer.
        let (before, after) = (
            buffer_with(device, &[0; 16]).unwrap(),
            buffer_with(device, &[0; 16]).unwrap(),
        );
        let event = device.new_shared_event().unwrap();
        let command_buffer = queue.command_buffer().unwrap();
        command_buffer.set_protection_options(2).unwrap();
        command_buffer.set_protection_options(0).unwrap();
        fill(&command_buffer, &before, 0x11);
        fill(&command_buffer, &after, 0x22);
        command_buffer
            .encode_signal_event(EventRef::from(&event), 1)
            .unwrap();
        command_buffer.commit().unwrap();
        command_buffer.wait_until_completed_for(WAIT).unwrap();
        assert_eq!(command_buffer.error(), None);
        assert_eq!(read_buffer(&after, 0, 16).unwrap(), [0x22; 16]);
        assert_eq!(event.signaled_value(), 1);
        eprintln!("PROTECTION: code 16 below 0x10, 1 from 0x10 up, signals dropped");
    });
}

/// Under 1, 3 and 7 a compute or render encoder's creation and a blit encoder's first command are
/// refused (Metal's process ends there); an empty blit encoder ends. The call is refused while an
/// encoder is open, and a change once an encoder was made under options is not measured.
#[test]
fn protection_options_that_end_metal_s_process_are_refused_where_it_ends() {
    on_device(|device| {
        let queue = device.new_command_queue().unwrap();
        let target = buffer_with(device, &[0; 16]).unwrap();
        for options in [1, 3, 7] {
            let command_buffer = queue.command_buffer().unwrap();
            command_buffer.set_protection_options(options).unwrap();
            let refused = |error: Error, call: &str| match error {
                Error::Invalid(invalid) => {
                    assert_eq!(invalid.call, call, "{options}");
                    assert_eq!(invalid.problem, Problem::ProtectedEncoder { options });
                }
                other => panic!("{options}: {other:?}"),
            };
            refused(
                command_buffer.compute_command_encoder().unwrap_err(),
                "compute_command_encoder",
            );
            let mut blit = command_buffer.blit_command_encoder().unwrap();
            refused(
                blit.fill_buffer(&target, 0..16, 1).unwrap_err(),
                "blit_command_encoder",
            );
            blit.end_encoding().unwrap();
        }
        // While an encoder is open: refused as Metal asserts.
        let command_buffer = queue.command_buffer().unwrap();
        let blit = command_buffer.blit_command_encoder().unwrap();
        let Err(Error::Invalid(open)) = command_buffer.set_protection_options(2) else {
            panic!("set while an encoder is open");
        };
        assert_eq!(open.problem, Problem::EncoderOpen);
        blit.end_encoding().unwrap();
        // Once an encoder was made under options: the same options again, not others.
        command_buffer.set_protection_options(2).unwrap();
        fill(&command_buffer, &target, 3);
        command_buffer.set_protection_options(2).unwrap();
        assert_eq!(
            command_buffer.set_protection_options(4),
            Err(Error::AppleOnly(AppleOnly::ProtectedContent))
        );
        command_buffer.commit().unwrap();
        command_buffer.wait_until_completed_for(WAIT).unwrap();
        assert_eq!(
            command_buffer.error(),
            Some(CommandBufferError::ProtectedEncoder(2))
        );
        eprintln!("PROTECTION: 1, 3 and 7 refused at creation and at a blit's first command");
    });
}
