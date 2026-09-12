#include "power_manager.h"
#include "stm32f105.h"
#include "gpio_driver.h"
#include "uart_protocol.h"
#include "can_driver.h"

/* Power management state struct (matches dump 0x200007E8) */
static power_state_t s_power_state = POWER_STATE_ACTIVE;
static uint16_t s_timer_counter = 0;
static volatile bool s_can_activity_flag = false;

void EXTI9_5_IRQHandler(void) {
    /* Check if EXTI Line 8 (PA8 ACC sense) triggered */
    if (EXTI->PR & (1UL << 8)) {
        EXTI->PR = (1UL << 8); /* Clear pending bit */
    }
}

void power_manager_init(void) {
    s_power_state = POWER_STATE_ACTIVE;
    s_timer_counter = 0;
    s_can_activity_flag = false;

    /* Ensure PWR peripheral clock is enabled */
    RCC->APB1ENR |= (1UL << 28);
}

void power_manager_notify_can_activity(void) {
    s_can_activity_flag = true;
}

power_state_t power_manager_get_state(void) {
    return s_power_state;
}

void power_manager_feed_watchdog(void) {
    IWDG->KR = 0xAAAA;
}

static void configure_exti_wakeup(bool enable) {
    if (enable) {
        /* Enable AFIO clock */
        RCC->APB2ENR |= (1UL << 0);

        /* Map EXTI Line 8 to GPIOA Pin 8 (ACC sense):
         * EXTICR[2] covers EXTI8..EXTI11.
         * EXTI8 is bits [3:0]. 0x0 = GPIOA */
        AFIO->EXTICR[2] &= ~(0x0FUL << 0);

        /* Enable Interrupt mask on line 8 */
        EXTI->IMR |= (1UL << 8);

        /* Trigger on Rising edge (ACC turned ON) */
        EXTI->RTSR |= (1UL << 8);
        EXTI->FTSR &= ~(1UL << 8);

        /* Clear any pending flag */
        EXTI->PR = (1UL << 8);

        /* Enable EXTI9_5_IRQn (IRQ 23) in NVIC */
        nvic_enable_irq(23);
    } else {
        /* Disable EXTI Line 8 */
        EXTI->IMR &= ~(1UL << 8);
        EXTI->RTSR &= ~(1UL << 8);
        EXTI->PR = (1UL << 8);
        nvic_disable_irq(23);
    }
}

static void enter_low_power_sleep(void) {
    /* 1. Flush any pending UART transmissions */
    for (volatile uint32_t i = 0; i < 72000; i++) {
        __asm__ volatile("nop");
    }

    /* 2. Configure wakeup sources */
    configure_exti_wakeup(true);

    /* 3. Enter sleep loop: wait for interrupt while servicing watchdog */
    while (1) {
        power_manager_feed_watchdog();

        /* If ACC is high, vehicle is on: wake up immediately */
        if (gpio_get_acc_status()) {
            break;
        }

        /* Sleep (WFI) until next interrupt (EXTI or SysTick) */
        __asm__ volatile("wfi");

        /* Check again if wake condition met */
        if (gpio_get_acc_status() || s_can_activity_flag) {
            break;
        }
    }

    /* 4. Wakeup recovery: teardown EXTI wakeup */
    configure_exti_wakeup(false);
}

/* Task 4: 100ms Periodic Executive (matches dump 0x08007C2C) */
void power_manager_task(void) {
    bool acc_active = gpio_get_acc_status();
    bool can_active = s_can_activity_flag;
    s_can_activity_flag = false; /* Clear activity pulse */

    switch (s_power_state) {
        case POWER_STATE_ACTIVE:
            if (!acc_active && !can_active) {
                /* ACC dropped and CAN is quiet -> start standby countdown */
                s_power_state = POWER_STATE_STANDBY_WAIT;
                s_timer_counter = 0;
            } else {
                s_timer_counter = 0;
            }
            break;

        case POWER_STATE_STANDBY_WAIT:
            if (acc_active || can_active) {
                /* Activity resumed -> return to Active */
                s_power_state = POWER_STATE_ACTIVE;
                s_timer_counter = 0;
            } else {
                s_timer_counter++;
                if (s_timer_counter >= POWER_STANDBY_TIMEOUT_TICKS) {
                    s_power_state = POWER_STATE_PRE_SLEEP;
                    s_timer_counter = 0;
                }
            }
            break;

        case POWER_STATE_PRE_SLEEP:
            /* Step 3 in dump: Notify SoC / Prepare sleep */
            s_power_state = POWER_STATE_SLEEP_PREP;
            s_timer_counter = 0;
            break;

        case POWER_STATE_SLEEP_PREP:
            /* Step 4 in dump: Peripheral clocks off & transition to sleep */
            s_power_state = POWER_STATE_SLEEP;
            s_timer_counter = 0;
            break;

        case POWER_STATE_SLEEP:
            /* Step 5 in dump: Enter sleep mode */
            enter_low_power_sleep();

            /* When sleep exits, restore Active state */
            s_power_state = POWER_STATE_ACTIVE;
            s_timer_counter = 0;
            break;

        default:
            s_power_state = POWER_STATE_ACTIVE;
            s_timer_counter = 0;
            break;
    }
}
