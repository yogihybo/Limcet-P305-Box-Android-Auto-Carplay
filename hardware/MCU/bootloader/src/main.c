#include "bootloader.h"

static void clock_init(void) {
    /* Enable HSE (8 MHz) */
    RCC->CR |= (1UL << 16);
    while ((RCC->CR & (1UL << 17)) == 0) {}

    /* FLASH Latency = 2 wait states for 72MHz */
    *((volatile uint32_t *)0x40022000UL) = 0x02;

    /* PLL = HSE * 9 = 72 MHz */
    RCC->CFGR = (0UL << 4) | (4UL << 8) | (0UL << 11) | (1UL << 16) | (7UL << 18);

    /* Enable PLL */
    RCC->CR |= (1UL << 24);
    while ((RCC->CR & (1UL << 25)) == 0) {}

    /* Switch to PLL */
    RCC->CFGR &= ~(3UL << 0);
    RCC->CFGR |=  (2UL << 0);
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
    uint32_t app_reset = *((volatile uint32_t *)(APP_FLASH_BASE + 4));

    /* Valid Stack Pointer (0x20000000..0x20010000) and Valid Code Pointer (0x08004000..0x0801FFFF) */
    if (app_sp >= 0x20000000UL && app_sp <= 0x20010000UL &&
        app_reset >= APP_FLASH_BASE && app_reset <= APP_FLASH_END &&
        (app_reset & 1) == 1) {
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
    clock_init();

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
