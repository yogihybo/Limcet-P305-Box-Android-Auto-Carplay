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
    /* PA8: configuration sense input (Pull-down: ODR=0) */
    configure_pin_mode(GPIOA, GPIO_PIN_CFG_SENSE0, 0x08);
    GPIOA->BRR = (1UL << GPIO_PIN_CFG_SENSE0);

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

    /* PD2: ACC sense, active low (Pull-up: ODR=1, so a floating pin reads "off") */
    configure_pin_mode(GPIOD, GPIO_PIN_ACC_SENSE, 0x08);
    GPIOD->BSRR = (1UL << GPIO_PIN_ACC_SENSE);

    /* PC0: Mic-mux sense input (Floating: CNF=01, MODE=00 -> 0x04). Real
     * firmware only reads this pin (see gpio_driver.h's GPIO_PIN_MIC_SENSE
     * comment) -- no confirmed pull direction from disassembly, so left
     * floating rather than guessing pull-up/down. */
    configure_pin_mode(GPIOC, GPIO_PIN_MIC_SENSE, 0x04);

    /* 4. Configure Output Pins (General Purpose Output Push-Pull 2MHz -> 0x02) */
    /* PB2: Video Relay Mux (0x08006C3A) */
    configure_pin_mode(GPIOB, 2, 0x02);

    /* PC13: AV Power / Relay (0x08006C52) */
    configure_pin_mode(GPIOC, 13, 0x02);

    /* PB3: Aux Control 1 (0x08006C70) */
    configure_pin_mode(GPIOB, 3, 0x02);

    /* PC12: Aux Relay 2 (0x08006C8A) */
    configure_pin_mode(GPIOC, 12, 0x02);

    /* PB4: Power Hold (0x08006CA8) */
    configure_pin_mode(GPIOB, 4, 0x02);

    /* PC14: SoC Reset Control (0x08006CC2) */
    configure_pin_mode(GPIOC, 14, 0x02);

    /* PB14: ARK1668 Hardware Reset (0x08005A18) */
    configure_pin_mode(GPIOB, 14, 0x02);

    /* PB5: Subsystem Output (0x08006C00) */
    configure_pin_mode(GPIOB, 5, 0x02);

    /* PC15: Task 5 / Rail Output (0x08006C1A) */
    configure_pin_mode(GPIOC, 15, 0x02);

    /* PB15: Power-down State Pin (0x08006E86) */
    configure_pin_mode(GPIOB, 15, 0x02);

    /* Set default run-mode output state matching factory wake (0x08006CE0) */
    gpio_set_run_mode_outputs();
}

void gpio_set_run_mode_outputs(void) {
    /* Outputs at wake (0x08006CE0): PC14, PB4, PC12, PB3, PB2, PB5 high; PC13, PC15 low */
    GPIOC->BSRR = (1UL << 14) | (1UL << 12);                /* PC14=1, PC12=1 */
    GPIOB->BSRR = (1UL << 4) | (1UL << 3) | (1UL << 2) | (1UL << 5); /* PB4=1, PB3=1, PB2=1, PB5=1 */
    GPIOC->BRR  = (1UL << 13) | (1UL << 15);               /* PC13=0, PC15=0 */
    /* PB14/PB15 LOW = SoC out of reset (factory sequencer 0x08009164; section 41).
     * main.c's gpio_hardware_init() has already done the 50 ms reset pulse. */
    GPIOB->BRR  = (1UL << 14) | (1UL << 15);
}

/* Every pin to the factory firmware's steady running configuration, read live
 * on the bench (MCU_FIRMWARE_VERIFIED_FINDINGS.md section 42): modes, pulls and
 * output levels on ports A-D. Called once, after all drivers have initialised.
 * Kept as the clean-room app has them:
 *   - PB6/PB7: the touch driver's I2C1 (it bit-bangs and re-moods them);
 * Deliberately not the factory's, because the clean-room app does not run the
 * peripheral behind them and an unclocked alternate-function pin can sit low:
 *   - PB10/PB11 (factory I2C2) and PA9 (factory USART1 TX): floating inputs,
 *     so those lines are left to their external pull-ups. */
void gpio_apply_factory_run_config(void) {
    uint32_t keep_crl_b = GPIOB->CRL & 0xFF000000UL;      /* PB6/PB7 modes */
    uint32_t keep_odr_b = GPIOB->ODR & 0x000000C0UL;      /* PB6/PB7 levels */

    GPIOA->ODR = 0x00002802UL;
    GPIOB->ODR = (0x000010FEUL & ~0x000000C0UL) | keep_odr_b;
    GPIOC->ODR = 0x00005000UL;
    GPIOD->ODR = 0x00000004UL;                             /* PD2 (ACC) pull-up */

    GPIOA->CRL = 0x88444B88UL;
    GPIOA->CRH = 0x388B8444UL;                             /* factory 0x388B84B4, PA9 -> input */
    GPIOB->CRL = (0xFF333384UL & 0x00FFFFFFUL) | keep_crl_b;
    GPIOB->CRH = 0x33B84443UL;                             /* factory 0x33B8FF43, PB10/PB11 -> input */
    GPIOC->CRL = 0x44484444UL;
    GPIOC->CRH = 0x33334344UL;
    GPIOD->CRL = 0x44444844UL;
    GPIOD->CRH = 0x44444444UL;
}

void gpio_set_powerdown_outputs(void) {
    /* Outputs at power-down (0x08006E08): PC14, PB4, PC12, PB3, PC13, PC15, PB5 low; PB2 high;
     * PB14 and PB15 go high (0x08006B5A) */
    GPIOC->BRR  = (1UL << 14) | (1UL << 12) | (1UL << 13) | (1UL << 15); /* PC14=0, PC12=0, PC13=0, PC15=0 */
    GPIOB->BRR  = (1UL << 4) | (1UL << 3) | (1UL << 5);                  /* PB4=0, PB3=0, PB5=0 */
    GPIOB->BSRR = (1UL << 2) | (1UL << 14) | (1UL << 15);                /* PB2=1, PB14=1, PB15=1 */

    /* Then every other pin: the factory's complete parked configuration, read
     * live from the factory firmware on the bench (MCU_FIRMWARE_VERIFIED_FINDINGS.md
     * section 41). Beyond the table above this releases I2C1 (PB6/PB7 to floating
     * inputs), leaves PB0 floating and PB1 pulled up as inputs, and sets the
     * factory's pulls on PA/PC -- so nothing is left driving or pulling against
     * the vehicle while parked. ODR (levels / pull direction) first, then modes,
     * so no pin passes through a wrong level. Wake is a system reset, which
     * rebuilds the running configuration from scratch. */
    GPIOA->ODR = 0x00002802UL;  GPIOB->ODR = 0x0000D006UL;
    GPIOC->ODR = 0x00000000UL;  GPIOD->ODR = 0x00000004UL;  /* PD2 (ACC) keeps its pull-up */
    GPIOA->CRL = 0x88444B88UL;  GPIOA->CRH = 0x388B84B4UL;
    GPIOB->CRL = 0x44333384UL;  GPIOB->CRH = 0x33B8FF43UL;
    GPIOC->CRL = 0x44484444UL;  GPIOC->CRH = 0x33334344UL;
    GPIOD->CRL = 0x44444844UL;  GPIOD->CRH = 0x44444444UL;
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

        case RELAY_OUT_PB5: /* PB5 */
            if (active) GPIOB->BSRR = (1UL << 5);
            else        GPIOB->BRR  = (1UL << 5);
            break;

        case RELAY_OUT_PC15: /* PC15 */
            if (active) GPIOC->BSRR = (1UL << 15);
            else        GPIOC->BRR  = (1UL << 15);
            break;

        case RELAY_OUT_PB15: /* PB15 */
            if (active) GPIOB->BSRR = (1UL << 15);
            else        GPIOB->BRR  = (1UL << 15);
            break;

        default:
            break;
    }
}

bool gpio_get_acc_status(void) {
#ifdef BENCH_FORCE_ACC_ON
    /* Bench build (make BENCH=1): nothing drives PD2 on a bench board, and its
     * pull-up reads as "ignition off", which would park the MCU. */
    return true;
#else
    /* PD2 low = ACC on, as the factory reads it (0x08007CD2, 0x080088B0). */
    return (GPIOD->IDR & (1UL << GPIO_PIN_ACC_SENSE)) == 0;
#endif
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
    /* Bit 2: PA8 configuration sense */
    if (GPIOA->IDR & (1UL << GPIO_PIN_CFG_SENSE0)) {
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
