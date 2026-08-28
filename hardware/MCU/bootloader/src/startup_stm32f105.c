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

void Reset_Handler(void) {
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
