#include "stm32f105.h"
#include "can_driver.h"
#include "uart_protocol.h"
#include "vehicle_profiles.h"

static void clock_init(void) {
    /* Enable HSE (High-Speed External Oscillator, typically 8 MHz) */
    RCC->CR |= (1UL << 16); /* HSEON */
    while ((RCC->CR & (1UL << 17)) == 0) {} /* Wait for HSERDY */

    /* Configure PLL: HSE * 9 = 72 MHz (SYSCLK = 72MHz, APB1 = 36MHz, APB2 = 72MHz) */
    /* FLASH Latency = 2 wait states for 72MHz */
    *((volatile uint32_t *)0x40022000UL) = 0x02; /* FLASH_ACR: 2WS */

    /* HPRE = 0 (SYSCLK / 1), PPRE1 = 4 (HCLK / 2 = 36 MHz), PPRE2 = 0 (HCLK / 1 = 72 MHz) */
    RCC->CFGR = (0UL << 4) | (4UL << 8) | (0UL << 11) | (1UL << 16) | (7UL << 18); /* PLLSRC=HSE, PLLMUL=9 */

    /* Enable PLL */
    RCC->CR |= (1UL << 24); /* PLLON */
    while ((RCC->CR & (1UL << 25)) == 0) {} /* Wait for PLLRDY */

    /* Select PLL as System Clock */
    RCC->CFGR &= ~(3UL << 0);
    RCC->CFGR |=  (2UL << 0); /* SW = PLL */
    while ((RCC->CFGR & (3UL << 2)) != (2UL << 2)) {} /* Wait for SWS = PLL */
}

static void iwdg_init(void) {
    /* Start Independent Watchdog (IWDG) with ~2 second timeout */
    IWDG->KR = 0x5555; /* Enable register access */
    IWDG->PR = 0x06;   /* Prescaler = 256 -> 40kHz / 256 = 156.25 Hz */
    IWDG->RLR = 312;   /* 312 / 156.25 = ~2.0 seconds */
    IWDG->KR = 0xAAAA; /* Reload counter */
    IWDG->KR = 0xCCCC; /* Start IWDG */
}

static inline void iwdg_feed(void) {
    IWDG->KR = 0xAAAA;
}

int main(void) {
    /* Initialize System Clocks (72 MHz) */
    clock_init();

    /* Initialize Vehicle Profiles */
    vehicle_profiles_init();

    /* Initialize UART to SoC (/dev/ttyHS0 @ 38400 baud) */
    uart_protocol_init(38400);

    /* Initialize CAN Bus (500 kbit/s ISO 11898-2) */
    can_init(500000);

    /* Start Watchdog */
    iwdg_init();

    /* Main Event Loop */
    while (1) {
        /* Service Watchdog */
        iwdg_feed();

        /* Process Inbound UART Messages from SoC */
        uart_process_rx();

        /* Process Inbound CAN Messages & Dispatch Vehicle Events */
        can_dispatch_process();
    }

    return 0;
}
