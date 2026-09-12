#include "vehicle_profiles.h"
#include "uart_protocol.h"

/* State Tracking */
static uint8_t g_last_swc_status_byte = 0; /* mirrors the real firmware's own
                                               status byte (0x80/0x04 bits) */
static uint8_t g_last_swc_field2 = 0;      /* mirrors the real firmware's own
                                               2-bit field (values 2/3) */
static bool    g_last_reverse_state = false;
static bool    g_last_lights_state = false;
static bool    g_last_door_state = false;
static uint8_t g_door_debounce = 0;

/* Obstacle distance lookup table matching factory disassembly 0x080093A6:
 *   1           -> 1 (1 bar, closest / red)
 *   2, 3        -> 5 (2 bars / yellow)
 *   4, 5, 6, 7  -> 8 (3-4 bars / green)
 *   0 / default -> 12 (0x0C, no obstacle / off) */
static uint8_t map_radar_distance(uint8_t raw_val) {
    if (raw_val >= 4 && raw_val <= 7) return 8;
    if (raw_val == 2 || raw_val == 3) return 5;
    if (raw_val == 1) return 1;
    return 12;
}

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
 *   - a DIFFERENT data byte's low 2 bits == 2 -> SWC_RAW_CODE_0x12 */
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

/* Real firmware's steering-angle handler (0x080091E0) on CAN ID 0x025:
 * Byte 0 bit 3 is sign/direction bit.
 * 12-bit magnitude in (data[0] & 0x0F) << 8 | data[1].
 * If sign_bit != 0: raw = 0x0FFF - raw, sign = 0. Else sign = 1.
 * Angle scaled: angle = (raw >> 1), clamped to 0x7F.
 * Forwarded to SoC over UART via CMD 0x0A and composite CMD 0x03. */
static void handle_toyota_prado_steering(const CanFrame *f) {
    if (f->dlc < 2) return;

    uint8_t sign_bit = (f->data[0] & 0x08);
    uint16_t raw = (((uint16_t)(f->data[0] & 0x0F)) << 8) | f->data[1];
    uint8_t sign;

    if (sign_bit != 0) {
        sign = 0;
        raw = (0x0FFF - raw) & 0x0FFF;
    } else {
        sign = 1;
    }

    uint16_t scaled = (raw >> 1);
    if (scaled > 0x7F) {
        scaled = 0x7F;
    }

    uart_update_steering_telemetry(sign, (uint8_t)scaled);

    /* Form signed angle in deci-degrees for CMD 0x0A:
     * sign == 0 -> left / negative, sign == 1 -> right / positive */
    int16_t angle = (sign == 0) ? -(int16_t)scaled : (int16_t)scaled;
    uart_send_steering_angle(angle);
}

/* Real firmware's transmission handler (0x0800956A) on CAN ID 0x1D0:
 * Data[3] masked with 0x38 (data[3] & 0x38):
 *   0x20 -> Reverse gear active
 *   0x10 -> Drive / Neutral
 *   0x38 -> Park
 * Note per live testing and dump verification: we only report Reverse state
 * transitions to the SoC via CMD 0x01 (never discrete P/N/D cmds). */
static void handle_toyota_prado_reverse(const CanFrame *f) {
    if (f->dlc < 4) return;

    uint8_t gear = f->data[3] & 0x38;
    bool reverse = (gear == 0x20);

    uart_update_transmission_telemetry(gear);

    if (reverse != g_last_reverse_state) {
        g_last_reverse_state = reverse;
        uart_send_reverse_state(reverse);
        uart_send_composite_vehicle_status();
    }
}

/* Real firmware's speed & radar handler (0x080093D8) on CAN ID 0x396:
 * Decodes 4-channel front and 4-channel rear ultrasonic parking sensors.
 * Distances mapped via table 0x080093A6 into discrete obstacle levels.
 * Emits CMD 0x04 / CMD 0x05 radar telemetry and updates composite CMD 0x03. */
static void handle_toyota_prado_radar_speed(const CanFrame *f) {
    if (f->dlc < 4) return;

    /* Front sensors from Data[1] and Data[2] */
    uint8_t front[4];
    front[0] = map_radar_distance(f->data[1] >> 4);   /* FL */
    front[1] = map_radar_distance(f->data[2] & 0x0F); /* FML */
    front[2] = map_radar_distance(f->data[2] >> 4);   /* FMR */
    front[3] = map_radar_distance(f->data[1] & 0x0F); /* FR */

    /* Rear sensors from Data[3] and Data[2] */
    uint8_t rear[4];
    rear[0] = map_radar_distance(f->data[3] >> 4);   /* RL */
    rear[1] = map_radar_distance(f->data[2] & 0x0F); /* RML */
    rear[2] = map_radar_distance(f->data[2] & 0x0F); /* RMR */
    rear[3] = map_radar_distance(f->data[3] & 0x0F); /* RR */

    uart_update_radar_telemetry(false, rear);
    uart_update_radar_telemetry(true, front);
}

/* Real firmware's body controller handler (0x080095E4) on CAN ID 0x622:
 * Byte 3 bit 4 (0x10): headlamps / illumination status.
 * Byte 5 high nibble (0x10): door ajar / perimeter warning. */
static void handle_toyota_prado_body(const CanFrame *f) {
    if (f->dlc < 4) return;

    bool lights_on = (f->data[3] & 0x10) != 0;
    if (lights_on != g_last_lights_state) {
        g_last_lights_state = lights_on;
        uart_send_headlights_state(lights_on);
        uart_send_composite_vehicle_status();
    }

    if (f->dlc >= 6) {
        bool door_open = ((f->data[5] & 0xF0) == 0x10);
        if (door_open) {
            if (g_door_debounce < 3) g_door_debounce++;
        } else {
            g_door_debounce = 0;
        }
        bool debounced_door = (g_door_debounce >= 2);
        if (debounced_door != g_last_door_state) {
            g_last_door_state = debounced_door;
            uart_update_door_state(debounced_door);
            uart_send_composite_vehicle_status();
        }
    }
}

/* ========================================================================== */
/* Mode Tables                                                                */
/* ========================================================================== */

/* Mode 1: Toyota Prado 150 Primary Profile
 * Confirmed via live vehicle firmware dump (0x0800B9F4 dispatch table) */
static const CanDispatchEntry g_mode1_table[] = {
    { TOYOTA_PRADO_CAN_STEERING, handle_toyota_prado_steering },    /* 0x025 */
    { TOYOTA_PRADO_CAN_GEAR,     handle_toyota_prado_reverse },     /* 0x1D0 */
    { TOYOTA_PRADO_CAN_POWER,    handle_toyota_prado_radar_speed }, /* 0x396 */
    { TOYOTA_PRADO_CAN_BODY,     handle_toyota_prado_body },        /* 0x622 */
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
    g_last_lights_state = false;
    g_last_door_state = false;
    g_door_debounce = 0;
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
