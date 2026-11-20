/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Derived from UniXNU/QcaBluetooth userland/BlueTool.c (fa528145).
 * HCI setup, keys, pairing and profiles belong to the native daemon. */
#include <dispatch/dispatch.h>
#include <errno.h>
#include <os/log.h>
#include <stdint.h>
#include <string.h>
#include <sys/sysctl.h>
#include <xpc/xpc.h>

extern xpc_connection_t qbt_xpc_listener(const char *, dispatch_queue_t, uint64_t)
    __asm__("_xpc_connection_create_mach_service");

static os_log_t logger;

static int controller_state(const char *name)
{
    int result = EIO;
    size_t size = sizeof(result);
    if (sysctlbyname(name, &result, &size, NULL, 0)) return errno;
    return size == sizeof(result) ? result : EIO;
}

static void request(xpc_connection_t peer, xpc_object_t message)
{
    if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) return;
    xpc_object_t reply = xpc_dictionary_create_reply(message);
    if (!reply) return;
    const char *operation = xpc_dictionary_get_string(message, "kMsgId");
    int error = ENOTSUP;
    if (operation && !strcmp(operation, "RunBuiltinScript")) {
        xpc_object_t args = xpc_dictionary_get_value(message, "kMsgArgs");
        const char *script = args && xpc_get_type(args) == XPC_TYPE_DICTIONARY ?
            xpc_dictionary_get_string(args, "script") : NULL;
        if (script && (!strcmp(script, "boot") || !strcmp(script, "init")))
            error = controller_state("kern.qbt_power_on");
        else if (script && !strcmp(script, "deepsleep")) {
            /* Socket teardown owns power-off. A live link can use UART in-band sleep. */
            error = controller_state("kern.qbt_boot");
            if (error == ENETDOWN) error = 0;
        } else error = EINVAL;
        os_log(logger, "script %{public}s status %d", script ? script : "missing", error);
    }
    xpc_dictionary_set_int64(reply, "result", error ? 1 : 0);
    if (!error) {
        char firmware[64] = {0};
        size_t size = sizeof(firmware) - 1;
        (void)sysctlbyname("kern.qbt_firmware", firmware, &size, NULL, 0);
        xpc_dictionary_set_int64(reply, "manufacturer", 29);
        xpc_dictionary_set_string(reply, "firmware", firmware);
    } else if (operation && !strcmp(operation, "RunCommand")) {
        xpc_dictionary_set_string(reply, "output", "Unsupported controller command");
    }
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
}

int main(void)
{
    logger = os_log_create("org.unixnu.bluetooth", "BlueTool");
    dispatch_queue_t queue = dispatch_queue_create("org.unixnu.bluetooth.bootstrap", DISPATCH_QUEUE_SERIAL);
    xpc_connection_t listener = qbt_xpc_listener("com.apple.BlueTool", queue, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) return 1;
    xpc_object_t entitled = xpc_bool_create(true);
    int error = xpc_connection_set_peer_entitlement_matches_value_requirement(listener,
        "com.apple.bluetooth.control", entitled);
    xpc_release(entitled);
    if (error) return 1;
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        uid_t uid = xpc_connection_get_euid(peer);
        if (uid != 0 && uid != 501) {
            xpc_connection_cancel(peer);
            return;
        }
        xpc_connection_set_target_queue(peer, queue);
        xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
            request(peer, event);
        });
        xpc_connection_resume(peer);
    });
    xpc_connection_resume(listener);
    dispatch_main();
}
