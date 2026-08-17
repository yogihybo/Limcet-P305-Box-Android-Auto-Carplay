#include "vehicle_profiles.h"
#include "uart_protocol.h"

/* State Tracking */
static uint16_t g_last_swc_buttons = 0;
static bool     g_last_reverse_state = false;

/* ========================================================================== */
/* Toyota Prado 150 CAN Handlers                                              */
/* ========================================================================== */

static void handle_toyota_prado_swc(const CanFrame *f) {
    if (f->dlc < 2) return;
    
    uint16_t buttons = ((uint16_t)f->data[1] << 8) | f->data[0];
    if (buttons == g_last_swc_buttons) {
        return;
    }
    
    /* Check individual bit transitions */
    /* Byte 0 Buttons */
    if ((buttons & 0x0080) && !(g_last_swc_buttons & 0x0080)) {
        uart_send_key_event(KEYCODE_VOL_UP, true);
    } else if (!(buttons & 0x0080) && (g_last_swc_buttons & 0x0080)) {
        uart_send_key_event(KEYCODE_VOL_UP, false);
    }
    
    if ((buttons & 0x0040) && !(g_last_swc_buttons & 0x0040)) {
        uart_send_key_event(KEYCODE_VOL_DOWN, true);
    } else if (!(buttons & 0x0040) && (g_last_swc_buttons & 0x0040)) {
        uart_send_key_event(KEYCODE_VOL_DOWN, false);
    }
    
    if ((buttons & 0x0004) && !(g_last_swc_buttons & 0x0004)) {
        uart_send_key_event(KEYCODE_MODE_SOURCE, true);
    } else if (!(buttons & 0x0004) && (g_last_swc_buttons & 0x0004)) {
        uart_send_key_event(KEYCODE_MODE_SOURCE, false);
    }
    
    if ((buttons & 0x0010) && !(g_last_swc_buttons & 0x0010)) {
        uart_send_key_event(KEYCODE_NEXT_TRACK, true);
    } else if (!(buttons & 0x0010) && (g_last_swc_buttons & 0x0010)) {
        uart_send_key_event(KEYCODE_NEXT_TRACK, false);
    }
    
    if ((buttons & 0x0008) && !(g_last_swc_buttons & 0x0008)) {
        uart_send_key_event(KEYCODE_PREV_TRACK, true);
    } else if (!(buttons & 0x0008) && (g_last_swc_buttons & 0x0008)) {
        uart_send_key_event(KEYCODE_PREV_TRACK, false);
    }
    
    /* Byte 1 Buttons */
    if ((buttons & 0x8000) && !(g_last_swc_buttons & 0x8000)) {
        uart_send_key_event(KEYCODE_PHONE_PICKUP, true);
    } else if (!(buttons & 0x8000) && (g_last_swc_buttons & 0x8000)) {
        uart_send_key_event(KEYCODE_PHONE_PICKUP, false);
    }
    
    if ((buttons & 0x0400) && !(g_last_swc_buttons & 0x0400)) {
        uart_send_key_event(KEYCODE_ENTER_OK, true);
    } else if (!(buttons & 0x0400) && (g_last_swc_buttons & 0x0400)) {
        uart_send_key_event(KEYCODE_ENTER_OK, false);
    }
    
    g_last_swc_buttons = buttons;
}

static void handle_toyota_prado_status(const CanFrame *f) {
    if (f->dlc < 4) return;
    
    /* Steering angle decode: 16-bit signed, 0.1 deg per LSB */
    int16_t angle = (int16_t)(((uint16_t)f->data[0] << 8) | f->data[1]);
    uart_send_steering_angle(angle);
}

static void handle_toyota_prado_reverse(const CanFrame *f) {
    if (f->dlc < 1) return;
    
    /* Bit 0 of Byte 0 indicates Reverse engaged */
    bool reverse = (f->data[0] & 0x01) != 0;
    if (reverse != g_last_reverse_state) {
        g_last_reverse_state = reverse;
        uart_send_reverse_state(reverse);
    }
}

/* Generic Fallback Handlers */
static void handle_generic_speed(const CanFrame *f) {
    (void)f;
}

static void handle_generic_door(const CanFrame *f) {
    (void)f;
}

static void handle_generic_hvac(const CanFrame *f) {
    (void)f;
}

/* ========================================================================== */
/* Mode Tables                                                                */
/* ========================================================================== */

/* Mode 1: Primary Profile (Configured for Toyota Prado 150) */
static const CanDispatchEntry g_mode1_table[] = {
    { TOYOTA_PRADO_CAN_SWC,    handle_toyota_prado_swc },     /* 0x3C4 */
    { TOYOTA_PRADO_CAN_STATUS, handle_toyota_prado_status },  /* 0x025 */
    { TOYOTA_PRADO_CAN_GEAR,   handle_toyota_prado_reverse }, /* 0x127 */
    { TOYOTA_PRADO_CAN_SPEED,  handle_generic_speed },        /* 0x0B4 */
    { 0x215,                   handle_generic_door },
    { 0x245,                   handle_generic_hvac }
};

/* Mode 2: Profile 2 */
static const CanDispatchEntry g_mode2_table[] = {
    { 0x110, handle_toyota_prado_swc },
    { 0x220, handle_toyota_prado_status },
    { 0x170, handle_toyota_prado_reverse }
};

/* Mode 3: Profile 3 */
static const CanDispatchEntry g_mode3_table[] = {
    { 0x168, handle_toyota_prado_swc },
    { 0x135, handle_toyota_prado_status },
    { 0x214, handle_toyota_prado_reverse }
};

void vehicle_profiles_init(void) {
    g_last_swc_buttons = 0;
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
