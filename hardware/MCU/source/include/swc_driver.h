#ifndef SWC_DRIVER_H
#define SWC_DRIVER_H

#include "stm32f105.h"
#include <stdbool.h>

/* OEM KeyCodes disassembled at 0x0800B9E4 */
#define SWC_KEY_NONE        0x00
#define SWC_KEY_NEXT        0x03 /* Seek+ / Next track (0 ohm / GND on SW1) */
#define SWC_KEY_PREV        0x04 /* Seek- / Prev track (330 ohm on SW1) */
#define SWC_KEY_VOL_UP      0x09 /* Volume Up (330 ohm on SW2) */
#define SWC_KEY_VOL_DOWN    0x08 /* Volume Down (1k ohm on SW2) */
#define SWC_KEY_MODE        0x19 /* Mode / Source select (3.1k ohm on SW2) */

/* Button State definitions matching MCU_CMD_INPUT_EVENT protocol */
#define SWC_STATE_RELEASE   0x00
#define SWC_STATE_PRESS     0x01
#define SWC_STATE_HELD      0x02

/* Timing constants from disassembly:
 * Task period: 25 ms
 * Hold delay before repeat: 1200 ms (0x4B0)
 * Repeat tick interval: 100 ms (0x64)
 * Debounce count: 2 consecutive samples
 * Resting pullup threshold: >= 224 (0xE0, ~2.9V)
 */
#define SWC_POLL_INTERVAL_MS    25
#define SWC_HOLD_DELAY_MS       1200
#define SWC_REPEAT_INTERVAL_MS  100
#define SWC_DEBOUNCE_THRESHOLD  2
#define SWC_IDLE_ADC_THRESHOLD  224

void swc_init(void);
void swc_process(void);

/* Diagnostic accessor */
uint16_t swc_get_raw_adc(uint8_t channel);

#endif /* SWC_DRIVER_H */
