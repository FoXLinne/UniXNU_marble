//! What a device test's claim that the validation layer reported nothing holds, from what the
//! device's instance enabled of the layers it asked for.
//!
//! A device opens although a layer its caller asked for is one the loader cannot provide
//! (`device/create.rs`, `tests/missing_layer.rs`): the creation drops the whole layer list, creates
//! the instance with none, and the device says as it is made which layers it was made without
//! ([`LAYERS_DROPPED`]). That is right for a caller that asked for a device and wrong for a claim:
//! a test that requires the validation layer to have reported nothing passes by reporting nothing
//! at all, on every host the loader can give it no layer.
//!
//! So a test states the claim against the instance's own answer ([`Watched::layer_reported_nothing`]):
//! where the layer was enabled the claim holds over everything the layer checks, and where it was
//! not the test says so in its own output (`LAYER-ABSENT <claim>`) rather than passing on an empty
//! list. The decision is the instance's alone -- never the loader's listing, which a layer it lists
//! and cannot load passes alike, and never the host or the driver's name.
//!
//! A device test: compiled only under the validation crate's GPU safety cfgs.

use crate::device::create::LAYERS_DROPPED;
use crate::{DebugMessage, MessageSeverity, MessageSink};
use std::ffi::{CStr, CString};
use std::sync::{Arc, Mutex};

/// The validation layer, which this host may or may not provide.
pub(super) const VALIDATION: &CStr = c"VK_LAYER_KHRONOS_validation";

/// The id name the Vulkan loader gives the messages it reports itself.
const LOADER_MESSAGE: &str = "Loader Message";

/// What the loader's message about a layer it cannot provide names: the check it failed, and what
/// it found (`loader_validate_layers`, Vulkan-Loader `loader/loader.c`).
const VALIDATE_LAYERS: &str = "loader_validate_layers";

/// The rest of that message: the layer is none the loader lists.
const NOT_AVAILABLE: &str = "does not exist in the list of available layers";

/// Whether a message with this id name and text is the loader's own complaint about an instance
/// layer it cannot provide, and nothing wider. The loader reports it through the messenger chained
/// at `vkCreateInstance`, before the call answers `VK_ERROR_LAYER_NOT_PRESENT`.
fn loader_layer_message(id_name: &str, text: &str) -> bool {
    id_name == LOADER_MESSAGE && text.contains(VALIDATE_LAYERS) && text.contains(NOT_AVAILABLE)
}

/// What one message list holds.
#[derive(Default)]
struct Seen {
    /// The messages that fail a run, bounded so a flood cannot grow without limit.
    failing: Vec<DebugMessage>,
    /// Whether a device reporting here announced it was made without the layers it asked for.
    dropped: bool,
    /// Whether a device reporting here asked for the validation layer.
    asked: bool,
}

/// The most messages kept of one list.
const MAX: usize = 64;

/// The messages the devices of one test report, and what their instance enabled of the validation
/// layer they asked for.
#[derive(Clone, Default)]
pub(super) struct Watched(Arc<Mutex<Seen>>);

impl Watched {
    /// The instance layers a device reporting here asks for: the validation layer alone. The
    /// loader may be unable to provide it, which the device itself says
    /// ([`Watched::layer_reported_nothing`] reads that answer, not this list).
    pub(super) fn layers(&self) -> Vec<CString> {
        self.0.lock().unwrap_or_else(|p| p.into_inner()).asked = true;
        vec![VALIDATION.to_owned()]
    }

    /// A sink reporting here, which calls `also` with each message first: a test's own work on the
    /// messages it watches for (a sentinel the sink owns, a line it prints).
    pub(super) fn sink(&self, also: impl Fn(&DebugMessage) + Send + Sync + 'static) -> MessageSink {
        let seen = Arc::clone(&self.0);
        MessageSink::new(move |message: &DebugMessage| {
            also(message);
            let mut seen = seen.lock().unwrap_or_else(|p| p.into_inner());
            // What the instance enabled of the layers the device asked for: informational, so it
            // fails nothing, and kept rather than dropped with the rest, since a claim of what the
            // validation layer reported holds nothing without it.
            if message.id_name == LAYERS_DROPPED {
                seen.dropped = true;
                return;
            }
            let fails = message.severity == MessageSeverity::Error
                || (message.severity == MessageSeverity::Warning && message.kind.validation);
            if fails && message.degradation.is_none() && seen.failing.len() < MAX {
                seen.failing.push(message.clone());
            }
        })
    }

    /// Require that the validation layer reported nothing of `claim` on the devices reporting
    /// here, and say what that holds.
    ///
    /// Every message the layer reported fails here, as `assert!(failures.is_empty())` did. What
    /// the claim then stands on is the instance's answer alone: where a device announced it was
    /// made without the layer it asked for, there was nothing to report, so the claim was not
    /// exercised and the test says so; the loader's own complaint about that same layer passes
    /// with it, and nothing else does.
    ///
    /// # Panics
    ///
    /// Where the layer reported anything that fails a run.
    pub(super) fn layer_reported_nothing(&self, claim: &str) {
        let mut seen = self.0.lock().unwrap_or_else(|p| p.into_inner());
        let dropped = seen.dropped;
        let asked = seen.asked;
        let failing: Vec<DebugMessage> = seen
            .failing
            .split_off(0)
            .into_iter()
            // The device said it was made without the layer, which is what the loader complained
            // about first and all it complained about.
            .filter(|message| !(dropped && loader_layer_message(&message.id_name, &message.text)))
            .collect();
        drop(seen);
        assert!(
            failing.is_empty(),
            "{claim}: the validation layer reported\n{failing:#?}"
        );
        if dropped || !asked {
            eprintln!(
                "LAYER-ABSENT {claim}: the device's instance enabled no validation layer, so it \
                 reported nothing at all; the claim was not exercised here"
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{loader_layer_message, Watched, VALIDATION};
    use crate::device::create::LAYERS_DROPPED;
    use crate::{DebugMessage, MessageKind, MessageSeverity};

    /// A message of the layer's or the loader's, as the sink hears it.
    fn message(severity: MessageSeverity, id_name: &str, text: &str) -> DebugMessage {
        DebugMessage {
            severity,
            kind: MessageKind {
                general: severity == MessageSeverity::Error,
                validation: false,
                performance: false,
            },
            id_name: id_name.to_owned(),
            id: 0,
            text: text.to_owned(),
            degradation: None,
        }
    }

    /// The loader's own complaint about the layer it cannot provide, and nothing wider.
    #[test]
    fn the_loader_s_message_about_a_layer_it_cannot_provide_is_matched_and_nothing_wider() {
        assert!(loader_layer_message(
            "Loader Message",
            "loader_validate_layers: Layer 0 does not exist in the list of available layers"
        ));
        for (id_name, text) in [
            (
                "Loader Message",
                "terminator_CreateInstance: Failed to CreateInstance in ICD 0",
            ),
            (
                "VUID-vkCreateInstance-ppEnabledLayerNames-01727",
                "loader_validate_layers: Layer 0 does not exist in the list of available layers",
            ),
            ("Loader Message", "loader_validate_layers: Layer 0 is fine"),
        ] {
            assert!(!loader_layer_message(id_name, text), "{id_name}: {text}");
        }
    }

    /// The message the loader sends about the layer it cannot provide.
    fn loader_complaint() -> DebugMessage {
        message(
            MessageSeverity::Error,
            "Loader Message",
            "loader_validate_layers: Layer 0 does not exist in the list of available layers",
        )
    }

    /// The message a device sends as it is made without the layers it asked for.
    fn made_without() -> DebugMessage {
        message(
            MessageSeverity::Info,
            LAYERS_DROPPED,
            "metal2vulkan: the device was made without the instance layer(s) \
             VK_LAYER_KHRONOS_validation",
        )
    }

    /// A list whose device asked for the validation layer and was made without it states no claim
    /// of the layer's: the loader's own complaint about that layer passes, and the claim prints
    /// that it was not exercised rather than passing on an empty list. Its own input: no device,
    /// no loader, no host.
    #[test]
    fn a_claim_on_a_device_made_without_the_layer_is_not_exercised() {
        let watched = Watched::default();
        assert_eq!(watched.layers(), [VALIDATION.to_owned()]);
        let sink = watched.sink(|_| {});
        sink.deliver(&loader_complaint());
        sink.deliver(&made_without());
        watched.layer_reported_nothing("nothing of the layer's");
    }

    /// The loader's complaint fails where no device says it was made without the layer: a loader
    /// that refused a layer nothing accounts for is not a clean run.
    #[test]
    #[should_panic(expected = "the validation layer reported")]
    fn the_loader_s_complaint_fails_where_no_device_explains_it() {
        let watched = Watched::default();
        let sink = watched.sink(|_| {});
        sink.deliver(&loader_complaint());
        watched.layer_reported_nothing("nothing of the layer's");
    }

    /// A list whose device enabled the layer it asked for and had it report nothing holds the
    /// claim, and anything the layer did report fails it: the check is not vacuous either way.
    #[test]
    fn a_claim_holds_where_the_layer_was_enabled_and_fails_on_what_it_reported() {
        let watched = Watched::default();
        assert_eq!(watched.layers(), [VALIDATION.to_owned()]);
        let sink = watched.sink(|_| {});
        watched.layer_reported_nothing("every command was valid");
        sink.deliver(&message(
            MessageSeverity::Error,
            "VUID-vkCmdDraw-None-08600",
            "a bound descriptor set was not valid",
        ));
        let failed = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            watched.layer_reported_nothing("every command was valid")
        }));
        assert!(
            failed.is_err(),
            "the claim held over a message the layer reported"
        );
    }

    /// The test's own work on each message runs too, beside the list's.
    #[test]
    fn a_test_s_own_receiver_hears_every_message() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        use std::sync::Arc;
        let heard = Arc::new(AtomicUsize::new(0));
        let counted = Arc::clone(&heard);
        let watched = Watched::default();
        let sink = watched.sink(move |_| {
            counted.fetch_add(1, Ordering::Relaxed);
        });
        sink.deliver(&made_without());
        sink.deliver(&loader_complaint());
        assert_eq!(heard.load(Ordering::Relaxed), 2);
        watched.layer_reported_nothing("nothing of the layer's");
    }
}
