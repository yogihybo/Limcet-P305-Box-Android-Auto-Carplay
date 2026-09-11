#include "stm32f105.h"

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

int main(void);
void Reset_Handler(void);
void Default_Handler(void);

void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)    __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector"), used))
void (* const g_pfnVectors[])(void) = {
    (void (*)(void))(&_estack),             /* 0x00: Initial Stack Pointer */
    Reset_Handler,                          /* 0x04: Reset Handler */
    NMI_Handler,                            /* 0x08: NMI Handler */
    HardFault_Handler,                      /* 0x0C: Hard Fault Handler */
    MemManage_Handler,                      /* 0x10: MPU Fault Handler */
    BusFault_Handler,                       /* 0x14: Bus Fault Handler */
    UsageFault_Handler,                     /* 0x18: Usage Fault Handler */
    0, 0, 0, 0,                             /* 0x1C-0x28: Reserved */
    SVC_Handler,                            /* 0x2C: SVCall Handler */
    DebugMon_Handler,                       /* 0x30: Debug Monitor Handler */
    0,                                      /* 0x34: Reserved */
    PendSV_Handler,                         /* 0x38: PendSV Handler */
    SysTick_Handler                         /* 0x3C: SysTick Handler */
};

/* Real factory bootloader (0x08000338) calls this before the C-runtime
 * data/bss copy -- confirmed via disassembly, see
 * docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md section 6. Matches ST's own
 * standard CMSIS SystemInit() pattern for STM32F105/107 (Connectivity
 * Line): reset RCC to its power-on-reset shape, then set VTOR. */
static void SystemInit(void) {
    RCC->CR |= (1UL << 0);              /* HSION */
    RCC->CFGR &= 0xF0FF0000UL;          /* Reset SW, HPRE, PPRE1, PPRE2, ADCPRE, MCO */
    RCC->CR &= 0xFEF6FFFFUL;            /* Reset HSEON, CSSON, PLLON */
    RCC->CR &= ~(1UL << 18);            /* Reset HSEBYP */
    RCC->CFGR &= ~(0x7FUL << 16);       /* Reset PLLSRC, PLLXTPRE, PLLMUL, USBPRE */
    RCC->CR &= ~((1UL << 26) | (1UL << 28)); /* Reset PLL2ON, PLL3ON */
    RCC->CIR = 0x00FF0000UL;            /* Disable and clear all RCC interrupt flags */
    RCC->CFGR2 = 0;                     /* Reset PREDIV1, PREDIV2, PLL2MUL, PLL3MUL */
    SCB->VTOR = FLASH_BASE;             /* Vector table stays at the bootloader's own base */
}

void Reset_Handler(void) {
    SystemInit();

    uint32_t *pSrc = &_sidata;
    uint32_t *pDst = &_sdata;

    while (pDst < &_edata) {
        *pDst++ = *pSrc++;
    }

    pDst = &_sbss;
    while (pDst < &_ebss) {
        *pDst++ = 0;
    }

    main();

    while (1) {}
}

void Default_Handler(void) {
    while (1) {}
}
