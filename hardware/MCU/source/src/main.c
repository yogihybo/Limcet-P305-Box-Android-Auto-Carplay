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

static void gpio_hardware_init(void) {
    /* Enable GPIOA, GPIOB, GPIOC, AFIO clocks */
    RCC->APB2ENR |= (1UL << 2) | (1UL << 3) | (1UL << 4) | (1UL << 0);

    /* Disable JTAG to free PB3, PB4, PA15 for GPIOs while keeping SWD (PA13/PA14) active */
    AFIO->MAPR = (AFIO->MAPR & ~(7UL << 24)) | (2UL << 24); /* SWJ_CFG: JTAG-DP Disabled, SW-DP Enabled */

    /* PA1: "Audio Amp Mute" -- label UNCONFIRMED, not re-derived from real
     * disassembly this session (General purpose output push-pull, 2MHz ->
     * Mode 10, CNF 00 -> 0x02). Its originally-cited address (0x0800599C)
     * was already independently proven wrong for this exact claim (it's a
     * GPIOA-bit-8 READ, unrelated -- see MCU_FIRMWARE_VERIFIED_FINDINGS.md),
     * and CMD 0x84 (Audio Route) -- the confirmed real audio-routing command
     * -- turned out not to touch PA1 at all (see uart_protocol.c's
     * handle_audio_route(), which sends real "AT+AUDROUTE=1/2" over USART3
     * and drives GPIOC13/PC2 instead). Kept as a boot-time pop-prevention
     * measure regardless, since driving it HIGH-then-LOW during power-up
     * stabilization is a sensible precaution independent of its true real
     * function, but do not treat "Audio Amp Mute" as a confirmed label. */
    GPIOA->CRL &= ~(0x0FUL << 4);
    GPIOA->CRL |=  (0x02UL << 4);
    GPIOA->BSRR =  (1UL << 1);

    /* PB0: 4-Wire Resistive Touch Relay (General purpose output push-pull, 2MHz) */
    /* Start HIGH (Touch mapped to ArkMicro CarPlay/Android Auto SoC) */
    GPIOB->CRL &= ~(0x0FUL << 0);
    GPIOB->CRL |=  (0x02UL << 0);
    GPIOB->BSRR =  (1UL << 0);

    /* PB6: Microphone Select Multiplexer (General purpose output push-pull, 2MHz) */
    /* Start HIGH (OEM Roof Microphone routed to ArkMicro SoC) */
    GPIOB->CRL &= ~(0x0FUL << 24);
    GPIOB->CRL |=  (0x02UL << 24);
    GPIOB->BSRR =  (1UL << 6);

    /* PB1: CMD 0xA0 id=0x00 output (real target per MCU_FIRMWARE_VERIFIED_FINDINGS.md,
     * driven HIGH when settingId 0x00's struct offset 0x3b == 1). Function unconfirmed
     * beyond the pin identity itself -- default LOW until a sync_settings frame says otherwise. */
    GPIOB->CRL &= ~(0x0FUL << 4);
    GPIOB->CRL |=  (0x02UL << 4);
    GPIOB->BRR  =  (1UL << 1);

    /* PA15, PB8, PB9: CMD 0xA0 id=0x0b's coordinated 3-pin enable group (real finding --
     * all three fire HIGH together when struct offset 0x3d is cleared to 0; more consistent
     * with a subsystem power-up sequence than a single-purpose signal, but which subsystem
     * is unconfirmed without a schematic). PA15 is free for GPIO use here because JTAG was
     * already disabled above (SWJ_CFG), which frees PA15/PB3/PB4 from their JTDI/JTDO/NJTRST
     * alternate functions while leaving SWD (PA13/PA14) intact. Default LOW. */
    GPIOA->CRH &= ~(0x0FUL << 28);
    GPIOA->CRH |=  (0x02UL << 28);
    GPIOA->BRR  =  (1UL << 15);

    GPIOB->CRH &= ~(0x0FUL << 0);
    GPIOB->CRH |=  (0x02UL << 0);
    GPIOB->BRR  =  (1UL << 8);

    GPIOB->CRH &= ~(0x0FUL << 4);
    GPIOB->CRH |=  (0x02UL << 4);
    GPIOB->BRR  =  (1UL << 9);

    /* GPIOB Pin 14: ArkMicro ARK1668 SoC Hardware Reset line -- CORRECTED this
     * session. This clean-room source previously (wrongly) used PC13 for this,
     * inherited from a pasted handoff doc, never independently re-derived. Real
     * disassembly of can_app.bin (0x08005A18, port literal 0x40010C00 = GPIOB,
     * mask 0x4000 = pin 14) plus this project's own earlier LIVE HARDWARE
     * finding (connecting SWD halts the CPU before this pin's boot-time
     * release call runs, holding the whole SoC in reset -- see
     * docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md's "CRITICAL SAFETY FINDING")
     * both independently confirm GPIOB Pin 14 is the real reset-release pin,
     * not GPIOC Pin 13. */
    GPIOB->CRH &= ~(0x0FUL << 24);
    GPIOB->CRH |=  (0x02UL << 24);

    /* Hold ArkMicro SoC in hardware reset for 50ms */
    GPIOB->BRR = (1UL << 14);
    for (volatile uint32_t i = 0; i < 360000; i++) {
        __asm__ volatile("nop");
    }

    /* Release ArkMicro SoC from hardware reset */
    GPIOB->BSRR = (1UL << 14);

    /* Allow power rails & SoC PLL to stabilize (150ms), then unmute audio amplifier cleanly */
    for (volatile uint32_t i = 0; i < 1080000; i++) {
        __asm__ volatile("nop");
    }
    GPIOA->BRR = (1UL << 1); /* PA1 = LOW (Unmuted) */

    /* PC13: CMD 0xA0 id=0x11's real target (GPIOC Pin 13) -- NOT the SoC
     * reset line (see above). Real firmware asserts this LOW as part of the
     * same boot-time hardware-init sequence this function reimplements
     * (0x080056C0, confirmed this session), then releases it later via a
     * main-loop-polled condition -- also reachable from CMD 0xA0 id=0x11
     * (see docs/1.3.1_MCU_FIRMWARE_DECOMPILATION.md's "Camera Type / Video
     * Relay Multiplexer" claim and docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md's
     * discussion of it -- plausible given the structural match, but that
     * doc's adjacent id=0x0d "camera" claim is independently falsified, so
     * this is not re-confirmed, just no longer blocked by the reset
     * collision). Default LOW at boot, matching real firmware's own
     * boot-time default. */
    GPIOC->CRH &= ~(0x0FUL << 20);
    GPIOC->CRH |=  (0x02UL << 20);
    GPIOC->BRR  =  (1UL << 13);
}

int main(void) {
    /* Initialize System Clocks (72 MHz) */
    clock_init();

    /* Initialize Hardware GPIOs & Sequence ARK1668 Power-On Reset */
    gpio_hardware_init();

    /* Initialize Vehicle Profiles (Toyota Prado 150 CAN Matrix) */
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
