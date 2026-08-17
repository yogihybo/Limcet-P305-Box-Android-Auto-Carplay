#ifndef VEHICLE_PROFILES_H
#define VEHICLE_PROFILES_H

#include "can_driver.h"

/* Standard Steering Wheel Control Keycodes passed to SoC */
#define KEYCODE_VOL_UP          0x01
#define KEYCODE_VOL_DOWN        0x02
#define KEYCODE_NEXT_TRACK      0x03
#define KEYCODE_PREV_TRACK      0x04
#define KEYCODE_MODE_SOURCE     0x05
#define KEYCODE_PHONE_PICKUP    0x06
#define KEYCODE_PHONE_HANGUP    0x07
#define KEYCODE_VOICE_ASSIST    0x08
#define KEYCODE_ENTER_OK        0x09
#define KEYCODE_BACK            0x0A

/* Toyota Prado 150 CAN IDs */
#define TOYOTA_PRADO_CAN_SWC    0x3C4   /* Steering Wheel Controls */
#define TOYOTA_PRADO_CAN_STATUS 0x025   /* Steering angle & status */
#define TOYOTA_PRADO_CAN_GEAR   0x127   /* Gear selector / Reverse */
#define TOYOTA_PRADO_CAN_SPEED  0x0B4   /* Wheel speeds */

/* Profiles API */
void vehicle_profiles_init(void);
const CanDispatchEntry* vehicle_get_dispatch_table(uint8_t mode, uint8_t *out_count);

#endif /* VEHICLE_PROFILES_H */
