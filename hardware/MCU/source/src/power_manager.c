#include "power_manager.h"
#include "stm32f105.h"
#include "gpio_driver.h"

/* Power management state struct (matches factory dump Task 4 at 0x08007C2C, struct 0x200007E8) */
static power_state_t s_power_state = POWER_STATE_INIT;
static uint16_t s_settle_timer_ms = 0;
static uint16_t s_acc_missing_timer_ms = 0;

void power_manager_init(void) {
    s_power_state = POWER_STATE_INIT;
    s_settle_timer_ms = 0;
    s_acc_missing_timer_ms = 0;

    /* Ensure PWR peripheral clock is enabled (RCC_APB1ENR PWREN) */
    RCC->APB1ENR |= (1UL << 28);
}

power_state_t power_manager_get_state(void) {
    return s_power_state;
}

void power_manager_feed_watchdog(void) {
    IWDG->KR = 0xAAAA;
}

/* Perform hardware power-down, clock-down to 2 MHz HSI, and enter parked polling loop */
static void enter_power_down_and_park(void) {
    /* 1. Disable USART1 and USART2 peripherals (UE = 0) */
    USART1->CR1 &= ~(1UL << 13);
    USART2->CR1 &= ~(1UL << 13);

    /* 2. Hold CAN in initialization mode while parked.
     * The factory leaves it running at the wrong clock rate on the car's bus.
     * Plain CAN sleep will not do because auto-wakeup is enabled on-chip.
     * Request initialization mode (INRQ = 1) and wait for hardware ACK (INAK = 1). */
    CAN1->MCR |= (1UL << 0);
    while ((CAN1->MSR & (1UL << 0)) == 0) {
        power_manager_feed_watchdog();
    }

    /* 3. Set power-down outputs matching factory 0x08006E08 & 0x08006B5A:
     * PC14, PB4, PC12, PB3, PC13, PC15, PB5 LOW; PB2 HIGH; PB14, PB15 HIGH */
    gpio_set_powerdown_outputs();

    /* 4. Clock-down sequence matching factory 0x08007B70:
     * - Enable HSI and wait for ready flag (0x080056D8 / 0x08005654)
     * - Switch SYSCLK to HSI and wait for confirmation (0x08005756 / 0x08005768)
     * - Disable Main PLL and PLL2 (0x080056F6 / 0x08005738)
     * - Disable HSE external crystal (0x0800560E)
     * - Flash latency = 0 wait states (0x08004864)
     * - AHB prescaler = /4 (0x90, HCLK = 2 MHz from 8 MHz internal RC; 0x08005772) */
    RCC->CR |= (1UL << 0); /* HSION */
    while ((RCC->CR & (1UL << 1)) == 0) {} /* Wait for HSIRDY */

    RCC->CFGR &= ~(3UL << 0); /* SW = HSI */
    while ((RCC->CFGR & (3UL << 2)) != (0UL << 2)) {} /* Wait for SWS = HSI */

    RCC->CR &= ~((1UL << 24) | (1UL << 26)); /* PLLON = 0, PLL2ON = 0 */
    RCC->CR &= ~(1UL << 16);                 /* HSEON = 0 */

    *((volatile uint32_t *)0x40022000UL) = 0x00; /* FLASH_ACR = 0WS */

    /* AHB prescaler = /4 (bits [7:4] = 1001b = 0x90) -> HCLK = 2 MHz */
    RCC->CFGR = (RCC->CFGR & ~(0xFUL << 4)) | (0x9UL << 4);

    /* 5. Parked busy loop matching factory 0x08007CCA:
     * Keeps watchdog refreshed and polls PD2 (active low).
     * No sleep / WFI instruction anywhere in factory binary. */
    while (!gpio_get_acc_status()) {
        power_manager_feed_watchdog();
    }

    /* 6. Wake: Full system reset instead of in-place reinitialization.
     * Ensures clean bring-up from scratch at the correct 72 MHz clock rate,
     * resetting all peripheral state and avoiding clock drift. */
    __asm__ volatile("dsb");
    SCB->AIRCR = (0x5FAUL << 16) | (1UL << 2); /* SYSRESETREQ */
    while (1) {
        /* Wait for reset */
    }
}

/* Task 4: 100ms Periodic Executive (matches factory dump 0x08007C2C) */
void power_manager_task(void) {
    /* Sample ACC line on PD2 (active low) */
    bool acc_on = gpio_get_acc_status();
    if (acc_on) {
        /* Raw PD2-low read resets the 5-second countdown timer (0x080088BA) */
        s_acc_missing_timer_ms = 0;
    } else {
        if (s_acc_missing_timer_ms < 0xFFFF - POWER_TASK_INTERVAL_MS) {
            s_acc_missing_timer_ms += POWER_TASK_INTERVAL_MS;
        } else {
            s_acc_missing_timer_ms = 0xFFFF;
        }
    }

    /* State machine matching factory 0x08007C2C */
    switch (s_power_state) {
        case POWER_STATE_INIT:
            /* State 0: Move to settle step 1 */
            s_power_state = POWER_STATE_SETTLE_50MS;
            s_settle_timer_ms = 0;
            break;

        case POWER_STATE_SETTLE_50MS:
            /* State 1: 50 ms settle step after boot/wake (0x08007C54) */
            s_settle_timer_ms += POWER_TASK_INTERVAL_MS;
            if (s_settle_timer_ms >= 50) {
                s_power_state = POWER_STATE_SETTLE_500MS;
                s_settle_timer_ms = 0;
            }
            break;

        case POWER_STATE_SETTLE_500MS:
            /* State 2: 500 ms settle step after boot/wake (0x08007C6E) */
            s_settle_timer_ms += POWER_TASK_INTERVAL_MS;
            if (s_settle_timer_ms >= 500) {
                s_power_state = POWER_STATE_ACTIVE;
                s_settle_timer_ms = 0;
            }
            break;

        case POWER_STATE_ACTIVE:
            /* State 3: Active run mode.
             * Check if ACC (PD2, active low) hasn't been seen for 5.0 s (0x08007C1C) */
            if (s_acc_missing_timer_ms >= POWER_ACC_TIMEOUT_MS) {
                s_power_state = POWER_STATE_PRE_PARK_100MS;
                s_settle_timer_ms = 0;
            }
            break;

        case POWER_STATE_PRE_PARK_100MS:
            /* State 4: Further 100 ms settle delay before shutdown (0x08007CA4) */
            s_settle_timer_ms += POWER_TASK_INTERVAL_MS;
            if (s_settle_timer_ms >= POWER_PRE_PARK_SETTLE_MS) {
                s_power_state = POWER_STATE_PARKED;
                enter_power_down_and_park();
            }
            break;

        case POWER_STATE_PARKED:
            /* State 5: Parked */
            enter_power_down_and_park();
            break;

        default:
            s_power_state = POWER_STATE_ACTIVE;
            s_settle_timer_ms = 0;
            break;
    }
}
