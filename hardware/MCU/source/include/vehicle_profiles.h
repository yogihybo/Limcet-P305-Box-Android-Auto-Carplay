#ifndef VEHICLE_PROFILES_H
#define VEHICLE_PROFILES_H

#include "can_driver.h"

/* Standard Steering Wheel Control Keycodes passed to SoC (Single-byte IDs) */
#define KEYCODE_PREV_TRACK      0x01   /* Seek- / Prev Track */
#define KEYCODE_NEXT_TRACK      0x02   /* Seek+ / Next Track */
#define KEYCODE_VOL_DOWN        0x03   /* Volume Down */
#define KEYCODE_VOL_UP          0x04   /* Volume Up */
#define KEYCODE_MODE_SOURCE     0x05   /* Mode / Source */
#define KEYCODE_MUTE            0x06   /* Audio Mute */
#define KEYCODE_VOICE_ASSIST    0x07   /* Voice / Siri / Google */
#define KEYCODE_PHONE_PICKUP    0x08   /* Phone Hook */
#define KEYCODE_ENTER_OK        0x09   /* Steering OK / Select */
#define KEYCODE_BACK            0x0A   /* Back */

/* Real internal SWC key codes, read directly from the real firmware's Mode 1
 * (0x0800A939) handler this session -- NOT our own KEYCODE_* enum above.
 * Each real call site sends a 16-bit value where the low byte is always
 * 0x01 (a "pressed" flag, matching our own key_code/pressed split) and the
 * high byte is the actual internal key identifier: 0x41, 0x40, 0x11, 0x12.
 * Which PHYSICAL button each corresponds to (volume/mode/etc.) is NOT
 * confirmed -- only that 4 distinct, real buttons exist with these exact
 * codes. Named generically rather than guessing a KEYCODE_VOL_UP-style
 * label that would overclaim semantic knowledge we don't have. */
#define SWC_RAW_CODE_0x41        0x41
#define SWC_RAW_CODE_0x40        0x40
#define SWC_RAW_CODE_0x11        0x11
#define SWC_RAW_CODE_0x12        0x12

/* Toyota Prado 150 CAN IDs -- Mode 1 ("Default/Primary Profile") values,
 * corrected this session. The prior 0x3C4/0x025/0x127/0x0B4 values were
 * NEVER disassembly-derived -- they don't appear in any of this project's
 * three real reference DCn32-VOLVO firmware binaries' CAN dispatch tables
 * (each independently re-extracted and cross-checked, see
 * docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md). The values below are the real,
 * disassembly-confirmed Mode 1 table entries (0x0800BB30 region in the
 * reference can_app.bin) -- Mode 2 (0x110/0x220/0x170) and Mode 3
 * (0x168/0x135/0x214) below in vehicle_profiles.c were already correct.
 * Still NOT independently confirmed against a real Prado's own OEM CAN
 * bus (this project has never captured one) -- these are the best
 * available evidence (the real firmware's own built-in "which car am I
 * in" profile set), not a Prado-specific guarantee. */
#define TOYOTA_PRADO_CAN_SWC    0x105   /* Steering Wheel Controls */
#define TOYOTA_PRADO_CAN_STATUS 0x28A   /* Steering angle & status */
#define TOYOTA_PRADO_CAN_GEAR   0x185   /* Gear selector / Reverse */
#define TOYOTA_PRADO_CAN_SPEED  0x0F5   /* Vehicle speed & engine RPM */

/* Profiles API */
void vehicle_profiles_init(void);
const CanDispatchEntry* vehicle_get_dispatch_table(uint8_t mode, uint8_t *out_count);

#endif /* VEHICLE_PROFILES_H */
