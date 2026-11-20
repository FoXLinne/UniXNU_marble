/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef QBT_H
#define QBT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct qbt;
enum qbt_controller_family { QBT_WCN7850 = 0, QBT_WCN6855 = 1 };
/* All protocol entry points run on one executor. IRQ and socket producers use
 * their own queues and wake it; callbacks never reenter a command operation. */
struct qbt_platform {
    void *context;
    void *(*allocate)(void *, size_t);
    void (*release)(void *, void *, size_t);
    uint64_t (*now_ns)(void *);
    size_t (*uart_write)(void *, const uint8_t *, size_t);
    bool (*uart_drained)(void *);
    int (*uart_speed)(void *, uint32_t);
    int (*uart_flow)(void *, bool);
    /* clock(false) succeeds only with a real RX wake source armed. It may call qbt_receive and qbt_rx_wake while it
     * stops the UART; qbt_step turns the clock back on when a wake indication arrived that way. */
    int (*clock)(void *, bool);
    void (*pump)(void *);
    void (*wait_until)(void *, uint64_t);
    size_t (*receive)(void *, const uint8_t *, size_t);
    /* Returned firmware remains valid until firmware_put; NVM is copied. */
    int (*firmware_get)(void *, const char *, const uint8_t **, size_t *);
    void (*firmware_put)(void *, const uint8_t *, size_t);
    void (*log)(void *, int, const char *);
    /* Optional diagnostics on the executor. For ACL (type 2) bytes contain
     * the four-byte HCI header only. Command and event bytes exclude H4 type.
     * The pointer remains valid for this callback. */
    void (*trace_hci)(void *, bool tx, uint8_t type, const uint8_t *, size_t);
    enum qbt_controller_family controller_family;
};
enum qbt_discovery_command {
    QBT_INQUIRY,
    QBT_LE_SCAN_PARAMS,
    QBT_LE_SCAN_ENABLE,
    QBT_LE_EXT_SCAN_PARAMS,
    QBT_LE_EXT_SCAN_ENABLE,
    QBT_DISCOVERY_COMMANDS,
};
struct qbt_discovery_stats {
    uint32_t sent[QBT_DISCOVERY_COMMANDS];
    uint32_t replied[QBT_DISCOVERY_COMMANDS];
    uint8_t status[QBT_DISCOVERY_COMMANDS];
    uint32_t inquiry_results, le_reports, le_extended_reports;
};
struct qbt_stats {
    uint64_t tx_bytes, rx_bytes, host_bytes, delivered_bytes;
    uint64_t commands, timeouts, malformed, unmatched_events;
    uint64_t wake_ind, wake_ack, sleep_ind, wake_retries, clock_sleeps;
    uint64_t backpressure, tx_rejected, resets;
    uint64_t tlv_sent, tlv_acks, tlv_final_status, hardware_errors, last_hardware_error;
    /* clock_reverts: stops undone because a wake indication arrived inside clock(false).
     * tx_refused: writes an idle UART did not take. */
    uint64_t clock_reverts, tx_refused;
    struct qbt_discovery_stats discovery;
};
struct qbt_info {
    unsigned state; /* 0 off, 1 initializing, 2 ready, 3 failed */
    int error;
    uint32_t baud, product, soc;
    uint16_t rom, patch;
    char firmware[64];
};
struct qbt *qbt_create(const struct qbt_platform *);
void qbt_destroy(struct qbt *);
/* Returns consumed input; the UART retains unconsumed bytes under backpressure. */
size_t qbt_receive(struct qbt *, const uint8_t *, size_t);
/* Atomic enqueue: 0 accepted, negative errno leaves ownership with the caller. */
int qbt_transmit(struct qbt *, const uint8_t *, size_t);
size_t qbt_transmit_capacity(const struct qbt *);
void qbt_step(struct qbt *);
uint64_t qbt_deadline(const struct qbt *);
void qbt_set_service(struct qbt *, bool);
void qbt_rx_wake(struct qbt *);
/* Abort command waiters promptly after a UART/DMA fault on the executor. */
void qbt_fail(struct qbt *, int error);
/* address uses display/network order; the Linux adapter converts to bdaddr_t. */
int qbt_boot(struct qbt *, uint32_t baud, const uint8_t address[6]);
int qbt_shutdown(struct qbt *);
void qbt_get_info(const struct qbt *, struct qbt_info *);
void qbt_get_stats(const struct qbt *, struct qbt_stats *);
#ifdef __cplusplus
}
#endif
#endif
