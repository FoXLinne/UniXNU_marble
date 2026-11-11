/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The battery manager the audio DSP runs (PMIC GLINK, channel "PMIC_RTR_ADSP_APPS"): messages start with a 12-byte header (owner, type,
 * opcode); a property read is a request {battery 0, property, 0} answered by {property, value, result}; the DSP also sends notifications
 * when something changed. This is the message codec, the refresh cycle (one request in flight, the next one only when it is answered
 * or the owner's timer gives up on it) and the mapping to what iOS reads from a power source. No I/O, no waits: the owner sends the
 * bytes and feeds back what arrives. Charging itself stays with the DSP firmware; the one setting written here is the USB input current
 * limit on a computer's port, which the firmware keeps low until the host side asks for more (as a USB device stack does once the
 * port configured it). */
#ifndef MARBLE_BATTMGR_H
#define MARBLE_BATTMGR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MARBLE_BATTMGR_OWNER 32778u  /* the battery manager; 32779 is the USB-C port manager on the same channel */
#define MARBLE_BATTMGR_TYPE_REQUEST 1u
#define MARBLE_BATTMGR_TYPE_NOTIFY 2u
#define MARBLE_BATTMGR_OP_REQUEST_NOTIFICATION 0x04u
#define MARBLE_BATTMGR_OP_NOTIFICATION 0x07u
#define MARBLE_BATTMGR_OP_BATTERY_GET 0x30u
#define MARBLE_BATTMGR_OP_USB_GET 0x32u
#define MARBLE_BATTMGR_OP_USB_SET 0x33u
#define MARBLE_BATTMGR_REQUEST_BYTES 24u
#define MARBLE_BATTMGR_ENABLE_BYTES 28u

/* Properties from MiCode marble-s-oss qti_battery_charger.c. */
enum {
    BmStatus, BmPresent, BmCapacity, BmVoltage, BmCurrent, BmTemperature, BmCharge,
    BmUsbOnline, BmUsbType, BmUsbVoltage, BmUsbCurrent, BmTimeToEmpty, BmTimeToFull,
    BmFull, BmDesign, BmCycles, BmUsbAdapter, BmUsbLimit, BmUsbMax, BmChargeType,
    BmThermalLevel, BmChargeCurrent, BmFields
};

typedef struct {
    int32_t value[BmFields];
    uint32_t valid;               /* bit per field */
    uint32_t cycle_valid, completed_valid; /* successful reads in the active and completed cycles */
    uint8_t cursor, in_flight, enabled, slow_due;
    uint8_t sample_only, sample_wanted;
    uint32_t cycles_done, responses, notifications, errors, timeouts, ignored;
    uint32_t refresh_wanted;      /* a notification asked for a new cycle while one ran */
    int last_timeout;             /* the field the last timeout skipped, -1 none */
    /* the input current limit on a computer's port: what a standard port (SDP) gets, 0 to leave the firmware's; the port type the
     * tries count for; the value due to be sent, the one in flight, tries for this port type, sets the firmware took, the last result
     * (the firmware's, or -60 timed out) */
    uint32_t sdp_limit_ua;
    int32_t limit_port;
    uint32_t limit_due, limit_in_flight, limit_tries, limit_sets;
    int32_t limit_result;
} MarbleBattmgr;

/* Firmware current is discharge-positive; IOPMPowerSource current is charge-positive.
 * Capacity and temperature use hundredths; voltage/current use micro-units;
 * charge counter is mAh on marble; full/design capacity is uAh. */
typedef struct {
    int present, external, charging, fully_charged, critical;
    int percent;                  /* 0..100 */
    int charge_known;             /* percent comes from a reading, not a default */
    int voltage_mv, current_ma;   /* current: positive while the battery takes charge */
    int temperature, temperature_valid;  /* valid: a reading in the range a battery can have */
    int raw_mah, full_mah, design_mah, cycles;
    int time_to_empty_min, time_to_full_min;  /* -1 unknown */
    uint32_t adapter_family;      /* kIOPSFamilyCode value, 0 when no adapter */
    int adapter_mv, adapter_ma, adapter_watts;
} MarbleBatterySnapshot;

enum {BmIgnored = 0, BmAnswer = 1, BmCycleDone = 2, BmNotified = 3, BmEnabled = 4, BmSampleDone = 5};

void marble_battmgr_init(MarbleBattmgr *b);
/* The notification enable request (sent once the channel is up). */
uint32_t marble_battmgr_enable_request(uint8_t out[MARBLE_BATTMGR_ENABLE_BYTES]);
/* Starts a refresh cycle (no effect while one runs: it is remembered and runs after). slow: also the fields that hardly change. */
void marble_battmgr_refresh(MarbleBattmgr *b, int slow);
/* One measured current for the gas-gauge logger; shares the request queue. */
void marble_battmgr_sample(MarbleBattmgr *b);
/* The next request: an input current limit due after the last cycle, else the running cycle's next field; 0 when nothing is to be
 * sent now (one in flight, or nothing due). */
uint32_t marble_battmgr_next(MarbleBattmgr *b, uint8_t out[MARBLE_BATTMGR_REQUEST_BYTES]);
/* The owner's request timer ran out: the field is skipped (a limit set counts as tried). */
int marble_battmgr_timeout(MarbleBattmgr *b);
/* A message from the channel; returns one of the Bm* results. */
int marble_battmgr_receive(MarbleBattmgr *b, const uint8_t *data, uint32_t length);
/* The power source values from the fields read so far. */
void marble_battmgr_snapshot(const MarbleBattmgr *b, MarbleBatterySnapshot *s);

#ifdef __cplusplus
}
#endif
#endif
