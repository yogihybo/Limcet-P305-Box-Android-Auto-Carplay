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

/* Toyota Prado 150 CAN IDs */
#define TOYOTA_PRADO_CAN_SWC    0x3C4   /* Steering Wheel Controls */
#define TOYOTA_PRADO_CAN_STATUS 0x025   /* Steering angle & status */
#define TOYOTA_PRADO_CAN_GEAR   0x127   /* Gear selector / Reverse */
#define TOYOTA_PRADO_CAN_SPEED  0x0B4   /* Wheel speeds */

/* Profiles API */
void vehicle_profiles_init(void);
const CanDispatchEntry* vehicle_get_dispatch_table(uint8_t mode, uint8_t *out_count);

#endif /* VEHICLE_PROFILES_H */
