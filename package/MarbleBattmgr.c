/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "MarbleBattmgr.h"

/* MiCode marble-s-oss drivers/power/supply/qti_battery_charger.c. */
static const struct {uint8_t opcode, property, slow;} kFields[BmFields] = {
    [BmStatus] = {MARBLE_BATTMGR_OP_BATTERY_GET, 0, 0},
    [BmPresent] = {MARBLE_BATTMGR_OP_BATTERY_GET, 2, 0},
    [BmCapacity] = {MARBLE_BATTMGR_OP_BATTERY_GET, 4, 0},
    [BmVoltage] = {MARBLE_BATTMGR_OP_BATTERY_GET, 7, 0},
    [BmCurrent] = {MARBLE_BATTMGR_OP_BATTERY_GET, 9, 0},
    [BmTemperature] = {MARBLE_BATTMGR_OP_BATTERY_GET, 13, 0},
    [BmCharge] = {MARBLE_BATTMGR_OP_BATTERY_GET, 15, 0},
    [BmUsbOnline] = {MARBLE_BATTMGR_OP_USB_GET, 0, 0},
    [BmUsbType] = {MARBLE_BATTMGR_OP_USB_GET, 6, 0},
    [BmUsbVoltage] = {MARBLE_BATTMGR_OP_USB_GET, 1, 0},
    [BmUsbCurrent] = {MARBLE_BATTMGR_OP_USB_GET, 3, 0},
    [BmTimeToEmpty] = {MARBLE_BATTMGR_OP_BATTERY_GET, 21, 0},
    [BmTimeToFull] = {MARBLE_BATTMGR_OP_BATTERY_GET, 20, 0},
    [BmFull] = {MARBLE_BATTMGR_OP_BATTERY_GET, 18, 1},
    [BmDesign] = {MARBLE_BATTMGR_OP_BATTERY_GET, 17, 1},
    [BmCycles] = {MARBLE_BATTMGR_OP_BATTERY_GET, 16, 1},
    [BmUsbAdapter] = {MARBLE_BATTMGR_OP_USB_GET, 7, 0},
    [BmUsbLimit] = {MARBLE_BATTMGR_OP_USB_GET, 5, 0},
    [BmUsbMax] = {MARBLE_BATTMGR_OP_USB_GET, 4, 0},
    [BmChargeType] = {MARBLE_BATTMGR_OP_BATTERY_GET, 3, 0},
    [BmThermalLevel] = {MARBLE_BATTMGR_OP_BATTERY_GET, 10, 0},
    [BmChargeCurrent] = {MARBLE_BATTMGR_OP_BATTERY_GET, 12, 0},
};
#define NO_CYCLE 0xff

static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void header(uint8_t *out, uint32_t opcode)
{
    put32(out, MARBLE_BATTMGR_OWNER);
    put32(out + 4, MARBLE_BATTMGR_TYPE_REQUEST);
    put32(out + 8, opcode);
}

void marble_battmgr_init(MarbleBattmgr *b)
{
    for (unsigned i = 0; i < sizeof *b; ++i) ((uint8_t *)b)[i] = 0;
    b->cursor = NO_CYCLE;
    b->last_timeout = -1;
    b->sdp_limit_ua = 500000;  /* Configured USB 2 SDP budget. */
    b->limit_port = -1;
}

uint32_t marble_battmgr_enable_request(uint8_t out[MARBLE_BATTMGR_ENABLE_BYTES])
{
    header(out, MARBLE_BATTMGR_OP_REQUEST_NOTIFICATION);
    put32(out + 12, 0);    /* battery 0 */
    put32(out + 16, 0);    /* power state */
    put32(out + 20, 1);    /* low capacity notification threshold, percent */
    put32(out + 24, 100);  /* high */
    return MARBLE_BATTMGR_ENABLE_BYTES;
}

static void skip_to_wanted(MarbleBattmgr *b)
{
    while (b->cursor < BmFields && kFields[b->cursor].slow && !b->slow_due) b->cursor++;
}

static void advance(MarbleBattmgr *b)
{
    b->in_flight = 0;
    b->cursor++;
    skip_to_wanted(b);
}

void marble_battmgr_refresh(MarbleBattmgr *b, int slow)
{
    if (b->cursor != NO_CYCLE) {b->refresh_wanted = 1 + (slow != 0); return;}
    b->slow_due = slow || !(b->valid & (1u << BmFull));
    b->cursor = 0;
    b->in_flight = 0;
    skip_to_wanted(b);
}

uint32_t marble_battmgr_next(MarbleBattmgr *b, uint8_t out[MARBLE_BATTMGR_REQUEST_BYTES])
{
    if (b->in_flight || b->limit_in_flight) return 0;
    if (b->limit_due) {
        header(out, MARBLE_BATTMGR_OP_USB_SET);
        put32(out + 12, 0);
        put32(out + 16, 5);  /* input current limit */
        put32(out + 20, b->limit_due);
        b->limit_in_flight = b->limit_due;
        b->limit_due = 0;
        b->limit_tries++;
        return MARBLE_BATTMGR_REQUEST_BYTES;
    }
    if (b->cursor == NO_CYCLE || b->cursor >= BmFields) return 0;
    header(out, kFields[b->cursor].opcode);
    put32(out + 12, 0);
    put32(out + 16, kFields[b->cursor].property);
    put32(out + 20, 0);
    b->in_flight = 1;
    return MARBLE_BATTMGR_REQUEST_BYTES;
}

/* The input current limit a port type gets from the host side: a standard port (SDP 1) the configured value, a charging downstream port
 * (CDP 3) its 1.5 A; others (dedicated chargers, Type-C current, PD contracts) are the firmware's own. */
static uint32_t wanted_limit(const MarbleBattmgr *b, int port)
{
    return port == 1 ? b->sdp_limit_ua : port == 3 ? 1500000u : 0u;
}

/* After a cycle: a set is due when the port wants a higher limit than the firmware reports, at most three tries per port type. */
static void check_limit(MarbleBattmgr *b)
{
    if (!(b->valid & (1u << BmUsbOnline)) || !b->value[BmUsbOnline]) {b->limit_port = -1; b->limit_tries = 0; b->limit_due = 0; return;}
    const int port = (b->valid >> BmUsbAdapter) & 1 ? b->value[BmUsbAdapter] : (b->valid >> BmUsbType) & 1 ? b->value[BmUsbType] : 0;
    if (port != b->limit_port) {b->limit_port = port; b->limit_tries = 0;}
    const uint32_t want = wanted_limit(b, port);
    if (!want || b->limit_tries >= 3) return;
    if ((b->valid >> BmUsbLimit) & 1 && b->value[BmUsbLimit] >= (int32_t)want - 100000) return;  /* 100 mA under it: input limiting */
    b->limit_due = want;
}

/* Ends the cycle when the cursor ran past the last field; starts the remembered one. Returns 1 when a cycle ended. */
static int finish(MarbleBattmgr *b)
{
    if (b->cursor == NO_CYCLE || b->cursor < BmFields) return 0;
    b->cursor = NO_CYCLE;
    b->cycles_done++;
    check_limit(b);
    if (b->refresh_wanted) {
        const int slow = b->refresh_wanted == 2;
        b->refresh_wanted = 0;
        marble_battmgr_refresh(b, slow);
    }
    return 1;
}

void marble_battmgr_timeout(MarbleBattmgr *b)
{
    if (b->limit_in_flight) {b->limit_in_flight = 0; b->limit_result = -60; b->timeouts++; return;}
    if (b->cursor == NO_CYCLE || !b->in_flight) return;
    b->timeouts++;
    b->last_timeout = b->cursor;
    b->valid &= ~(1u << b->cursor);
    advance(b);
    finish(b);
}

int marble_battmgr_receive(MarbleBattmgr *b, const uint8_t *data, uint32_t length)
{
    if (length < 12 || le32(data) != MARBLE_BATTMGR_OWNER) {b->ignored++; return BmIgnored;}
    const uint32_t type = le32(data + 4), opcode = le32(data + 8);
    if (opcode == MARBLE_BATTMGR_OP_NOTIFICATION && type == MARBLE_BATTMGR_TYPE_NOTIFY && length >= 16) {
        b->notifications++;
        marble_battmgr_refresh(b, 0);
        return BmNotified;
    }
    if (type != MARBLE_BATTMGR_TYPE_REQUEST) {b->ignored++; return BmIgnored;}
    if (opcode == MARBLE_BATTMGR_OP_REQUEST_NOTIFICATION) {b->enabled = 1; return BmEnabled;}
    if (opcode == MARBLE_BATTMGR_OP_USB_SET) {
        if (!b->limit_in_flight || length < 24 || le32(data + 12) != 5) {b->ignored++; return BmIgnored;}
        b->limit_result = (int32_t)le32(data + 20);
        if (!b->limit_result) b->limit_sets++;
        b->limit_in_flight = 0;
        marble_battmgr_refresh(b, 0);  /* read the limit back */
        return BmAnswer;
    }
    if (b->cursor == NO_CYCLE || b->cursor >= BmFields || !b->in_flight || length < 24) {b->ignored++; return BmIgnored;}
    const uint32_t property = le32(data + 12), value = le32(data + 16), result = le32(data + 20);
    if (opcode != kFields[b->cursor].opcode || property != kFields[b->cursor].property) {b->ignored++; return BmIgnored;}
    b->responses++;
    if (result) {
        b->errors++;
        b->valid &= ~(1u << b->cursor);
    } else {
        b->value[b->cursor] = (int32_t)value;
        b->valid |= 1u << b->cursor;
    }
    advance(b);
    return finish(b) ? BmCycleDone : BmAnswer;
}

/* POWER_SUPPLY_USB_TYPE (unknown 0, SDP 1, DCP 2, CDP 3, ACA 4, C 5, PD 6, PD_DRP 7, PD_PPS 8) to the kIOPSFamilyCode values (the
 * USB family's error space, 0xe0004000 + n: host 0, dedicated charging port 4, downstream charging port 5, unknown 7, Type-C 9, PD 10). */
static uint32_t family(int type)
{
    switch (type) {
    case 1: return 0xe0004000u;
    case 2: return 0xe0004004u;
    case 3: return 0xe0004005u;
    case 5: return 0xe0004009u;
    case 6: case 7: case 8: return 0xe000400au;
    default: return 0xe0004007u;
    }
}

void marble_battmgr_snapshot(const MarbleBattmgr *b, MarbleBatterySnapshot *s)
{
#define HAS(f) ((b->valid >> (f)) & 1u)
#define V(f) (b->value[(f)])
    const int status = HAS(BmStatus) ? V(BmStatus) : 0;  /* POWER_SUPPLY_STATUS: 1 charging, 2 discharging, 3 not charging, 4 full */
    s->present = HAS(BmPresent) ? V(BmPresent) != 0 : HAS(BmCapacity);
    s->external = HAS(BmUsbOnline) && V(BmUsbOnline) != 0;
    const int percent = HAS(BmCapacity) ? (V(BmCapacity) + 50) / 100 : 0;
    s->percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    s->charge_known = HAS(BmCapacity);
    s->charging = s->external && status == 1;
    s->fully_charged = s->external && (status == 4);
    s->critical = s->present && !s->external && s->percent <= 2;
    s->voltage_mv = HAS(BmVoltage) ? V(BmVoltage) / 1000 : 0;
    s->current_ma = HAS(BmCurrent) ? -V(BmCurrent) / 1000 : 0;
    /* -40 to 90 C; the firmware answers a fixed out-of-range value when it has no reading */
    s->temperature_valid = HAS(BmTemperature) && V(BmTemperature) >= -4000 && V(BmTemperature) <= 9000;
    s->temperature = s->temperature_valid ? V(BmTemperature) : 0;
    s->raw_mah = HAS(BmCharge) ? V(BmCharge) : 0;
    s->full_mah = HAS(BmFull) ? V(BmFull) / 1000 : 0;
    s->design_mah = HAS(BmDesign) ? V(BmDesign) / 1000 : 0;
    s->cycles = HAS(BmCycles) ? V(BmCycles) : 0;
    /* The gauge reports times in seconds, and a negative or huge value when it has none. */
    s->time_to_empty_min = HAS(BmTimeToEmpty) && V(BmTimeToEmpty) > 0 && V(BmTimeToEmpty) < 100 * 3600 ? V(BmTimeToEmpty) / 60 : -1;
    s->time_to_full_min = HAS(BmTimeToFull) && V(BmTimeToFull) > 0 && V(BmTimeToFull) < 100 * 3600 ? V(BmTimeToFull) / 60 : -1;
    if (s->external) {
        s->adapter_family = family(HAS(BmUsbType) ? V(BmUsbType) : 0);
        s->adapter_mv = HAS(BmUsbVoltage) ? V(BmUsbVoltage) / 1000 : 0;
        s->adapter_ma = HAS(BmUsbCurrent) ? V(BmUsbCurrent) / 1000 : 0;
        if (s->adapter_mv < 0) s->adapter_mv = 0;
        if (s->adapter_ma < 0) s->adapter_ma = -s->adapter_ma;
        s->adapter_watts = (int)(((int64_t)s->adapter_mv * s->adapter_ma + 500000) / 1000000);
    } else {
        s->adapter_family = 0;
        s->adapter_mv = s->adapter_ma = s->adapter_watts = 0;
    }
#undef HAS
#undef V
}
