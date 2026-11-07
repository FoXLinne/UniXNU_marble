//! Optional device extensions (`DeviceOptions::optional_device_extensions`): each is enabled
//! where the device lists it and the instance enables what it requires, and a device that cannot
//! enable one is made without it, never refused; `Device::enables_extension` says which.
//!
//! A device test: compiled only under the validation crate's GPU safety cfgs (`m2v_gpu`,
//! `m2v_cpu_icd`), on the kind of device the cfg selects (MoltenVK and lavapipe, both of which
//! list `VK_KHR_swapchain`). Nothing runs on the GPU.

use crate::{Device, DeviceKind, DeviceOptions, MessageSink};
use std::ffi::{CStr, CString};

/// Options of the kind the cfg selects, with a message sink of their own (a group of devices no
/// other test makes), the instance extensions `instance` and the optional extensions `optional`.
fn options(instance: &[&CStr], optional: &[&CStr]) -> DeviceOptions {
    let owned = |names: &[&CStr]| names.iter().map(|&name| name.to_owned()).collect();
    DeviceOptions {
        kinds: match cfg!(m2v_cpu_icd) {
            true => vec![DeviceKind::Cpu],
            false => vec![
                DeviceKind::Discrete,
                DeviceKind::Integrated,
                DeviceKind::Virtual,
            ],
        },
        messages: Some(MessageSink::new(|_| {})),
        instance_extensions: owned(instance),
        optional_device_extensions: owned(optional),
        ..DeviceOptions::default()
    }
}

#[test]
fn an_optional_extension_is_enabled_where_the_device_can() {
    let swapchain = c"VK_KHR_swapchain";
    let maintenance1 = c"VK_EXT_swapchain_maintenance1";
    let unlisted = c"VK_M2V_no_such_extension";
    // Without `VK_KHR_surface` on the instance, the swapchain cannot be enabled: the device is
    // made without it (as a device extension it is refused, `InstanceExtensionRequired`), and
    // without the extension no device lists.
    let device = Device::system_default_with(&options(&[], &[swapchain, unlisted])).unwrap();
    assert!(!device.enables_extension(swapchain));
    assert!(!device.enables_extension(unlisted));
    let required = DeviceOptions {
        device_extensions: vec![CString::from(swapchain)],
        ..options(&[], &[])
    };
    assert!(Device::system_default_with(&required).is_err());
    // With it, the device enables the swapchain; `VK_EXT_swapchain_maintenance1` needs
    // `VK_EXT_surface_maintenance1` too, which the instance lacks, so it is left out.
    let surface = c"VK_KHR_surface";
    let device =
        Device::system_default_with(&options(&[surface], &[swapchain, maintenance1, unlisted]))
            .unwrap();
    assert!(device.enables_extension(swapchain));
    assert!(!device.enables_extension(maintenance1));
    assert!(!device.enables_extension(unlisted));
    // The runtime's own extensions are answered too: each of the vocabulary's the device
    // enabled, by the name the device listed.
    for extension in &device.enabled().extensions {
        assert!(
            crate::enables::names_of(*extension).any(|name| device.enables_extension(name)),
            "{extension:?}"
        );
    }
}
