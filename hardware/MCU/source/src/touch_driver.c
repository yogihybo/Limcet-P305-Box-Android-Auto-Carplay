#include "touch_driver.h"
#include "uart_protocol.h"
#include "gpio_driver.h"

/* State storage for touch coordinates matching disassembly at 0x2000025A:
 * +10: X coordinate (uint16_t, 0..800)
 * +12: Y coordinate (uint16_t, 0..480)
 * +14: Touch state  (uint8_t, 0=release, 1=press)
 */

/* Rotary encoder quadrature state */
static uint8_t  g_rotary_last_code = 0;
static int8_t   g_rotary_accum = 0;

/* Timeout for I2C bus transactions (prevents lockup if hardware is absent) */
#define I2C_TIMEOUT_CYCLES  5000

static void i2c1_hardware_init(void);

/* Real-world bug this addresses: a GT911 (or any I2C slave) that glitches
 * mid-transaction can hold SDA low indefinitely -- a well-documented Goodix
 * failure mode. When that happens, the STM32 I2C1 peripheral's own state
 * machine is physically wedged waiting for a bus condition that will never
 * occur; writing STOP to CR1 (the previous error-path recovery here) is a
 * register write to the peripheral, not an electrical fix to the bus, so it
 * does nothing. Every subsequent transaction attempt just re-times-out
 * silently, forever -- matching the real reported symptom exactly: "touch
 * stops registering, only a soft reset of the whole head unit brings it
 * back" (a soft reset re-toggles the physical GPIO lines and re-runs
 * i2c1_hardware_init() from scratch, which is why it "fixes" it). After
 * enough consecutive failures, i2c1_bus_recovery() performs the standard
 * real I2C bus-recovery procedure instead of waiting for an external reset:
 * temporarily drive SCL as a plain GPIO and clock it up to 9 times (the
 * maximum bits a stuck slave could still be holding the bus for) while
 * watching SDA, force a manual STOP condition electrically, then fully
 * re-initialize the I2C1 peripheral (including its SWRST reset). */
#define I2C_FAIL_RECOVERY_THRESHOLD 3
static uint8_t s_i2c_consec_failures = 0;

static void i2c1_bus_recovery(void) {
    /* Release I2C1 from the peripheral so SCL/SDA can be driven as plain
     * GPIO for manual bus recovery. */
    I2C1->CR1 &= ~(1UL << 0); /* PE = 0 */

    /* PB6 (SCL): General purpose open-drain output, 2MHz (Mode=10, CNF=01 -> 0x06) */
    GPIOB->CRL &= ~(0x0FUL << 24);
    GPIOB->CRL |=  (0x06UL << 24);
    GPIOB->BSRR = (1UL << 6); /* Idle high */

    /* PB7 (SDA): Input, so we can see whether a stuck slave releases it */
    GPIOB->CRL &= ~(0x0FUL << 28);
    GPIOB->CRL |=  (0x08UL << 28); /* Input pull-up (CNF=10, MODE=00) */
    GPIOB->BSRR = (1UL << 7);

    /* Clock SCL up to 9 times -- enough bits for a slave stuck mid-byte to
     * finish clocking out whatever it's holding and release SDA, per the
     * standard I2C bus-recovery procedure (NXP UM10204 3.1.16). */
    for (int i = 0; i < 9; i++) {
        GPIOB->BRR = (1UL << 6);
        for (volatile int d = 0; d < 50; d++) {}
        GPIOB->BSRR = (1UL << 6);
        for (volatile int d = 0; d < 50; d++) {}
        if (GPIOB->IDR & (1UL << 7)) break; /* SDA released early */
    }

    /* Force a manual STOP condition: SDA low-to-high while SCL is high. */
    GPIOB->CRL &= ~(0x0FUL << 28);
    GPIOB->CRL |=  (0x06UL << 28); /* PB7 as open-drain output */
    GPIOB->BRR = (1UL << 7);
    for (volatile int d = 0; d < 50; d++) {}
    GPIOB->BSRR = (1UL << 6);
    for (volatile int d = 0; d < 50; d++) {}
    GPIOB->BSRR = (1UL << 7);
    for (volatile int d = 0; d < 50; d++) {}

    /* Full peripheral re-init, including the SWRST this driver already
     * correctly does at boot -- but never repeated after a real failure
     * until now. */
    i2c1_hardware_init();
}

static void i2c1_hardware_init(void) {
    /* 1. Enable GPIOB and I2C1 clocks */
    RCC->APB2ENR |= (1UL << 3);  /* GPIOB */
    RCC->APB1ENR |= (1UL << 21); /* I2C1 */

    /* 2. Configure PB6 (SCL) and PB7 (SDA) as Alternate Function Open-Drain 2MHz:
     * Mode = 10 (2MHz), CNF = 11 (Alt Func Open-Drain) -> 0x0E per nibble
     */
    GPIOB->CRL &= ~((0x0FUL << 24) | (0x0FUL << 28));
    GPIOB->CRL |=  ((0x0EUL << 24) | (0x0EUL << 28));

    /* 3. Reset and configure I2C1 */
    I2C1->CR1 |= (1UL << 15); /* SWRST */
    for (volatile int i = 0; i < 200; i++) {}
    I2C1->CR1 &= ~(1UL << 15);

    /* APB1 clock is 36 MHz */
    I2C1->CR2 = 36; /* FREQ = 36 MHz */

    /* 100 kHz standard mode:
     * Thigh = Tlow = 5000 ns -> CCR = 5000 ns / (1/36MHz) = 180 (0xB4)
     */
    I2C1->CCR = 180;

    /* TRISE = (1000 ns / (1/36MHz)) + 1 = 36 + 1 = 37 */
    I2C1->TRISE = 37;

    /* Enable peripheral and ACK */
    I2C1->CR1 = (1UL << 0) | (1UL << 10); /* PE | ACK */
}

void touch_init(void) {
    /* 1. Enable GPIOA, GPIOB, GPIOC clocks */
    RCC->APB2ENR |= (1UL << 2) | (1UL << 3) | (1UL << 4);

    /* 2. PB0 is a floating input in the factory firmware, not a touch-routing
     *    relay output (MCU_FIRMWARE_VERIFIED_FINDINGS.md section 42); it is left to gpio_apply_factory_run_config(). */

    /* 3. Configure Rotary Encoder pins as Input with Pull-Up:
     * - PA0: Rotary Pin A (CNF=10, MODE=00 -> 0x08, ODR bit 0 = 1)
     * - PC4: Rotary Pin B (CNF=10, MODE=00 -> 0x08, ODR bit 4 = 1)
     */
    GPIOA->CRL &= ~(0x0FUL << 0);
    GPIOA->CRL |=  (0x08UL << 0);
    GPIOA->ODR |=  (1UL << 0);

    GPIOC->CRL &= ~(0x0FUL << 16);
    GPIOC->CRL |=  (0x08UL << 16);
    GPIOC->ODR |=  (1UL << 4);

    /* Sample initial rotary pin state */
    uint8_t a = (GPIOA->IDR & (1UL << 0)) ? 1 : 0;
    uint8_t b = (GPIOC->IDR & (1UL << 4)) ? 1 : 0;
    g_rotary_last_code = (a << 1) | b;
    g_rotary_accum = 0;

    /* 4. Initialize I2C1 for Goodix touch controller */
    i2c1_hardware_init();
}

void touch_set_relay(bool soc_mode) {
    /* No-op: the "PB0 touch relay" came from the DCn32 (Volvo) image; on this
     * unit PB0 is a floating input (MCU_FIRMWARE_VERIFIED_FINDINGS.md section 42). */
    (void)soc_mode;
}

/* Quadrature Gray code transition table:
 * Prev: [1:0], Curr: [1:0] -> 4-bit index
 *  0: invalid / no change
 * +1: Clockwise step
 * -1: Counter-Clockwise step
 */
static const int8_t k_quad_lut[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

void touch_process_knob(void) {
    /* Sample quadrature encoder inputs (PA0, PC4) */
    uint8_t a = (GPIOA->IDR & (1UL << 0)) ? 1 : 0;
    uint8_t b = (GPIOC->IDR & (1UL << 4)) ? 1 : 0;
    uint8_t curr_code = (a << 1) | b;

    if (curr_code != g_rotary_last_code) {
        uint8_t idx = (g_rotary_last_code << 2) | curr_code;
        g_rotary_accum += k_quad_lut[idx];
        g_rotary_last_code = curr_code;

        /* 4 quadrature transitions per physical detent click */
        if (g_rotary_accum >= 4) {
            g_rotary_accum = 0;
            uart_send_key_event(ROTARY_EVENT_CW, 1);
        } else if (g_rotary_accum <= -4) {
            g_rotary_accum = 0;
            uart_send_key_event(ROTARY_EVENT_CCW, 1);
        }
    }
}

static bool i2c1_read_bytes(uint8_t dev_addr, uint16_t reg_addr, uint8_t reg_len, uint8_t *data, uint8_t len) {
    uint32_t timeout;

    /* Generate START */
    I2C1->CR1 |= (1UL << 8); /* START */
    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 0))) { /* SB */
        if (--timeout == 0) goto err;
    }

    /* Send device write address */
    I2C1->DR = (dev_addr & 0xFE);
    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 1))) { /* ADDR */
        if (--timeout == 0) goto err;
    }
    (void)I2C1->SR2; /* Clear ADDR */

    /* Register address: 1 byte (factory variants 1/3) or 2 bytes, MSB first
     * (variant 2, factory helper 0x08009B66) */
    if (reg_len == 2) {
        timeout = I2C_TIMEOUT_CYCLES;
        while (!(I2C1->SR1 & (1UL << 7))) { /* TXE */
            if (--timeout == 0) goto err;
        }
        I2C1->DR = (uint8_t)(reg_addr >> 8);
    }

    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 7))) { /* TXE */
        if (--timeout == 0) goto err;
    }
    I2C1->DR = (uint8_t)(reg_addr & 0xFF);

    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 7))) { /* TXE */
        if (--timeout == 0) goto err;
    }

    /* Repeated START for read */
    I2C1->CR1 |= (1UL << 8); /* START */
    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 0))) { /* SB */
        if (--timeout == 0) goto err;
    }

    /* Send device read address */
    I2C1->DR = (dev_addr | 0x01);
    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 1))) { /* ADDR */
        if (--timeout == 0) goto err;
    }

    if (len == 1) {
        I2C1->CR1 &= ~(1UL << 10); /* Disable ACK */
        (void)I2C1->SR2;
        I2C1->CR1 |= (1UL << 9);  /* STOP */

        timeout = I2C_TIMEOUT_CYCLES;
        while (!(I2C1->SR1 & (1UL << 6))) { /* RXNE */
            if (--timeout == 0) goto err;
        }
        data[0] = (uint8_t)I2C1->DR;
    } else {
        (void)I2C1->SR2;
        for (uint8_t i = 0; i < len; i++) {
            if (i == len - 1) {
                I2C1->CR1 &= ~(1UL << 10); /* Disable ACK */
                I2C1->CR1 |= (1UL << 9);  /* STOP */
            }
            timeout = I2C_TIMEOUT_CYCLES;
            while (!(I2C1->SR1 & (1UL << 6))) { /* RXNE */
                if (--timeout == 0) goto err;
            }
            data[i] = (uint8_t)I2C1->DR;
        }
    }
    I2C1->CR1 |= (1UL << 10); /* Re-enable ACK */
    return true;

err:
    I2C1->CR1 |= (1UL << 9);  /* Generate STOP */
    I2C1->CR1 |= (1UL << 10); /* Re-enable ACK */
    return false;
}


/* ------------------------------------------------------------------------
 * Touch, as the factory firmware does it (MCU_FIRMWARE_VERIFIED_FINDINGS.md
 * sections 30.1, 30.2 and 43). The controller variant is fixed per board by
 * the PC11/PA10 straps (0x08008794: variant = (PA10 << 1 | PC11) & 3):
 *   1, 3: I2C address 0xBA, 8-bit registers 0x08 (4 B), 0x20 (4 B), 0x2B (1 B)
 *   2:    I2C address 0x98, 16-bit register 0x2201 (8 B)
 *   0:    no touch controller
 * Polled only in "aftermarket" mode (CMD 0x82 payload 1), after the factory's
 * start-up delays, every 20 ms. Reported to the SoC as CMD 0x20
 * {X lo, X hi, Y lo, Y hi, state}, state 0 = up, 1 = down, 2 = held (repeated).
 * Deliberately different from the factory: a failed read is not decoded (the
 * factory decodes its stale buffer), and 3 failures in a row run a bus
 * recovery (the factory has none -- the touch lockup, section 30.5). */
extern uint32_t system_millis(void);

#define TOUCH_HOLDOFF_MS        6000U   /* 0x200001BC+0x14, loaded at power-up */
#define TOUCH_POLL_MS           20U     /* 0x0800A27C */

static uint8_t  s_task_state;           /* 0x2000025A+0x12 */
static uint32_t s_task_since;
static uint8_t  s_pen_state;            /* 0x2000025A+0xE */
static uint32_t s_pen_since;            /* 0x20000076 */
static uint16_t s_x, s_y;               /* 0x2000025A+0xA / +0xC */

static void pen_report(uint8_t state, uint32_t now) {
    s_pen_state = state;
    s_pen_since = now;
    uart_send_touch_coordinates(s_x, s_y, state);
}

static bool touch_read_failed(bool ok) {
    if (ok) {
        s_i2c_consec_failures = 0;
        return false;
    }
    if (s_i2c_consec_failures < 255) {
        s_i2c_consec_failures++;
    }
    if (s_i2c_consec_failures >= I2C_FAIL_RECOVERY_THRESHOLD) {
        i2c1_bus_recovery();
        s_i2c_consec_failures = 0;
    }
    return true;
}

/* 0x08009F14 + 0x08009DB4 */
static void touch_poll_ba(uint32_t now) {
    uint8_t r08[4], r20[4], r2b;
    (void)i2c1_read_bytes(0xBA, 0x08, 1, r08, 4);
    bool ok = i2c1_read_bytes(0xBA, 0x20, 1, r20, 4);
    (void)i2c1_read_bytes(0xBA, 0x2B, 1, &r2b, 1);
    if (touch_read_failed(ok)) {
        return;
    }

    uint16_t x = (uint16_t)((r20[0] << 2) | (r20[1] & 3));
    if (x != 0) {
        if (x < 0xF0)       x = (uint16_t)(x - 160);
        else if (x < 0x1E0) x = (uint16_t)(x - 120);
        else if (x < 0x2D0) x = (uint16_t)(x - 80);
        else                x = (uint16_t)(x - 60);
    }
    if (x >= TOUCH_MAX_X) x = TOUCH_MAX_X;     /* unsigned: values under 160 wrap and clamp */
    s_x = x;

    uint16_t y = (uint16_t)((r20[2] << 2) | (r20[3] & 3));
    if (y != 0) {
        y = (uint16_t)(y - 260);
        y = (y == 0) ? 0 : (y >= TOUCH_MAX_Y) ? 0 : (uint16_t)(TOUCH_MAX_Y - y);
    }
    s_y = y;

    if (r20[0] != 0) {
        if (s_pen_state == 0) {
            pen_report(1, now);
        } else if (s_pen_state == 1 && (now - s_pen_since) >= 200) {
            pen_report(2, now);
        }
        if (s_pen_state == 2 && (now - s_pen_since) >= 30) {
            pen_report(2, now);
        }
    } else if (s_pen_state != 0) {
        pen_report(0, now);
    }
}

/* 0x0800A188 + 0x08009F68 */
static void touch_poll_98(uint32_t now) {
    uint8_t d[8];
    if (touch_read_failed(i2c1_read_bytes(0x98, 0x2201, 2, d, 8))) {
        return;
    }
    if (d[0] == 2) {
        uint16_t x = (uint16_t)((d[2] * 25U) / 8U);
        s_x = (x >= TOUCH_MAX_X) ? TOUCH_MAX_X : x;
        uint16_t y = (uint16_t)((d[3] * 15U) / 8U);
        s_y = (y == 0) ? 0 : (y >= TOUCH_MAX_Y) ? 0 : (uint16_t)(TOUCH_MAX_Y - y);
        if (d[1] & 0x80) {
            if (s_pen_state == 0) {
                pen_report(1, now);
            }
        } else if (s_pen_state != 0) {
            pen_report(0, now);
        }
    }
    if (d[0] == 0xFF && (d[1] & 0x80)) {
        if (s_pen_state == 1 && (now - s_pen_since) >= 100) {
            pen_report(2, now);
        }
        if (s_pen_state == 2 && (now - s_pen_since) >= 30) {
            pen_report(2, now);
        }
    }
}

static void touch_poll(uint32_t now) {
    switch (gpio_get_composite_sense_mask() & 3) {
        case 1: case 3: touch_poll_ba(now); break;
        case 2:         touch_poll_98(now); break;
        default:        break;
    }
}

/* Touch task 0x0800A1BC; call at least every 20 ms. */
void touch_process_digitizer(void) {
    uint32_t now = system_millis();

    /* Factory states 1-3: 200 ms, one poll, 100 ms, 200 ms; then running. */
    switch (s_task_state) {
        case 0:
            s_task_since = now;
            s_task_state = 1;
            return;
        case 1:
            if ((now - s_task_since) >= 200) {
                touch_poll(now);
                s_task_since = now;
                s_task_state = 2;
            }
            return;
        case 2:
        case 3:
            if ((now - s_task_since) >= (s_task_state == 2 ? 100U : 200U)) {
                s_task_since = now;
                s_task_state++;
            }
            return;
        default:
            break;
    }

    /* Running: only in aftermarket mode, after the power-up hold-off. */
    if (uart_get_app_mode() != 1 || now < TOUCH_HOLDOFF_MS) {
        return;
    }
    if ((now - s_task_since) >= TOUCH_POLL_MS) {
        s_task_since = now;
        touch_poll(now);
    }
}

bool touch_get_coordinates(uint16_t *x, uint16_t *y, uint8_t *state) {
    if (x) *x = s_x;
    if (y) *y = s_y;
    if (state) *state = s_pen_state;
    return s_pen_state != 0;
}
