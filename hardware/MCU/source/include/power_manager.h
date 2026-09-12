#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

/* ==============================================================================
 * MCU Power States (Reversed from factory dump Task 4 at 0x08007C2C, struct 0x200007E8)
 * ============================================================================== */
typedef enum {
    POWER_STATE_INIT            = 0,    /* State 0: System Initialization */
    POWER_STATE_ACTIVE          = 1,    /* State 1: Active Run Mode (ACC high or CAN active) */
    POWER_STATE_STANDBY_WAIT    = 2,    /* State 2: Standby Countdown (ACC low, CAN idle) */
    POWER_STATE_PRE_SLEEP       = 3,    /* State 3: Flush Communications & Pre-sleep */
    POWER_STATE_SLEEP_PREP      = 4,    /* State 4: Peripheral Clock / Power Disable */
    POWER_STATE_SLEEP           = 5     /* State 5: Low-Power Sleep with EXTI Wakeup */
} power_state_t;

/* State Machine Constants */
#define POWER_TASK_INTERVAL_MS      100     /* Task 4 execution period (100 ms) */
#define POWER_STANDBY_TIMEOUT_TICKS 50      /* 50 * 100ms = 5.0 seconds (matches cmp r0, #0x32) */

/* ==============================================================================
 * Function Prototypes
 * ============================================================================== */
void power_manager_init(void);
void power_manager_task(void);
power_state_t power_manager_get_state(void);
void power_manager_notify_can_activity(void);
void power_manager_feed_watchdog(void);

#endif /* POWER_MANAGER_H */
