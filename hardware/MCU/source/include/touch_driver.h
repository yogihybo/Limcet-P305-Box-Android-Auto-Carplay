#ifndef TOUCH_DRIVER_H
#define TOUCH_DRIVER_H

#include "stm32f105.h"
#include <stdbool.h>

/* Screen resolution bounds matching OEM disassembly */
#define TOUCH_MAX_X             800
#define TOUCH_MAX_Y             480

/* Touch state values matching MCU_CMD_STATUS_QUERY (0x20) */
#define TOUCH_STATE_RELEASE     0x00
#define TOUCH_STATE_PRESS       0x01

/* Rotary Encoder Event Codes matching disassembly 0x0800ADA0 / 0x0800ADAE */
#define ROTARY_EVENT_CW         0x40
#define ROTARY_EVENT_CCW        0x41

/* Goodix GT911 I2C Address (disassembly: 0xBA write, 0xBB read) */
#define GOODIX_I2C_ADDR         0xBA

/* Timing intervals */
#define TOUCH_KNOB_INTERVAL_MS  5   /* Task 11 */
#define TOUCH_DIGI_INTERVAL_MS  25  /* Task 8 */

void touch_init(void);
void touch_process_knob(void);
void touch_process_digitizer(void);
void touch_set_relay(bool soc_mode);

/* Diagnostic accessors */
bool touch_get_coordinates(uint16_t *x, uint16_t *y, uint8_t *state);

#endif /* TOUCH_DRIVER_H */
