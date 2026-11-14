#include "bootloader.h"

/* Clock actually in use: HSI 8 MHz until clock_init() locks the PLL, or for
 * good if the crystal doesn't start. The factory derives its baud rate the
 * same way, from RCC_GetClocksFreq() inside USART_Init(). */
static uint32_t s_hclk_hz = 8000000UL;
static uint32_t s_pclk1_hz = 8000000UL;

/* SysTick as a free-running 24-bit down-counter at HCLK/8, polled (no
 * interrupt). millis() folds the elapsed ticks in on every call, so it only
 * has to be called at least once per wrap: 1.86 s at 72 MHz, 16.7 s on HSI. */
#define SYST_CSR  (*(volatile uint32_t *)0xE000E010UL)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014UL)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018UL)
#define SYST_MAX  0x00FFFFFFUL

static uint32_t s_tick_last;
static uint32_t s_tick_frac;
static uint32_t s_ms;

static void millis_init(void) {
    SYST_RVR = SYST_MAX;
    SYST_CVR = 0;
    SYST_CSR = 1UL;                 /* ENABLE, clock = HCLK/8, no interrupt */
    s_tick_last = SYST_CVR;
}

uint32_t millis(void) {
    uint32_t now = SYST_CVR;
    uint32_t ticks_per_ms = s_hclk_hz / 8000UL;
    s_tick_frac += (s_tick_last - now) & SYST_MAX;   /* counts down */
    s_tick_last = now;
    s_ms += s_tick_frac / ticks_per_ms;
    s_tick_frac %= ticks_per_ms;
    return s_ms;
}

void clock_init(void) {
    /* If SYSCLK is currently driven by PLL, switch back to HSI first */
    if ((RCC->CFGR & (3UL << 2)) == (2UL << 2)) {
        RCC->CR |= (1UL << 0);
        while ((RCC->CR & (1UL << 1)) == 0) {}
        RCC->CFGR &= ~(3UL << 0);
        while ((RCC->CFGR & (3UL << 2)) != (0UL << 2)) {}
    }

    /* Disable PLL and PLL2 before configuring RCC_CFGR2 */
    RCC->CR &= ~((1UL << 24) | (1UL << 26));
    while ((RCC->CR & ((1UL << 25) | (1UL << 27))) != 0) {}

    /* Enable HSE (25.000 MHz crystal on STM32F105 Connectivity Line board)
     * with bounded timeout (~0x1000 loops) matching factory SetSysClockTo72 */
    RCC->CR |= (1UL << 16);
    uint32_t hse_counter = 0x1000;
    while ((RCC->CR & (1UL << 17)) == 0 && --hse_counter) {}

    if (hse_counter == 0) {
        /* Crystal failed to start: stay on 8 MHz HSI safely rather than locking up */
        return;
    }

    /* FLASH Latency = 2 wait states + PRFTBE for 72MHz (RM0008 §3.2.3, matches OEM 0x08000274) */
    *((volatile uint32_t *)0x40022000UL) = 0x12;

    /* APB1 = HCLK / 2 = 36 MHz (PPRE1 = 4, matches OEM 0x080002A6) */
    RCC->CFGR |= (4UL << 8);

    /* Configure CFGR2 for 25 MHz HSE:
     * HSE (25 MHz) -> PREDIV2 (/5) = 5 MHz -> PLL2 (*8) = 40 MHz -> PREDIV1 (/5) = 8 MHz
     * Exact OEM register value disassembled at 0x080002AE: 0x00010644 */
    RCC->CFGR2 = (RCC->CFGR2 & 0xFFFEF000UL) | 0x00010644UL;

    /* Enable PLL2 and wait for lock */
    RCC->CR |= (1UL << 26);
    while ((RCC->CR & (1UL << 27)) == 0) {}

    /* Configure Main PLL:
     * PLLSRC = 1 (PREDIV1 output = 8 MHz)
     * PLLMUL = 7 (x9) -> 8 MHz * 9 = 72 MHz SYSCLK
     * Matches OEM 0x080002EC */
    RCC->CFGR = (RCC->CFGR & ~0x003F0000UL) | 0x001D0000UL;

    /* Enable Main PLL and wait for lock */
    RCC->CR |= (1UL << 24);
    while ((RCC->CR & (1UL << 25)) == 0) {}

    /* Switch SYSCLK to Main PLL */
    RCC->CFGR = (RCC->CFGR & ~3UL) | 2UL;
    while ((RCC->CFGR & (3UL << 2)) != (2UL << 2)) {}

    s_hclk_hz = 72000000UL;
    s_pclk1_hz = 36000000UL;
}

void update_gpio_rails_init(void) {
    /* Enable GPIOA, GPIOB, GPIOC, AFIO clocks
     * Matching factory bootloader 0x08001772 */
    RCC->APB2ENR |= (1UL << 0) | (1UL << 2) | (1UL << 3) | (1UL << 4);

    /* Disable JTAG to release PB4 for standard GPIO (keep SW-DP active) */
    AFIO->MAPR = (AFIO->MAPR & ~(7UL << 24)) | (2UL << 24);

    /* PC12, PC14 as Output Push-Pull 50MHz:
     * Mode: 0x03, CNF: 0x00 -> 0x03 per nibble
     * PC12 is bit 16-19 in CRH, PC14 is bit 24-27 in CRH */
    GPIOC->CRH &= ~((0xFUL << 16) | (0xFUL << 24));
    GPIOC->CRH |=  ((0x3UL << 16) | (0x3UL << 24));

    /* PB4 as Output Push-Pull 50MHz:
     * PB4 is bit 16-19 in CRL */
    GPIOB->CRL &= ~(0xFUL << 16);
    GPIOB->CRL |=  (0x3UL << 16);

    /* Factory bootloader rail configuration (0x08001772):
     * Drive PC14 HIGH, PB4 HIGH, PC12 LOW to keep ARK1680 SoC powered */
    GPIOC->BSRR = (1UL << 14) | (1UL << (12 + 16)); /* PC14=1, PC12=0 */
    GPIOB->BSRR = (1UL << 4);                       /* PB4=1 */
}

void iwdg_init(void) {
    /* Exact factory bootloader IWDG configuration (0x08001750):
     * Prescaler = /128 (PR = 5)
     * Reload = 0x270 (624) -> ~2.0 second timeout with 40 kHz LSI */
    IWDG->KR = 0x5555;
    IWDG->PR = 5;
    IWDG->RLR = 0x270;
    IWDG->KR = 0xAAAA;
    IWDG->KR = 0xCCCC;
}

void iwdg_feed(void) {
    IWDG->KR = 0xAAAA;
}

void uart_init(uint32_t baudrate) {
    RCC->APB1ENR |= (1UL << 17); /* USART2EN */
    RCC->APB2ENR |= (1UL << 2);  /* IOPAEN */

    /* PA2 TX (Alt Push-Pull 50MHz), PA3 RX (Input Pull-up) */
    GPIOA->CRL &= ~(0xFFUL << 8);
    GPIOA->CRL |=  (0x0BUL << 8) | (0x08UL << 12);
    GPIOA->ODR |=  (1UL << 3);

    /* BRR = PCLK1 / baud, rounded: 0x3AA at 36 MHz (38,388 baud), 0xD0 on
     * the 8 MHz HSI fallback (38,462 baud). */
    USART2->BRR = (s_pclk1_hz + (baudrate / 2)) / baudrate;

    USART2->CR1 = (1UL << 13) | (1UL << 3) | (1UL << 2); /* UE, TE, RE */
}

void uart_putc(uint8_t c) {
    while ((USART2->SR & (1UL << 7)) == 0) {
        iwdg_feed();
    }
    USART2->DR = c;
}

uint8_t uart_getc(void) {
    while ((USART2->SR & (1UL << 5)) == 0) {
        iwdg_feed();
    }
    return (uint8_t)(USART2->DR & 0xFF);
}

bool uart_getc_timeout(uint8_t *out_char, uint32_t timeout_ms) {
    uint32_t start = millis();
    do {
        uint32_t sr = USART2->SR;
        if (sr & (1UL << 5)) {
            /* RXNE: this DR read also clears any ORE/NE/FE flagged with it */
            *out_char = (uint8_t)(USART2->DR & 0xFF);
            return true;
        }
        if (sr & 0x0EUL) {
            (void)USART2->DR;       /* error without data: SR-then-DR read clears it */
        }
    } while ((millis() - start) < timeout_ms);
    return false;
}

bool is_app_valid(void) {
    uint32_t app_sp = *((volatile uint32_t *)APP_FLASH_BASE);

    /* Matches the real factory bootloader check exactly (0x08001844-0x08001858):
     * masked comparison of initial stack pointer: (SP & 0x2FFE0000) == 0x20000000 */
    if ((app_sp & 0x2FFE0000UL) == 0x20000000UL) {
        return true;
    }
    return false;
}

void jump_to_application(void) {
    uint32_t app_sp = *((volatile uint32_t *)APP_FLASH_BASE);
    uint32_t app_reset = *((volatile uint32_t *)(APP_FLASH_BASE + 4));

    /* Pre-jump delay loop (~65k iterations) matching factory OEM settling count */
    for (volatile uint32_t i = 0; i < 0xFF0E; i++) {
        __asm__ volatile("nop");
    }

    /* Disable interrupts before jump */
    __asm__ volatile("cpsid i");

    /* Write factory authentication handshake signature (0x20141003 @ 0x20004000) */
    *BOOTLOADER_HANDSHAKE_ADDR = BOOTLOADER_HANDSHAKE_VAL;

    /* Relocate Vector Table */
    SCB->VTOR = APP_FLASH_BASE;

    /* Set Main Stack Pointer */
    __asm__ volatile("msr msp, %0" : : "r"(app_sp) : );

    /* Re-enable interrupts and jump to application Reset_Handler */
    __asm__ volatile("cpsie i");
    void (*app_entry)(void) = (void (*)(void))app_reset;
    app_entry();

    while (1) {}
}

int main(void) {
    /* Reload IWDG in case it was active prior to soft reset */
    IWDG->KR = 0xAAAA;

    clock_init();
    millis_init();

    IWDG->KR = 0xAAAA;

    /* Check if update mode is requested:
     * 1) Application requested update via SRAM magic 0x5555AAAA at 0x20004004
     * 2) Previous update was interrupted: 0x5AA5 flag present at 0x08002FFE */
    bool force_bootloader = (*BOOTLOADER_MAGIC_ADDR == BOOTLOADER_MAGIC_VAL) ||
                            (*((volatile uint16_t *)UPDATE_FLAG_ADDR) == UPDATE_FLAG_VAL);

    /* If valid application exists and no update requested -> jump directly to app */
    if (!force_bootloader && is_app_valid()) {
        jump_to_application();
    }

    /* Otherwise, enter OEM IAP Update Mode:
     * 1) Assert power rails to keep SoC powered (PC14=1, PB4=1, PC12=0)
     * 2) Start IWDG watchdog with 2.0s timeout
     * 3) Initialize USART2 at 38400 8N1 */
    update_gpio_rails_init();
    iwdg_init();
    uart_init(38400);

    /* Process authentic 0x2E firmware update stream. It only returns when the
     * host has gone quiet for good (a completed update resets from inside). */
    mcu_process_firmware_update();

    /* Give up like the factory (0x08001B56 / 0x08001EA4): clear the request
     * and let the IWDG reset the MCU. The next boot runs the application, or
     * comes straight back here if the 0x5AA5 flag says an update was cut
     * short or the application is invalid -- never stranded in update mode
     * just because a request was made. */
    *BOOTLOADER_MAGIC_ADDR = 0;
    while (1) {
        /* no IWDG refresh */
    }

    return 0;
}
