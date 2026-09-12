#include "touch_driver.h"
#include "uart_protocol.h"

/* State storage for touch coordinates matching disassembly at 0x2000025A:
 * +10: X coordinate (uint16_t, 0..800)
 * +12: Y coordinate (uint16_t, 0..480)
 * +14: Touch state  (uint8_t, 0=release, 1=press)
 */
static uint16_t g_touch_x = 0;
static uint16_t g_touch_y = 0;
static uint8_t  g_touch_state = TOUCH_STATE_RELEASE;
static uint8_t  g_touch_last_state = TOUCH_STATE_RELEASE;

/* Rotary encoder quadrature state */
static uint8_t  g_rotary_last_code = 0;
static int8_t   g_rotary_accum = 0;

/* Timeout for I2C bus transactions (prevents lockup if hardware is absent) */
#define I2C_TIMEOUT_CYCLES  5000

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

    /* 2. Configure PB0: TOUCH_SEL relay output (General purpose output push-pull, 2MHz) */
    GPIOB->CRL &= ~(0x0FUL << 0);
    GPIOB->CRL |=  (0x02UL << 0);
    touch_set_relay(true); /* Default to ArkMicro SoC mode */

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
    if (soc_mode) {
        GPIOB->BSRR = (1UL << 0); /* PB0 HIGH -> Route touch to SoC */
    } else {
        GPIOB->BRR  = (1UL << 0); /* PB0 LOW  -> Bypass touch to Factory OEM */
    }
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

static bool i2c1_read_bytes(uint8_t dev_addr, uint16_t reg_addr, uint8_t *data, uint8_t len) {
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

    /* Send 2-byte register address */
    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 7))) { /* TXE */
        if (--timeout == 0) goto err;
    }
    I2C1->DR = (uint8_t)(reg_addr >> 8);

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

static bool i2c1_write_byte(uint8_t dev_addr, uint16_t reg_addr, uint8_t val) {
    uint32_t timeout;

    I2C1->CR1 |= (1UL << 8); /* START */
    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 0))) {
        if (--timeout == 0) goto err;
    }

    I2C1->DR = (dev_addr & 0xFE);
    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 1))) {
        if (--timeout == 0) goto err;
    }
    (void)I2C1->SR2;

    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 7))) {
        if (--timeout == 0) goto err;
    }
    I2C1->DR = (uint8_t)(reg_addr >> 8);

    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 7))) {
        if (--timeout == 0) goto err;
    }
    I2C1->DR = (uint8_t)(reg_addr & 0xFF);

    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 7))) {
        if (--timeout == 0) goto err;
    }
    I2C1->DR = val;

    timeout = I2C_TIMEOUT_CYCLES;
    while (!(I2C1->SR1 & (1UL << 2))) { /* BTF */
        if (--timeout == 0) goto err;
    }
    I2C1->CR1 |= (1UL << 9); /* STOP */
    return true;

err:
    I2C1->CR1 |= (1UL << 9);
    return false;
}

void touch_process_digitizer(void) {
    uint8_t status = 0;

    /* Read Goodix GT911 touch point status at register 0x814E */
    if (!i2c1_read_bytes(GOODIX_I2C_ADDR, 0x814E, &status, 1)) {
        return; /* No device or I2C bus error */
    }

    /* Bit 7: Buffer status (1 = coordinates ready), Bits [3:0]: point count */
    if ((status & 0x80) != 0) {
        uint8_t point_count = status & 0x0F;

        if (point_count > 0) {
            uint8_t raw_point[4];
            /* Read Point 1: X_low (0x8150), X_high (0x8151), Y_low (0x8152), Y_high (0x8153) */
            if (i2c1_read_bytes(GOODIX_I2C_ADDR, 0x8150, raw_point, 4)) {
                uint16_t raw_x = (uint16_t)raw_point[0] | ((uint16_t)raw_point[1] << 8);
                uint16_t raw_y = (uint16_t)raw_point[2] | ((uint16_t)raw_point[3] << 8);

                /* Normalization math from OEM disassembly 0x08009F80 - 0x0800A000:
                 * X = (25 * raw_x) / 8 -> 3.125 * raw_x
                 * Y = (15 * raw_y) / 8 -> 1.875 * raw_y
                 * Inverted Y for screen: Y = 480 - Y
                 */
                uint32_t calc_x = ((uint32_t)raw_x * 25UL) >> 3;
                if (calc_x > TOUCH_MAX_X) calc_x = TOUCH_MAX_X;
                g_touch_x = (uint16_t)calc_x;

                uint32_t calc_y = ((uint32_t)raw_y * 15UL) >> 3;
                if (calc_y > TOUCH_MAX_Y) calc_y = TOUCH_MAX_Y;
                g_touch_y = (uint16_t)(TOUCH_MAX_Y - calc_y);

                g_touch_state = TOUCH_STATE_PRESS;

                /* Emit touch coordinate packet MCU_CMD_STATUS_QUERY (0x20) */
                uart_send_touch_coordinates(g_touch_x, g_touch_y, g_touch_state);
            }
        } else {
            /* Touch released */
            if (g_touch_last_state == TOUCH_STATE_PRESS) {
                g_touch_state = TOUCH_STATE_RELEASE;
                uart_send_touch_coordinates(g_touch_x, g_touch_y, g_touch_state);
            }
        }

        /* Clear buffer ready bit in Goodix GT911 by writing 0 to 0x814E */
        i2c1_write_byte(GOODIX_I2C_ADDR, 0x814E, 0x00);
    } else {
        /* No active touch */
        if (g_touch_last_state == TOUCH_STATE_PRESS) {
            g_touch_state = TOUCH_STATE_RELEASE;
            uart_send_touch_coordinates(g_touch_x, g_touch_y, g_touch_state);
        }
    }

    g_touch_last_state = g_touch_state;
}

bool touch_get_coordinates(uint16_t *x, uint16_t *y, uint8_t *state) {
    if (x) *x = g_touch_x;
    if (y) *y = g_touch_y;
    if (state) *state = g_touch_state;
    return (g_touch_state == TOUCH_STATE_PRESS);
}
