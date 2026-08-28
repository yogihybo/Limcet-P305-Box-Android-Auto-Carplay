#include <stdint.h>
#include "stm32f105.h"

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

extern int main(void);
extern void CAN1_RX0_IRQHandler(void);
extern void USART2_IRQHandler(void);
extern void USART3_IRQHandler(void);

void Reset_Handler(void);
void Default_Handler(void);

/* Weak aliases for core exception handlers */
void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)    __attribute__((weak, alias("Default_Handler")));

/* Vector Table */
__attribute__((section(".isr_vector"), used))
void (* const g_pfnVectors[])(void) = {
    (void (*)(void))(&_estack),             /* 0x00: Initial Stack Pointer */
    Reset_Handler,                          /* 0x04: Reset Handler */
    NMI_Handler,                            /* 0x08: NMI */
    HardFault_Handler,                      /* 0x0C: HardFault */
    MemManage_Handler,                      /* 0x10: MemManage */
    BusFault_Handler,                       /* 0x14: BusFault */
    UsageFault_Handler,                     /* 0x18: UsageFault */
    0, 0, 0, 0,                             /* 0x1C-0x28: Reserved */
    SVC_Handler,                            /* 0x2C: SVCall */
    DebugMon_Handler,                       /* 0x30: Debug Monitor */
    0,                                      /* 0x34: Reserved */
    PendSV_Handler,                         /* 0x38: PendSV */
    SysTick_Handler,                        /* 0x3C: SysTick */
    
    /* External Interrupts (STM32F105) */
    Default_Handler,                        /* IRQ 0: WWDG */
    Default_Handler,                        /* IRQ 1: PVD */
    Default_Handler,                        /* IRQ 2: TAMPER */
    Default_Handler,                        /* IRQ 3: RTC */
    Default_Handler,                        /* IRQ 4: FLASH */
    Default_Handler,                        /* IRQ 5: RCC */
    Default_Handler,                        /* IRQ 6: EXTI0 */
    Default_Handler,                        /* IRQ 7: EXTI1 */
    Default_Handler,                        /* IRQ 8: EXTI2 */
    Default_Handler,                        /* IRQ 9: EXTI3 */
    Default_Handler,                        /* IRQ 10: EXTI4 */
    Default_Handler,                        /* IRQ 11: DMA1_C1 */
    Default_Handler,                        /* IRQ 12: DMA1_C2 */
    Default_Handler,                        /* IRQ 13: DMA1_C3 */
    Default_Handler,                        /* IRQ 14: DMA1_C4 */
    Default_Handler,                        /* IRQ 15: DMA1_C5 */
    Default_Handler,                        /* IRQ 16: DMA1_C6 */
    Default_Handler,                        /* IRQ 17: DMA1_C7 */
    Default_Handler,                        /* IRQ 18: ADC1_2 */
    Default_Handler,                        /* IRQ 19: CAN1_TX */
    CAN1_RX0_IRQHandler,                    /* IRQ 20: CAN1_RX0 */
    Default_Handler,                        /* IRQ 21: CAN1_RX1 */
    Default_Handler,                        /* IRQ 22: CAN1_SCE */
    Default_Handler,                        /* IRQ 23: EXTI9_5 */
    Default_Handler,                        /* IRQ 24: TIM1_BRK */
    Default_Handler,                        /* IRQ 25: TIM1_UP */
    Default_Handler,                        /* IRQ 26: TIM1_TRG_COM */
    Default_Handler,                        /* IRQ 27: TIM1_CC */
    Default_Handler,                        /* IRQ 28: TIM2 */
    Default_Handler,                        /* IRQ 29: TIM3 */
    Default_Handler,                        /* IRQ 30: TIM4 */
    Default_Handler,                        /* IRQ 31: I2C1_EV */
    Default_Handler,                        /* IRQ 32: I2C1_ER */
    Default_Handler,                        /* IRQ 33: I2C2_EV */
    Default_Handler,                        /* IRQ 34: I2C2_ER */
    Default_Handler,                        /* IRQ 35: SPI1 */
    Default_Handler,                        /* IRQ 36: SPI2 */
    Default_Handler,                        /* IRQ 37: USART1 */
    USART2_IRQHandler,                      /* IRQ 38: USART2 (SoC Link) */
    USART3_IRQHandler,                      /* IRQ 39: USART3 (Bluetooth AT relay) */
    Default_Handler,                        /* IRQ 40: EXTI15_10 */
    Default_Handler,                        /* IRQ 41: RTCAlarm */
    Default_Handler,                        /* IRQ 42: OTG_FS_WKUP */
    0, 0, 0, 0, 0, 0, 0,                    /* IRQ 43-49: Reserved */
    Default_Handler,                        /* IRQ 50: TIM5 */
    Default_Handler,                        /* IRQ 51: SPI3 */
    Default_Handler,                        /* IRQ 52: UART4 */
    Default_Handler,                        /* IRQ 53: UART5 */
    Default_Handler,                        /* IRQ 54: TIM6 */
    Default_Handler,                        /* IRQ 55: TIM7 */
    Default_Handler,                        /* IRQ 56: DMA2_C1 */
    Default_Handler,                        /* IRQ 57: DMA2_C2 */
    Default_Handler,                        /* IRQ 58: DMA2_C3 */
    Default_Handler,                        /* IRQ 59: DMA2_C4 */
    Default_Handler,                        /* IRQ 60: DMA2_C5 */
    Default_Handler,                        /* IRQ 61: ETH */
    Default_Handler,                        /* IRQ 62: ETH_WKUP */
    Default_Handler,                        /* IRQ 63: CAN2_TX */
    Default_Handler,                        /* IRQ 64: CAN2_RX0 */
    Default_Handler,                        /* IRQ 65: CAN2_RX1 */
    Default_Handler,                        /* IRQ 66: CAN2_SCE */
    Default_Handler                         /* IRQ 67: OTG_FS */
};

void Reset_Handler(void) {
    /* Relocate Vector Table to Application Base in Flash */
    SCB->VTOR = FLASH_APP_BASE;

    /* Copy .data segment from Flash to SRAM */
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    /* Zero out .bss segment */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0;
    }

    /* Jump to main() */
    main();

    /* Trap if main returns */
    while (1) {}
}

void Default_Handler(void) {
    while (1) {}
}
