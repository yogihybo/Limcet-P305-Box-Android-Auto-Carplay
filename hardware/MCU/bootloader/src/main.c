#include "bootloader.h"

static void clock_init(void) {
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

    /* Enable HSE (25.000 MHz crystal on STM32F105 Connectivity Line board) */
    RCC->CR |= (1UL << 16);
    while ((RCC->CR & (1UL << 17)) == 0) {}

    /* FLASH Latency = 2 wait states + PRFTBE for 72MHz (RM0008 §3.2.3, matches 0x08000274) */
    *((volatile uint32_t *)0x40022000UL) = 0x12;

    /* APB1 = HCLK / 2 = 36 MHz (PPRE1 = 4, matches 0x080002A6) */
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
}

void uart_init(uint32_t baudrate) {
    RCC->APB1ENR |= (1UL << 17); /* USART2EN */
    RCC->APB2ENR |= (1UL << 2);  /* IOPAEN */

    /* PA2 TX (Alt Push-Pull 50MHz), PA3 RX (Input Pull-up) */
    GPIOA->CRL &= ~(0xFFUL << 8);
    GPIOA->CRL |=  (0x0BUL << 8) | (0x08UL << 12);
    GPIOA->ODR |=  (1UL << 3);

    /* 36 MHz / 38400 = 0x3A98 */
    if (baudrate == 38400) {
        USART2->BRR = 0x03A98;
    } else {
        USART2->BRR = (36000000UL + (baudrate / 2)) / baudrate;
    }

    USART2->CR1 = (1UL << 13) | (1UL << 3) | (1UL << 2); /* UE, TE, RE */
}

void uart_putc(uint8_t c) {
    while ((USART2->SR & (1UL << 7)) == 0) {} /* TXE */
    USART2->DR = c;
}

uint8_t uart_getc(void) {
    while ((USART2->SR & (1UL << 5)) == 0) {} /* RXNE */
    return (uint8_t)(USART2->DR & 0xFF);
}

bool uart_getc_timeout(uint8_t *out_char, uint32_t timeout_ms) {
    /* ~72000 cycles per ms at 72MHz */
    uint32_t loops = timeout_ms * 9000;
    while (loops--) {
        if (USART2->SR & (1UL << 5)) {
            *out_char = (uint8_t)(USART2->DR & 0xFF);
            return true;
        }
    }
    return false;
}

bool is_app_valid(void) {
    uint32_t app_sp = *((volatile uint32_t *)APP_FLASH_BASE);

    /* Matches the real factory bootloader's own check exactly (disassembled
     * at 0x08001844-0x08001858) -- masked comparison of the stack pointer
     * only. The real bootloader does NOT separately validate the reset
     * vector's range or Thumb-bit before jumping. See
     * docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md sections 5.2 and 6. */
    if ((app_sp & 0x2FFE0000UL) == 0x20000000UL) {
        return true;
    }
    return false;
}

void jump_to_application(void) {
    uint32_t app_sp = *((volatile uint32_t *)APP_FLASH_BASE);
    uint32_t app_reset = *((volatile uint32_t *)(APP_FLASH_BASE + 4));

    /* Disable interrupts before jump */
    __asm__ volatile("cpsid i");

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

    IWDG->KR = 0xAAAA;

    /* Check if application requested Bootloader Update Mode */
    bool force_bootloader = (*BOOTLOADER_MAGIC_ADDR == BOOTLOADER_MAGIC_VAL);
    *BOOTLOADER_MAGIC_ADDR = 0; /* Clear magic flag */

    /* If valid application exists and no update requested -> Jump directly */
    if (!force_bootloader && is_app_valid()) {
        jump_to_application();
    }

    /* Otherwise, enter YMODEM IAP Flash Mode on USART2 */
    uart_init(38400);

    /* Receive and flash application image over YMODEM */
    if (ymodem_receive_and_flash()) {
        if (is_app_valid()) {
            jump_to_application();
        }
    }

    /* System Reset on failure / timeout */
    SCB->AIRCR = (0x5FAUL << 16) | (1UL << 2);
    while (1) {}
    return 0;
}
