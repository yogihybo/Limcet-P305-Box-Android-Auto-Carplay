#include "vehicle_profiles.h"
#include "uart_protocol.h"

/* State Tracking */
static uint8_t g_last_swc_status_byte = 0; /* mirrors the real firmware's own
                                               status byte (0x80/0x04 bits) */
static uint8_t g_last_swc_field2 = 0;      /* mirrors the real firmware's own
                                               2-bit field (values 2/3) */
static bool    g_last_reverse_state = false;

/* ========================================================================== */
/* Toyota Prado 150 CAN Handlers (Mode 1 -- see vehicle_profiles.h for the    */
/* real-vs-approximated status of each CAN ID/decode)                        */
/* ========================================================================== */

/* Real firmware's Mode 1 SWC handler (0x0800A939, re-traced this session) checks
 * 4 distinct real conditions, each debounced/edge-triggered independently, and
 * sends a real internal 16-bit code (low byte 0x01 = pressed, high byte = key
 * id) via an internal queue helper (0x8006228):
 *   - data byte, bit 0x80 -> SWC_RAW_CODE_0x41
 *   - data byte, bit 0x04 -> SWC_RAW_CODE_0x40
 *   - a DIFFERENT data byte's low 2 bits == 3 -> SWC_RAW_CODE_0x11
 *   - a DIFFERENT data byte's low 2 bits == 2 -> SWC_RAW_CODE_0x12
 * Real ambiguity, stated plainly: the real firmware reads these from a
 * 20-byte-stride per-CAN-ID history struct (offsets 0xc and 0xb within it),
 * not directly from the raw 8-byte CAN payload -- the exact wire-byte index
 * each struct offset corresponds to was NOT traced back to data[0..7]. The
 * mapping below (0x80/0x04 bits on data[0], the 2-bit field on data[1]) is a
 * reasonable placement (data[0] also matches reverse's real bit, which reads
 * the SAME struct offset 0xc as this handler's first two conditions -- see
 * handle_toyota_prado_reverse() below), not an independently wire-verified one.
 * Which PHYSICAL button each SWC_RAW_CODE_* is (volume/mode/etc.) is also not
 * confirmed -- see vehicle_profiles.h. */
static void handle_toyota_prado_swc(const CanFrame *f) {
    if (f->dlc < 2) return;

    uint8_t status = f->data[0];
    uint8_t field2 = f->data[1] & 0x03;

    if ((status & 0x80) && !(g_last_swc_status_byte & 0x80)) {
        uart_send_key_event(SWC_RAW_CODE_0x41, true);
    } else if (!(status & 0x80) && (g_last_swc_status_byte & 0x80)) {
        uart_send_key_event(SWC_RAW_CODE_0x41, false);
    }

    if ((status & 0x04) && !(g_last_swc_status_byte & 0x04)) {
        uart_send_key_event(SWC_RAW_CODE_0x40, true);
    } else if (!(status & 0x04) && (g_last_swc_status_byte & 0x04)) {
        uart_send_key_event(SWC_RAW_CODE_0x40, false);
    }

    if (field2 == 3 && g_last_swc_field2 != 3) {
        uart_send_key_event(SWC_RAW_CODE_0x11, true);
    } else if (field2 != 3 && g_last_swc_field2 == 3) {
        uart_send_key_event(SWC_RAW_CODE_0x11, false);
    }

    if (field2 == 2 && g_last_swc_field2 != 2) {
        uart_send_key_event(SWC_RAW_CODE_0x12, true);
    } else if (field2 != 2 && g_last_swc_field2 == 2) {
        uart_send_key_event(SWC_RAW_CODE_0x12, false);
    }

    g_last_swc_status_byte = status;
    g_last_swc_field2 = field2;
}

/* Real firmware's steering-angle handler (0x0800956A) on CAN ID 0x025:
 * Bytes 0-1 represent the 16-bit steering angle sensor value. */
static void handle_toyota_prado_steering(const CanFrame *f) {
    if (f->dlc < 2) return;

    int16_t angle = (int16_t)(((uint16_t)f->data[0] << 8) | f->data[1]);
    uart_send_steering_angle(angle);
}

/* Real firmware's reverse handler (0x080092B6) in live_app_1302.bin:
 * Reads CAN ID 0x1D0 data byte 1:
 * Checks if data[1] == 0x02 (Reverse gear engaged on Toyota Prado 150). */
static void handle_toyota_prado_reverse(const CanFrame *f) {
    if (f->dlc < 2) return;

    bool reverse = (f->data[1] == 0x02);
    if (reverse != g_last_reverse_state) {
        g_last_reverse_state = reverse;
        uart_send_reverse_state(reverse);
    }
}

/* Real firmware's body controller handler (0x080095E4) on CAN ID 0x622:
 * Byte 2 bit 4 (0x10) reports headlamps / illumination status. */
static void handle_toyota_prado_body(const CanFrame *f) {
    if (f->dlc < 3) return;

    bool lights_on = (f->data[2] & 0x10) != 0;
    uart_send_headlights_state(lights_on);
}

/* Generic Fallback Handlers */
static void handle_generic_speed(const CanFrame *f) {
    (void)f;
}

/* ========================================================================== */
/* Mode Tables                                                                */
/* ========================================================================== */

/* Mode 1: Toyota Prado 150 Primary Profile
 * Confirmed via live vehicle firmware dump (0x0800B9F4 dispatch table) */
static const CanDispatchEntry g_mode1_table[] = {
    { TOYOTA_PRADO_CAN_STEERING, handle_toyota_prado_steering }, /* 0x025 */
    { TOYOTA_PRADO_CAN_GEAR,     handle_toyota_prado_reverse },  /* 0x1D0 */
    { TOYOTA_PRADO_CAN_POWER,    handle_generic_speed },         /* 0x396 */
    { TOYOTA_PRADO_CAN_BODY,     handle_toyota_prado_body },     /* 0x622 */
};

/* Mode 2: Profile 2 (real IDs, already correct before this session's fix) */
static const CanDispatchEntry g_mode2_table[] = {
    { 0x110, handle_toyota_prado_swc },
    { 0x220, handle_toyota_prado_steering },
    { 0x170, handle_toyota_prado_reverse }
};

/* Mode 3: Profile 3 (real IDs, already correct before this session's fix) */
static const CanDispatchEntry g_mode3_table[] = {
    { 0x168, handle_toyota_prado_swc },
    { 0x135, handle_toyota_prado_steering },
    { 0x214, handle_toyota_prado_reverse }
};

void vehicle_profiles_init(void) {
    g_last_swc_status_byte = 0;
    g_last_swc_field2 = 0;
    g_last_reverse_state = false;
}

const CanDispatchEntry* vehicle_get_dispatch_table(uint8_t mode, uint8_t *out_count) {
    if (!out_count) return 0;

    switch (mode) {
        case 1:
            *out_count = sizeof(g_mode1_table) / sizeof(g_mode1_table[0]);
            return g_mode1_table;
        case 2:
            *out_count = sizeof(g_mode2_table) / sizeof(g_mode2_table[0]);
            return g_mode2_table;
        case 3:
            *out_count = sizeof(g_mode3_table) / sizeof(g_mode3_table[0]);
            return g_mode3_table;
        default:
            *out_count = 0;
            return 0;
    }
}
