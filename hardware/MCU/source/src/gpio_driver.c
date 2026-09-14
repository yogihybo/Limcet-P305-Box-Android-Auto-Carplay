#include "gpio_driver.h"
#include "stm32f105.h"
#include "uart_protocol.h"

/* Debounced hardware sense state */
static uint8_t s_current_sense_mask = 0;
static uint8_t s_last_raw_mask = 0;
static uint8_t s_debounce_count = 0;

/* Helper to configure 4-bit GPIO pin mode in CRL/CRH */
static void configure_pin_mode(GPIO_TypeDef *gpio, uint8_t pin, uint8_t mode_cnf) {
    if (pin < 8) {
        uint32_t shift = pin * 4;
        gpio->CRL &= ~(0x0FUL << shift);
        gpio->CRL |= ((uint32_t)mode_cnf << shift);
    } else {
        uint32_t shift = (pin - 8) * 4;
        gpio->CRH &= ~(0x0FUL << shift);
        gpio->CRH |= ((uint32_t)mode_cnf << shift);
    }
}

void gpio_driver_init(void) {
    /* 1. Enable GPIOA, GPIOB, GPIOC, GPIOD, and AFIO peripheral clocks */
    RCC->APB2ENR |= (1UL << 2) | /* GPIOA */
                    (1UL << 3) | /* GPIOB */
                    (1UL << 4) | /* GPIOC */
                    (1UL << 5) | /* GPIOD */
                    (1UL << 0);  /* AFIO */

    /* 2. Remap SWJ: Disable JTAG to release PB3, PB4, PA15 for general I/O (matches OEM 0x08006CF2) */
    AFIO->MAPR = (AFIO->MAPR & ~(7UL << 24)) | (2UL << 24);

    /* 3. Configure Input Pins with Pull-Up / Pull-Down (Mode: 00, CNF: 10 -> 0x08) */
    /* PA8: ACC 12V Sense (Pull-down: ODR=0) */
    configure_pin_mode(GPIOA, GPIO_PIN_ACC_SENSE, 0x08);
    GPIOA->BRR = (1UL << GPIO_PIN_ACC_SENSE);

    /* PC9: Reverse 12V Camera Trigger (Pull-down: ODR=0) */
    configure_pin_mode(GPIOC, GPIO_PIN_REV_SENSE, 0x08);
    GPIOC->BRR = (1UL << GPIO_PIN_REV_SENSE);

    /* PC8: Video Sync Detect (Pull-up: ODR=1) */
    configure_pin_mode(GPIOC, GPIO_PIN_SYNC_SENSE, 0x08);
    GPIOC->BSRR = (1UL << GPIO_PIN_SYNC_SENSE);

    /* PC11: Video Source Detect 0 (Pull-up: ODR=1) */
    configure_pin_mode(GPIOC, GPIO_PIN_VID_DET0, 0x08);
    GPIOC->BSRR = (1UL << GPIO_PIN_VID_DET0);

    /* PA10: Video Source Detect 1 (Pull-up: ODR=1) */
    configure_pin_mode(GPIOA, GPIO_PIN_VID_DET1, 0x08);
    GPIOA->BSRR = (1UL << GPIO_PIN_VID_DET1);

    /* PD2: External Wakeup Sense (Pull-up: ODR=1) */
    configure_pin_mode(GPIOD, GPIO_PIN_WAKEUP_SENSE, 0x08);
    GPIOD->BSRR = (1UL << GPIO_PIN_WAKEUP_SENSE);

    /* PC0: Mic-mux sense input (Floating: CNF=01, MODE=00 -> 0x04). Real
     * firmware only reads this pin (see gpio_driver.h's GPIO_PIN_MIC_SENSE
     * comment) -- no confirmed pull direction from disassembly, so left
     * floating rather than guessing pull-up/down. */
    configure_pin_mode(GPIOC, GPIO_PIN_MIC_SENSE, 0x04);

    /* 4. Configure Output Pins (General Purpose Output Push-Pull 2MHz -> 0x02) */
    /* PB2: Video Relay Mux */
    configure_pin_mode(GPIOB, 2, 0x02);
    gpio_set_relay(RELAY_VIDEO_MUX, false);

    /* PC13: AV Power / Relay */
    configure_pin_mode(GPIOC, 13, 0x02);
    gpio_set_relay(RELAY_AV_POWER, false);

    /* PB3: Aux Control 1 */
    configure_pin_mode(GPIOB, 3, 0x02);
    gpio_set_relay(RELAY_AUX1, false);

    /* PC12: Aux Relay 2 */
    configure_pin_mode(GPIOC, 12, 0x02);
    gpio_set_relay(RELAY_AUX2, false);

    /* PB4: Power Hold */
    configure_pin_mode(GPIOB, 4, 0x02);
    gpio_set_relay(RELAY_POWER_HOLD, true); /* Assert power hold on startup */

    /* PC14: SoC Reset Control */
    configure_pin_mode(GPIOC, 14, 0x02);
    gpio_set_relay(RELAY_SOC_RESET_C14, false);

    /* PB14: ARK1668 Hardware Reset */
    configure_pin_mode(GPIOB, 14, 0x02);
    gpio_set_relay(RELAY_SOC_RESET_B14, true); /* Release SoC reset (active low) */
}

void gpio_set_relay(gpio_relay_id_t relay_id, bool active) {
    switch (relay_id) {
        case RELAY_VIDEO_MUX: /* PB2 */
            if (active) GPIOB->BSRR = (1UL << 2);
            else        GPIOB->BRR  = (1UL << 2);
            break;

        case RELAY_AV_POWER: /* PC13 */
            if (active) GPIOC->BSRR = (1UL << 13);
            else        GPIOC->BRR  = (1UL << 13);
            break;

        case RELAY_AUX1: /* PB3 */
            if (active) GPIOB->BSRR = (1UL << 3);
            else        GPIOB->BRR  = (1UL << 3);
            break;

        case RELAY_AUX2: /* PC12 */
            if (active) GPIOC->BSRR = (1UL << 12);
            else        GPIOC->BRR  = (1UL << 12);
            break;

        case RELAY_POWER_HOLD: /* PB4 */
            if (active) GPIOB->BSRR = (1UL << 4);
            else        GPIOB->BRR  = (1UL << 4);
            break;

        case RELAY_SOC_RESET_C14: /* PC14 */
            if (active) GPIOC->BSRR = (1UL << 14);
            else        GPIOC->BRR  = (1UL << 14);
            break;

        case RELAY_SOC_RESET_B14: /* PB14 */
            if (active) GPIOB->BSRR = (1UL << 14);
            else        GPIOB->BRR  = (1UL << 14);
            break;

        default:
            break;
    }
}

bool gpio_get_acc_status(void) {
    return (GPIOA->IDR & (1UL << GPIO_PIN_ACC_SENSE)) != 0;
}

bool gpio_get_reverse_status(void) {
    return (GPIOC->IDR & (1UL << GPIO_PIN_REV_SENSE)) != 0;
}

bool gpio_get_sync_status(void) {
    return (GPIOC->IDR & (1UL << GPIO_PIN_SYNC_SENSE)) != 0;
}

bool gpio_get_mic_sense(void) {
    return (GPIOC->IDR & (1UL << GPIO_PIN_MIC_SENSE)) != 0;
}

uint8_t gpio_get_composite_sense_mask(void) {
    return s_current_sense_mask;
}

/* 50ms periodic sampling (matches Task 5 at 0x0800700C and 0x08008794) */
void gpio_poll_senses(void) {
    uint8_t raw = 0;

    /* Bit 0: PC11 Video Source Detect 0 */
    if (GPIOC->IDR & (1UL << GPIO_PIN_VID_DET0)) {
        raw |= (1 << 0);
    }
    /* Bit 1: PA10 Video Source Detect 1 */
    if (GPIOA->IDR & (1UL << GPIO_PIN_VID_DET1)) {
        raw |= (1 << 1);
    }
    /* Bit 2: PA8 ACC 12V Sense */
    if (GPIOA->IDR & (1UL << GPIO_PIN_ACC_SENSE)) {
        raw |= (1 << 2);
    }
    /* Bit 3: PC9 Reverse Camera 12V Trigger */
    if (GPIOC->IDR & (1UL << GPIO_PIN_REV_SENSE)) {
        raw |= (1 << 3);
    }
    /* Bit 4: PC8 Video Sync Sense */
    if (GPIOC->IDR & (1UL << GPIO_PIN_SYNC_SENSE)) {
        raw |= (1 << 4);
    }

    /* Debounce: 2 consecutive matching samples (100ms stability) */
    if (raw == s_last_raw_mask) {
        if (s_debounce_count < 2) {
            s_debounce_count++;
            if (s_debounce_count == 2) {
                uint8_t changed = s_current_sense_mask ^ raw;
                s_current_sense_mask = raw;

                /* Handle Reverse Gear Trigger change (Bit 3) */
                if (changed & (1 << 3)) {
                    bool rev_active = (raw & (1 << 3)) != 0;
                    uart_send_reverse_state(rev_active);
                }
            }
        }
    } else {
        s_last_raw_mask = raw;
        s_debounce_count = 0;
    }
}
