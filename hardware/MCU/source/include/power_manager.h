#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

/* ==============================================================================
 * MCU Power States (Reversed from factory dump Task 4 at 0x08007C2C, struct 0x200007E8)
 * ============================================================================== */
typedef enum {
    POWER_STATE_INIT            = 0,    /* State 0: System Initialization */
    POWER_STATE_SETTLE_50MS     = 1,    /* State 1: 50 ms settle step after boot/wake */
    POWER_STATE_SETTLE_500MS    = 2,    /* State 2: 500 ms settle step after boot/wake */
    POWER_STATE_ACTIVE          = 3,    /* State 3: Active Run Mode (monitoring ACC on PD2) */
    POWER_STATE_PRE_PARK_100MS  = 4,    /* State 4: ACC missing, 100 ms final settle */
    POWER_STATE_PARKED          = 5     /* State 5: Parked (outputs off, 2 MHz clock, polling PD2) */
} power_state_t;

/* State Machine Constants */
#define POWER_TASK_INTERVAL_MS      100     /* Task 4 execution period (100 ms) */
#define POWER_ACC_TIMEOUT_MS        5000    /* 5.0 seconds without ACC before shutdown (0x08007C1C) */
#define POWER_PRE_PARK_SETTLE_MS    100     /* 100 ms pre-park settle step (0x08007CA4) */

/* ==============================================================================
 * Function Prototypes
 * ============================================================================== */
void power_manager_init(void);
void power_manager_task(void);
power_state_t power_manager_get_state(void);
void power_manager_feed_watchdog(void);

#endif /* POWER_MANAGER_H */
