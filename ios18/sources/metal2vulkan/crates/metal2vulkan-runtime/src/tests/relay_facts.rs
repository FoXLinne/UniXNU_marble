//! `device/relay.rs`, on the device the cfg selects: its queues' failures are relayed exactly
//! where its driver has the fact that earns the relay
//! (`DriverFact::QueueFailuresReportedByMessenger`) and its instance has the relay's messenger,
//! and a use of a `VkQueue` outside a batch makes the relay's mark of no work exactly then.
//!
//! `tests::batches::only_drivers_that_report_their_failures_by_messenger_relay_them` reads the
//! committed profiles, whose identities are the releases the lanes measured. A host running a
//! release no profile holds is the one identity those cannot speak for, and the relay is the
//! defect whose loss is silent: without it a GPU fault loses the whole device, where Metal fails
//! the command buffer it struck and the queue runs on. So this reads the live device.
//!
//! Device tests: compiled only under the validation crate's GPU safety cfgs (`m2v_gpu`,
//! `m2v_cpu_icd`), on the kind of device the cfg selects.

use crate::{Device, DeviceKind, DeviceOptions, ShaderCacheOptions};
use metal2vulkan::device::DriverFact;
use std::path::{Path, PathBuf};

/// A scratch directory for the device's shader cache, removed when dropped.
struct Scratch(PathBuf);

impl Scratch {
    fn new() -> Scratch {
        let path = std::env::temp_dir().join(format!("m2v-relay-facts-{}", std::process::id()));
        std::fs::create_dir_all(&path).unwrap();
        Scratch(path)
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// The device the cfg selects: a CPU device under `m2v_cpu_icd`, else a GPU, its shader cache
/// in `cache`.
fn device(cache: &Path) -> Device {
    let options = DeviceOptions {
        kinds: match cfg!(m2v_cpu_icd) {
            true => vec![DeviceKind::Cpu],
            false => vec![
                DeviceKind::Discrete,
                DeviceKind::Integrated,
                DeviceKind::Virtual,
            ],
        },
        shader_cache: ShaderCacheOptions {
            directory: Some(cache.to_owned()),
            ..ShaderCacheOptions::default()
        },
        ..DeviceOptions::default()
    };
    Device::system_default_with(&options).unwrap()
}

/// The live device's queues keep their submissions in flight exactly where its driver has the
/// fact, and its identity is printed either way.
#[test]
fn the_live_device_relays_its_queue_failures_exactly_where_its_driver_has_the_fact() {
    let scratch = Scratch::new();
    let device = device(&scratch.0);
    let properties = device.properties();
    let description = &properties.description;
    let fact = DriverFact::QueueFailuresReportedByMessenger;
    let has = properties.driver_facts.contains(&fact);
    println!(
        "RELAY-FACT {:?} {} ({:#x}/{:#x}, driver version {:#x}): the fact {has}",
        description.driver_id,
        properties.name,
        properties.vendor_id,
        properties.device_id,
        description.driver_version,
    );
    let queues = device.shared().queues.len();
    let relays = (0..queues).any(|queue| device.shared().flights(queue).is_some());
    // The relay also needs the instance's messenger, which an adopted instance and one without
    // `VK_EXT_debug_utils` lack: so the fact is necessary, not sufficient.
    assert!(
        !relays || has,
        "the device relays its queue failures without the fact that earns it"
    );
    assert!(
        relays || !has,
        "the device has the fact that earns the relay and relays nothing: its instance has no \
         messenger, or the relay was not installed"
    );
    drop(device);
}
