#!/usr/bin/env python3
"""Reproduce the Bluetooth session opening while the controller is shutting down."""
from pathlib import Path
import subprocess
import sys
import tempfile

DEVICE = Path(__file__).resolve().parents[2]
SOURCE = Path(sys.argv[1]) if len(sys.argv) > 1 else DEVICE / 'ios18/sources/QcaBluetooth/src/qbt_transport.c'
text = SOURCE.read_text()


def function(name):
    signature = text.index(f' {name}(')
    start = text.rfind('\n', 0, signature) + 1
    body = text.index('{', signature)
    depth = 1
    for cursor in range(body + 1, len(text)):
        depth += (text[cursor] == '{') - (text[cursor] == '}')
        if not depth:
            return text[start:cursor + 1]
    raise ValueError(name)


headers = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define BT_SK_FIFO 64u
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#ifndef __unused
#define __unused __attribute__((unused))
#endif

typedef int errno_t;
typedef void *kern_nexus_provider_t, *kern_nexus_t, *kern_channel_ring_t, *kern_channel_slot_t, *kern_packet_t, *kern_buflet_t;
static unsigned power_requests, power_wakeups, boot_wakeups, notifications;
static int bt_sk_lock;
static int fail_request;
static uint32_t bt_sk_speed;
static uint64_t bt_sk_kicks_tx;
static const uint8_t reset[] = {1, 0x03, 0x0c, 0};
static bool slot_available;
static struct {
    int boot, power_result;
    bool connected, closing, power_pending;
    unsigned busy;
    uint32_t rx_head, rx_len, tx_head, tx_len, spec[3];
    uint8_t tx[BT_SK_FIFO];
    char firmware[128];
    kern_channel_ring_t rxring, txring;
} bt_sk;
static void lck_mtx_lock(int *lock) {assert(!*lock); *lock = 1;}
static void lck_mtx_unlock(int *lock) {assert(*lock); *lock = 0;}
static void bt_kick(void) {assert(!bt_sk_lock);}
static void bt_sk_termios_default(void) {assert(bt_sk_lock);}
static int power(void *context, bool enabled) {
    assert(!context && enabled && bt_sk_lock);
    ++power_requests;
    return fail_request;
}
static struct {void *context; int (*power)(void *, bool);} bt_ops = {NULL, power};
static void wakeup(void *address) {
    if (address == &bt_sk.power_pending) ++power_wakeups;
    if (address == &bt_sk.boot) ++boot_wakeups;
}
static int kern_channel_notify(kern_channel_ring_t ring, unsigned flags) {
    assert(!bt_sk_lock && bt_sk.busy && !flags && ring);
    ++notifications;
    return 0;
}
static size_t test_strlcpy(char *to, const char *from, size_t n) {
    (void)snprintf(to, n, "%s", from);
    return strlen(from);
}
#define strlcpy test_strlcpy
static kern_channel_slot_t kern_channel_get_next_slot(kern_channel_ring_t ring, kern_channel_slot_t prev, void *props) {
    assert(ring == bt_sk.txring && !props);
    return slot_available && !prev ? ring : NULL;
}
static kern_packet_t kern_channel_slot_get_packet(kern_channel_ring_t ring, kern_channel_slot_t slot) {assert(ring == slot); return slot;}
static kern_buflet_t kern_packet_get_next_buflet(kern_packet_t ph, kern_buflet_t previous) {assert(!previous); return ph;}
static uint32_t kern_packet_get_data_length(kern_packet_t ph) {assert(ph); return sizeof(reset);}
static uint32_t kern_buflet_get_data_offset(kern_buflet_t buf) {assert(buf); return 0;}
static uint32_t kern_buflet_get_data_limit(kern_buflet_t buf) {assert(buf); return sizeof(reset);}
static const void *kern_buflet_get_data_address(kern_buflet_t buf) {assert(buf); return reset;}
static void kern_channel_advance_slot(kern_channel_ring_t ring, kern_channel_slot_t prev) {assert(ring == prev); slot_available = false;}
static int kern_channel_reclaim(kern_channel_ring_t ring) {assert(ring); return 0;}
'''

main = r'''
int main(void) {
    uint8_t tx[32];
    bt_sk.connected = true;
    bt_sk.rxring = (void *)1;
    bt_sk.txring = (void *)2;
    bt_sk.spec[0] = sizeof(tx);
    bt_sk.boot = ENETDOWN;
    lck_mtx_lock(&bt_sk_lock);
    assert(!bt_sk_request_power_locked());
    assert(!bt_sk_request_power_locked());
    lck_mtx_unlock(&bt_sk_lock);
    assert(power_requests == 1 && bt_sk.boot == -1 && bt_sk.power_result == EINPROGRESS);
    slot_available = true;
    assert(!bt_sk_sync_tx(NULL, NULL, bt_sk.txring, 0));
    assert(!slot_available && bt_sk.tx_len == sizeof(reset));
    assert(!qbt_transport_tx_take(tx, sizeof(tx)));

    qbt_transport_power_result(false, 0, NULL, 3000000);
    assert(bt_sk.power_pending && bt_sk.boot == -1 && bt_sk.power_result == EINPROGRESS);
    assert(!power_wakeups && !boot_wakeups && bt_sk.tx_len == sizeof(reset));
    assert(!qbt_transport_tx_take(tx, sizeof(tx)));

    qbt_transport_power_result(true, 0, "WCN6855", 3000000);
    assert(!bt_sk.power_pending && !bt_sk.boot && !bt_sk.power_result);
    assert(power_wakeups == 1 && boot_wakeups == 1 && notifications >= 2);
    assert(qbt_transport_tx_take(tx, sizeof(tx)) == sizeof(reset));
    assert(!memcmp(tx, reset, sizeof(reset)) && !bt_sk.tx_len);

    qbt_transport_power_result(false, 0, NULL, 3000000);
    assert(bt_sk.boot == ENETDOWN && power_wakeups == 1);
    lck_mtx_lock(&bt_sk_lock);
    fail_request = EBUSY;
    assert(bt_sk_request_power_locked() == EBUSY && !bt_sk.power_pending);
    fail_request = 0;
    assert(!bt_sk_request_power_locked());
    lck_mtx_unlock(&bt_sk_lock);
    slot_available = true;
    assert(!bt_sk_sync_tx(NULL, NULL, bt_sk.txring, 0) && !slot_available);
    qbt_transport_power_result(true, EAGAIN, NULL, 3000000);
    assert(!bt_sk.power_pending && bt_sk.power_result == EAGAIN && !bt_sk.tx_len);
    slot_available = true;
    assert(bt_sk_sync_tx(NULL, NULL, bt_sk.txring, 0) == EAGAIN && slot_available);
    assert(!qbt_transport_tx_take(tx, sizeof(tx)));
    puts("Bluetooth reopen: shutdown completion leaves startup pending, initial HCI bytes survive, failures clear the queue");
}
'''

with tempfile.TemporaryDirectory(prefix='qbt-reopen-') as directory:
    source = Path(directory) / 'reopen.c'
    executable = Path(directory) / 'reopen'
    source.write_text(headers + '\n'.join(function(name) for name in (
        'bt_sk_fifo_put', 'bt_sk_fifo_get', 'bt_sk_notify', 'bt_sk_io_status',
        'bt_sk_request_power_locked', 'qbt_transport_tx_take', 'bt_sk_sync_tx',
        'qbt_transport_power_result',
    )) + main)
    subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', source, '-o', executable], check=True)
    subprocess.run([executable], check=True)
