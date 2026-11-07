//! Compute encoders of raw `MTLDispatchType` values
//! (`CommandBuffer::compute_command_encoder_with_raw_dispatch_type`), on the device the cfg
//! selects, as Metal makes them (the M3 Max under macOS 27.0.1): 1 makes a concurrent encoder,
//! every other value of the 64 bits a serial one, whose `dispatchType` answers serial.

use super::host_memory::on_device;
use super::WAIT;
use metal2vulkan::metal::DispatchType;
use metal2vulkan_runtime::CommandBufferStatus;

/// Each raw value makes the encoder Metal makes of it, and the command buffer completes.
#[test]
fn raw_dispatch_types_make_metal_s_encoders() {
    on_device(|device| {
        let queue = device.new_command_queue().unwrap();
        let command_buffer = queue.command_buffer().unwrap();
        for (raw, made) in [
            (0, DispatchType::Serial),
            (1, DispatchType::Concurrent),
            (2, DispatchType::Serial),
            (7, DispatchType::Serial),
            (0x8000_0000, DispatchType::Serial),
            (u64::from(u32::MAX), DispatchType::Serial),
            (1 << 32, DispatchType::Serial),
            ((1 << 32) | 1, DispatchType::Serial),
            ((1 << 63) | 1, DispatchType::Serial),
            (u64::MAX, DispatchType::Serial),
        ] {
            let encoder = command_buffer
                .compute_command_encoder_with_raw_dispatch_type(raw)
                .unwrap();
            assert_eq!(encoder.dispatch_type(), made, "{raw:#x}");
            encoder.end_encoding().unwrap();
        }
        command_buffer.commit().unwrap();
        command_buffer.wait_until_completed_for(WAIT).unwrap();
        assert_eq!(command_buffer.status(), CommandBufferStatus::Completed);
    });
}
