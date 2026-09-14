#!/usr/bin/env python3
"""
patch_factory_app_live.py — Reconstructs the genuine Keil factory application firmware
extracted from the live Prado vehicle MCU (STM32F105RBT6).

Reconstructs all 573 missing words (567 VTOR hardware alignment gaps + 6 non-VTOR opcodes/literals)
resulting from the 512-byte NVIC alignment constraint under CVE-2020-8004 side-channel extraction.

Usage:
  python3 tools/patch_factory_app_live.py [input_raw.bin] [output_reconstructed.bin]
"""

import sys
import os
import argparse
import hashlib
import struct

# Factory application patch map: address -> (32-bit word, description)
FACTORY_APP_PATCH_MAP = {
    # =========================================================================
    # VECTOR TABLE: Authentic Silicon Reset & Stack Configuration
    # =========================================================================
    0x08003000: (0x20005000, "Authentic Initial Main Stack Pointer (MSP)"),
    0x08003004: (0x08003599, "Authentic Keil Reset_Handler entry point calling SystemInit"),

    # =========================================================================
    # CATEGORY B: Isolated Non-VTOR Missing Words / Filter Collisions
    # =========================================================================
    0x08003594: (0x20000004, "AHBPrescTable pointer in SystemCoreClockUpdate"),
    0x080069CC: (0x0003F88D, "strb.w r0, [sp, #3] (CAN1_TxFrame payload[3] stack store)"),
    0x08008368: (0x0006F8AD, "strh.w r0, [sp, #6] (UART timeout configuration frame stack store)"),
    0x0800A9B8: (0x0004F88D, "strb.w r0, [sp, #4] (ARK1668 0x2E 0xE1 packet construction store)"),
    0x0800AB54: (0x0006F88D, "strb.w r0, [sp, #6] (Host UART checksum computation stack store)"),

    # =========================================================================
    # BLOCK 1: Startup, Exceptions & CAN1/CAN2 ISRs (0x08003200 - 0x08003234)
    # =========================================================================
    0x08003200: (0x47704770, "bx lr / bx lr (DebugMon_Handler & PendSV_Handler returns)"),
    0x08003204: (0xF005B510, "push {r4, lr} / bl 0x08008ABA hw1 (SysTick_Handler tick caller)"),
    0x0800321C: (0x21000280, "add.w r2, r1, r0, lsl #2 hw2 / movs r1, #0 (CAN1_RX0_IRQHandler)"),
    0x08003220: (0xF0004837, "ldr r0, [pc, #0xdc] / bl 0x08004012 hw1 (Load CAN1_BASE & call CAN_Receive)"),
    0x08003224: (0x4836FEF6, "bl 0x08004012 hw2 / ldr r0, [pc, #0xd8] (Load 0x200003A0 rx buffer)"),
    0x08003228: (0x012CF890, "ldrb.w r0, [r0, #0x12c] (Fetch rx tail index)"),
    0x08003234: (0xF8904608, "mov r0, r1 / ldrb.w r0, [r0, #0x12c] hw1 (Load and check queue limit)"),

    # =========================================================================
    # BLOCK 2: Clock Trees & Core SystemInit (0x08003400 - 0x08003434)
    # =========================================================================
    0x08003400: (0xF0406840, "ldr r0, [r0, #4] / orr r0, r0, #2 hw1 (RCC_CFGR SW[1:0] switch to PLL)"),
    0x08003404: (0x60480002, "orr hw2 / str r0, [r1, #4] (Write RCC_CFGR switch to PLL)"),
    0x0800341C: (0xBD10FEF3, "bl 0x08003204 hw2 / pop {r4, pc} (SysTick_Config return)"),
    0x08003420: (0x4850B510, "push {r4, lr} / ldr r0, [pc, #0x140] (SystemInit RCC_CR load)"),
    0x08003424: (0xF0406800, "ldr r0, [r0] / orr r0, r0, #1 hw1 (RCC_CR_HSION enable)"),
    0x08003428: (0x494E0001, "orr hw2 / ldr r1, [pc, #0x138] (Load RCC_CFGR literal)"),
    0x08003434: (0x494B4008, "ands r0, r1 / ldr r1, [pc, #0x12c] (RCC_CFGR reset clear)"),

    # =========================================================================
    # BLOCK 3: NVIC_Init Implementation (0x08003600 - 0x08003634)
    # =========================================================================
    0x08003600: (0x784440CA, "lsrs r2, r1 / ldrb r4, [r0, #1] (NVIC_Init preemption priority)"),
    0x08003604: (0xF103FA04, "lsl.w r1, r4, r3 (NVIC_Init priority shift)"),
    0x0800361C: (0x40AC2401, "movs r4, #1 / lsls r4, r5 (Calculate NVIC ISER bitmask)"),
    0x08003620: (0x116D7805, "ldrb r5, [r0] / asrs r5, r5, #5 (Compute ISER register index)"),
    0x08003624: (0xF10500AD, "lsls r5, r5, #2 / add.w r5, r5, #-0x1fff2000 hw1"),
    0x08003628: (0xF8C525E0, "add.w hw2 / str.w r4, [r5, #0x100] hw1 (NVIC->ISER write)"),
    0x08003634: (0x2401051F, "and.w r5, r4, #0x1f hw2 / movs r4, #1 (NVIC ICER channel calculation)"),

    # =========================================================================
    # BLOCK 4: ADC Mode & Sequence Configurations (0x08003800 - 0x08003834)
    # =========================================================================
    0x08003800: (0x47704770, "bx lr / bx lr (ADC_GetSoftwareStartConvStatus return paths)"),
    0x08003804: (0x460AB510, "push {r4, lr} / mov r2, r1 (ADC_DiscModeChannelCountConfig)"),
    0x0800381C: (0x6842B121, "cbz r1, #0x8003828 / ldr r2, [r0, #4] (ADC_DiscModeCmd check)"),
    0x08003820: (0x6200F442, "orr.w r2, r2, #0x800 (Enable ADC_CR1_DISCEN)"),
    0x08003824: (0x47706042, "str r2, [r0, #4] / bx lr (Store CR1 & return)"),
    0x08003828: (0xF4226842, "ldr r2, [r0, #4] / bic.w r2, r2, #0x800 hw1 (Disable DISCEN)"),
    0x08003834: (0x460D4604, "mov r4, r0 / mov r5, r1 (ADC_RegularChannelConfig setup)"),

    # =========================================================================
    # BLOCK 5: ADC Injected Channels & Offsets (0x08003A00 - 0x08003A34)
    # =========================================================================
    0x08003A00: (0x0C87EB07, "add.w ip, r7, r7, lsl #2 (ADC_InjectedChannelConfig JSQR pos)"),
    0x08003A04: (0xF20CFA08, "lsl.w r2, r8, ip (Shift channel into JSQ position)"),
    0x08003A1C: (0xEA411E53, "subs r3, r2, #1 / orr.w r1, r1, r3, lsl #20 hw1 (JSQR_JL config)"),
    0x08003A20: (0x63815103, "orr.w hw2 / str r1, [r0, #0x38] (Store JSQR length)"),
    0x08003A24: (0xB508BD10, "pop {r4, pc} / push {r3, lr} (InjectedSequencer return / SetInjectedOffset)"),
    0x08003A28: (0x93002300, "movs r3, #0 / str r3, [sp] (Init tmpreg on stack)"),
    0x08003A34: (0x601A9B00, "ldr r3, [sp] / str r2, [r3] (Store Offset into JOFRx)"),

    # =========================================================================
    # BLOCK 6: CAN Initialization & BTR Bit Timing (0x08003C00 - 0x08003C34)
    # =========================================================================
    0x08003C00: (0x78CD07A4, "lsls r4, r4, #0x1e / ldrb r5, [r1, #3] (CAN_Init BTR TS1/TS2 timing)"),
    0x08003C04: (0x6405EA44, "orr.w r4, r4, r5, lsl #24 (CAN_Init BTR TS1/TS2 assembly)"),
    0x08003C1C: (0xF0246814, "ldr r4, [r2] / bic r4, r4, #1 hw1 (CAN_Init MCR enter normal mode)"),
    0x08003C20: (0x60140401, "bic hw2 / str r4, [r2] (Store CAN_MCR normal mode request)"),
    0x08003C24: (0xE0002300, "movs r3, #0 / b 0x08003C2A (Init INAK wait timeout counter)"),
    0x08003C28: (0x68541C5B, "adds r3, r3, #1 / ldr r4, [r2, #4] (Poll CAN_MSR INAK bit)"),
    0x08003C34: (0x42A374FF, "movw r4, #0xffff hw2 / cmp r3, r4 (Check INAK wait timeout limit)"),

    # =========================================================================
    # BLOCK 7: CAN Filter Configuration & Transmit Status (0x08003E00 - 0x08003E34)
    # =========================================================================
    0x08003E00: (0x72C2F44F, "mov.w r2, #0x184 (CAN_FilterInit FiRx load address)"),
    0x08003E04: (0xF44F5013, "str r3, [r2, r0] / mov.w r2, #0x194 hw1 (Store FiR1 & prep FiR2)"),
    0x08003E1C: (0x7380F422, "bic r3, r2, #0x100 (CAN_FilterInit FiRx bit clear)"),
    0x08003E20: (0x72D2F44F, "mov.w r2, #0x1a4 (CAN_FilterInit FiRx offset load)"),
    0x08003E24: (0xBD105013, "str r3, [r2, r0] / pop {r4, pc} (CAN_FilterInit exit)"),
    0x08003E28: (0x4602B510, "push {r4, lr} / mov r2, r0 (CAN_TransmitStatus entry)"),
    0x08003E34: (0x6F80F1B3, "cmp.w r3, #0x4000000 (CAN_TSR_RQCP0 check)"),

    # =========================================================================
    # BLOCK 8: CAN ITConfig & CAN_Receive Mailbox Parser (0x08004000 - 0x08004034)
    # =========================================================================
    0x08004000: (0x6882E005, "b 0x0800400E / ldr r2, [r0, #8] (CAN_ITConfig branch / read IER)"),
    0x08004004: (0x4200F422, "bic r2, r2, #0x8000 (CAN_ITConfig disable interrupt bit)"),
    0x0800401C: (0xF003681B, "ldr r3, [r3] / and r3, r3, #4 hw1 (CAN_Receive read RIxR IDE bit)"),
    0x08004020: (0x72130304, "and hw2 / strb r3, [r2, #8] (Store IDE to RxMessage.IDE)"),
    0x08004024: (0xB9537A13, "ldrb r3, [r2, #8] / cbnz r3, 0x0800403E (Check IDE standard/extended)"),
    0x08004028: (0x73D8F500, "add.w r3, r0, #0x1b0 (CAN_Receive calculate RI0R address)"),
    0x08004034: (0xEA0474FF, "movw r4, #0x7ff hw2 / and.w r3, r4, r3, lsr #21 (Extract StdId)"),

    # =========================================================================
    # BLOCK 9: CAN OperatingMode & Error Status Registers (0x08004200 - 0x08004234)
    # =========================================================================
    0x08004200: (0x0302F043, "orr r3, r3, #2 (CAN_OperatingModeRequest set INRQ)"),
    0x08004204: (0xE000600B, "str r3, [r1] / b 0x0800420A (Store MCR and enter wait loop)"),
    0x0800421C: (0x2B020302, "and r3, r3, #2 / cmp r3, #2 (CAN_OperatingModeRequest INAK check)"),
    0x08004220: (0x2001D101, "bne 0x08004226 / movs r0, #1 (Success return path)"),
    0x08004224: (0x47704770, "bx lr / bx lr (CAN_OperatingModeRequest returns)"),
    0x08004228: (0x694A4601, "mov r1, r0 / ldr r2, [r1, #0x14] (CAN_GetLastErrorCode read ESR)"),
    0x08004234: (0xBF00694A, "ldr r2, [r1, #0x14] / nop (CAN_GetReceiveErrorCounter read ESR)"),

    # =========================================================================
    # BLOCK 10: CAN IT Dispatch & Pending Bit Queries (0x08004400 - 0x08004434)
    # =========================================================================
    0x08004400: (0x69A0E021, "b 0x08004446 / ldr r0, [r4, #0x18] (CAN_GetITStatus case EWG)"),
    0x08004404: (0xF7FF2101, "movs r1, #1 / bl 0x08004306 hw1 (CAN_GetITStatus check EWGF)"),
    0x0800441C: (0x210469A0, "ldr r0, [r4, #0x18] / movs r1, #4 (CAN_GetITStatus case BOF)"),
    0x08004420: (0xFF71F7FF, "bl 0x08004306 (CAN_GetITStatus check BOFF)"),
    0x08004424: (0xE00E4606, "mov r6, r0 / b 0x08004446 (CAN_GetITStatus BOF exit)"),
    0x08004428: (0x69A0E00E, "b 0x08004448 / ldr r0, [r4, #0x18] (Default branch & case LEC)"),
    0x08004434: (0x6860E007, "b 0x08004446 / ldr r0, [r4, #4] (LEC exit & case ERR load MSR)"),

    # =========================================================================
    # BLOCK 11: GPIO Remapping Configuration Table (0x08004600 - 0x08004634)
    # =========================================================================
    0x08004600: (0xE0186011, "str r1, [r2] / b 0x08004636 (Store pin mask & branch to exit)"),
    0x08004604: (0x313C494F, "ldr r1, [pc, #0x13c] / adds r1, #0x3c (Load GPIO table case 3)"),
    0x0800461C: (0x4949E00B, "b 0x08004636 / ldr r1, [pc, #0x124] (Exit branch / load GPIO table case 4)"),
    0x08004620: (0x42883150, "adds r1, #0x50 / cmp r0, r1 (GPIO table case 4 check)"),
    0x08004624: (0x4947D107, "bne 0x08004636 / ldr r1, [pc, #0x11c] (Default exit / load table case 4 ptr)"),
    0x08004628: (0x68091F09, "subs r1, r1, #4 / ldr r1, [r1] (Dereference case 4 register)"),
    0x08004634: (0x47706011, "str r1, [r2] / bx lr (Store final pin mask & function return)"),

    # =========================================================================
    # BLOCK 12: EXTI Software Interrupt & Line Flags (0x08004800 - 0x08004834)
    # =========================================================================
    0x08004800: (0x49154770, "bx lr / ldr r1, [pc, #0x54] (EXTI_StructInit exit / EXTI_GenerateSWInterrupt)"),
    0x08004804: (0x68093110, "adds r1, #0x10 / ldr r1, [r1] (Calculate & read EXTI_SWIER)"),
    0x0800481C: (0xB10A400A, "ands r2, r1 / cbz r2, 0x08004824 (EXTI_GetFlagStatus check PR bit)"),
    0x08004820: (0xE0002001, "movs r0, #1 / b 0x08004826 (Return SET & exit)"),
    0x08004824: (0x47702000, "movs r0, #0 / bx lr (Return RESET & exit)"),
    0x08004828: (0x3114490C, "ldr r1, [pc, #0x30] / adds r1, #0x14 (EXTI_ClearFlag load EXTI_PR address)"),
    0x08004834: (0x3024F8DF, "ldr.w r3, [pc, #0x24] (EXTI_GetITStatus load EXTI_BASE)"),

    # =========================================================================
    # BLOCK 13: Flash EraseAllPages & Flag Status (0x08004A00 - 0x08004A34)
    # =========================================================================
    0x08004A00: (0x0040F040, "orr r0, r0, #0x40 (FLASH_EraseAllPages set FLASH_CR_STRT)"),
    0x08004A04: (0xF44F6108, "str r0, [r1, #0x10] / mov.w r0, #0xb0000 hw1 (Trigger erase & prep timeout)"),
    0x08004A1C: (0x46206108, "str r0, [r1, #0x10] / mov r0, r4 (Clear FLASH_CR_MER & set return status)"),
    0x08004A20: (0x4601BD10, "pop {r4, pc} / mov r1, r0 (FLASH_EraseAllPages exit / FLASH_GetFlagStatus entry)"),
    0x08004A24: (0x4A8E2000, "movs r0, #0 / ldr r2, [pc, #0x238] (Init status & load FLASH_R_BASE)"),
    0x08004A28: (0x401168D2, "ldr r2, [r2, #0xc] / ands r1, r2 (Read FLASH_SR & test flag bit)"),
    0x08004A34: (0xBF004770, "bx lr / nop (FLASH_GetFlagStatus return)"),

    # =========================================================================
    # BLOCK 14: Option Byte Write Protection Programming (0x08004C00 - 0x08004C34)
    # =========================================================================
    0x08004C00: (0x02E88006, "strh r6, [r0] / lsls r0, r5, #0xb (Store WRP0 & shift timeout)"),
    0x08004C04: (0xFE7FF7FF, "bl 0x08004906 (FLASH_WaitForLastOperation call for WRP0)"),
    0x08004C1C: (0x4605FE74, "bl 0x08004906 hw2 / mov r5, r0 (WRP1 wait complete & copy status)"),
    0x08004C20: (0xD10A2D04, "cmp r5, #4 / bne 0x08004C3A (Check WRP1 status & skip on error)"),
    0x08004C24: (0x0FFFF1B8, "cmp.w r8, #0xff (Check if WRP2 byte modified)"),
    0x08004C28: (0x4810D007, "beq 0x08004C3A / ldr r0, [pc, #0x40] (Skip WRP2 / load OB_BASE)"),
    0x08004C34: (0xFE67F7FF, "bl 0x08004906 (FLASH_WaitForLastOperation call for WRP2)"),

    # =========================================================================
    # BLOCK 15: Flash Status & GPIO Peripheral Clock Control (0x08004E00 - 0x08004E34)
    # =========================================================================
    0x08004E00: (0x0110F001, "and.w r1, r1, #0x10 (FLASH_GetStatus test WRPRTERR bit)"),
    0x08004E04: (0x2003B109, "cbz r1, 0x08004E0A / movs r0, #3 (Branch or return FLASH_ERROR_WRP)"),
    0x08004E1C: (0x1FFFF800, "OB_BASE literal 0x1FFFF800 (Flash option bytes base address)"),
    0x08004E20: (0x4604B510, "push {r4, lr} / mov r4, r0 (GPIO clock controller entry)"),
    0x08004E24: (0x428448CC, "ldr r0, [pc, #0x330] / cmp r4, r0 (Load GPIOA_BASE & compare)"),
    0x08004E28: (0x2101D108, "bne 0x08004E3C / movs r1, #1 (Branch if not GPIOA & set ENABLE)"),
    0x08004E34: (0xF0002004, "movs r0, #4 / bl 0x08005A48 hw1 (Set APB2_GPIOA & call ClockCmd)"),

    # =========================================================================
    # BLOCK 16: GPIO Mode Initialization & Port Data Read/Write (0x08005000 - 0x08005034)
    # =========================================================================
    0x08005000: (0x70C12104, "movs r1, #4 / strb r1, [r0, #3] (GPIO_StructInit Mode IN_FLOATING)"),
    0x08005004: (0x46024770, "bx lr / mov r2, r0 (GPIO_StructInit exit / GPIO_ReadInputDataBit entry)"),
    0x0800501C: (0x4770B280, "uxth r0, r0 / bx lr (GPIO_ReadInputData return)"),
    0x08005020: (0x20004602, "mov r2, r0 / movs r0, #0 (GPIO_ReadOutputDataBit entry)"),
    0x08005024: (0x400B68D3, "ldr r3, [r2, #0xc] / ands r3, r1 (Read GPIOx_ODR & test bit)"),
    0x08005028: (0x2001B10B, "cbz r3, 0x0800502E / movs r0, #1 (Test bit result & return SET)"),
    0x08005034: (0xB28068C8, "ldr r0, [r1, #0xc] / uxth r0, r0 (GPIO_ReadOutputData read ODR)"),

    # =========================================================================
    # BLOCK 17: I2C Speed & Duty Cycle Calculation (0x08005200 - 0x08005234)
    # =========================================================================
    0x08005200: (0x2804B280, "uxth r0, r0 / cmp r0, #4 (I2C standard mode speed limit check)"),
    0x08005204: (0x460700D3, "blo 0x08005208 / mov r7, r0 (Preserve min CCR=4 or update result)"),
    0x0800521C: (0xEB006828, "ldr r0, [r5] / add.w r0, r0, r0, lsl #1 hw1 (Duty 2: 3 * ClockSpeed)"),
    0x08005220: (0xFBB80040, "add.w hw2 / udiv r0, r8, r0 hw1 (Calculate CCR for Duty 2)"),
    0x08005224: (0xB287F0F0, "udiv hw2 / uxth r7, r0 (Truncate Duty 2 CCR result)"),
    0x08005228: (0x6828E009, "b 0x0800523E / ldr r0, [r5] (Skip 16_9 config & prep Duty 16_9)"),
    0x08005234: (0xF0F0FBB8, "udiv r0, r8, r0 (Calculate CCR for Duty 16_9)"),

    # =========================================================================
    # BLOCK 18: I2C Control Bits: SMBusAlert, PEC, NACKPosition (0x08005400 - 0x08005434)
    # =========================================================================
    0x08005400: (0x5200F442, "orr r2, r2, #0x2000 (I2C_SMBusAlertConfig enable alert)"),
    0x08005404: (0x47708002, "strh r2, [r0] / bx lr (Store CR1 & return)"),
    0x0800541C: (0xE0048002, "strh r2, [r0] / b 0x0800542A (Store CR1 enable PEC & exit)"),
    0x08005420: (0xF64E8802, "ldrh r2, [r0] / movw r3, #0xefff hw1 (Prep PEC disable mask)"),
    0x08005424: (0x401A73FF, "movw hw2 / ands r2, r3 (Clear PEC bit)"),
    0x08005428: (0x47708002, "strh r2, [r0] / bx lr (Store CR1 disable PEC & return)"),
    0x08005434: (0x6200F442, "orr r2, r2, #0x800 (I2C_NACKPositionConfig set POS bit)"),

    # =========================================================================
    # BLOCK 19: RCC DeInit & HSE Configuration (0x08005600 - 0x08005634)
    # =========================================================================
    0x08005600: (0xF04F6008, "str r0, [r1] / mov.w r0, #0xff0000 hw1 (Reset RCC_CR & prep CIR mask)"),
    0x08005604: (0x6088007F, "mov.w hw2 / str r0, [r1, #8] (Clear all RCC interrupt flags in CIR)"),
    0x0800561C: (0xF4216809, "ldr r1, [r1] / bic.w r1, r1, #0x40000 hw1 (Clear HSEBYP bit)"),
    0x08005620: (0x60112180, "bic.w hw2 / str r1, [r2] (Store RCC_CR cleared HSE bits)"),
    0x08005624: (0x3F80F5B0, "cmp.w r0, #0x10000 (Check if RCC_HSE_ON requested)"),
    0x08005628: (0xF5B0D003, "beq 0x08005632 / cmp.w r0, #0x40000 hw1 (Branch ON / check Bypass)"),
    0x08005634: (0xF4416809, "ldr r1, [r1] / orr.w r1, r1, #0x10000 hw1 (Enable HSEON bit)"),

    # =========================================================================
    # BLOCK 20: RCC LSE, RTC Clock Source & CSS Security (0x08005800 - 0x08005834)
    # =========================================================================
    0x08005800: (0xE004D10B, "bne 0x0800581A / b 0x0800580E (LSE mode dispatch check)"),
    0x08005804: (0x4A6A2101, "movs r1, #1 / ldr r2, [pc, #0x1a8] (LSE_ON set bit & load RCC_BASE)"),
    0x0800581C: (0x60084967, "ldr r1, [pc, #0x19c] / str r0, [r1] (RCC_LSICmd set LSION bitband)"),
    0x08005820: (0xBF004770, "bx lr / nop (RCC_LSICmd return)"),
    0x08005824: (0x6A114A62, "ldr r2, [pc, #0x188] / ldr r1, [r2, #0x20] (Load BDCR for RTC source)"),
    0x08005828: (0x0100EA41, "orr.w r1, r1, r0 (Set RTCCLKSource selection bits)"),
    0x08005834: (0x47706008, "str r0, [r1] / bx lr (RCC_ClockSecuritySystemCmd write CSSON)"),

    # =========================================================================
    # BLOCK 21: RCC Peripheral Clock Enabling & Control (0x08005A00 - 0x08005A34)
    # =========================================================================
    0x08005A00: (0x4B2D4302, "orrs r2, r0 / ldr r3, [pc, #0xb4] (RCC_APB2PeriphClockCmd read/set)"),
    0x08005A04: (0x4770619A, "str r2, [r3, #0x18] / bx lr (RCC_APB2PeriphClockCmd store & exit)"),
    0x08005A1C: (0x61DA4B26, "ldr r3, [pc, #0x98] / str r2, [r3, #0x1c] (RCC_APB1PeriphClockCmd enable)"),
    0x08005A20: (0x4A254770, "bx lr / ldr r2, [pc, #0x94] (RCC_APB1PeriphClockCmd exit & disable entry)"),
    0x08005A24: (0x438269D2, "ldr r2, [r2, #0x1c] / bics r2, r0 (RCC_APB1PeriphClockCmd disable bic)"),
    0x08005A28: (0x61DA4B23, "ldr r3, [pc, #0x8c] / str r2, [r3, #0x1c] (RCC_APB1PeriphClockCmd store)"),
    0x08005A34: (0x4B204302, "orrs r2, r0 / ldr r3, [pc, #0x80] (RCC_AHBPeriphResetCmd read/set)"),

    # =========================================================================
    # BLOCK 22: USART Baud Rate & Structure Configuration (0x08005C00 - 0x08005C34)
    # =========================================================================
    0x08005C00: (0x2032E008, "b 0x08005C14 / movs r0, #0x32 (USART_Init BRR oversampling else branch)"),
    0x08005C04: (0x1009EB00, "add.w r0, r0, sb, lsl #4 (USART_Init BRR 16x fractional calculation)"),
    0x08005C1C: (0x5180F242, "movw r1, #0x2580 (USART_StructInit default baud 9600)"),
    0x08005C20: (0x21006001, "str r1, [r0] / movs r1, #0 (Store baudrate & clear word length)"),
    0x08005C24: (0x80C18081, "strh r1, [r0, #4] / strh r1, [r0, #6] (StopBits 1, Parity None)"),
    0x08005C28: (0x210C8101, "strh r1, [r0, #8] / movs r1, #0xc (Store parity & set Mode Rx|Tx)"),
    0x08005C34: (0x4602B510, "push {r4, lr} / mov r2, r0 (USART_ClockInit function prologue)"),

    # =========================================================================
    # BLOCK 23: USART IrDA, Status Flags & Clear Logic (0x08005E00 - 0x08005E34)
    # =========================================================================
    0x08005E00: (0x8A82B121, "cbz r1, #0x8005e0c / ldrh r2, [r0, #0x14] (USART_IrDACmd check enable & read CR3)"),
    0x08005E04: (0x0202F042, "orr.w r2, r2, #2 (USART_IrDACmd set IREN bit in CR3)"),
    0x08005E1C: (0x7F00F5B1, "cmp.w r1, #0x200 (USART_GetFlagStatus CTS check)"),
    0x08005E20: (0xBF00D100, "bne #0x8005e24 / nop (USART_GetFlagStatus CTS bypass)"),
    0x08005E24: (0x420A8812, "ldrh r2, [r2] / tst r2, r1 (USART_GetFlagStatus read SR & test flag)"),
    0x08005E28: (0x2001D001, "beq #0x8005e2e / movs r0, #1 (USART_GetFlagStatus branch RESET / return SET)"),
    0x08005E34: (0xF5B27200, "and.w r2, r1, #0x200 hw2 / cmp.w r2, #0x200 hw1 (USART_ClearFlag CTS mask check)"),

    # =========================================================================
    # BLOCK 24: Board Init, Device Table & Module Dispatch (0x08006000 - 0x08006034)
    # =========================================================================
    0x08006000: (0xFD7BF002, "bl #0x8008afa (delay_ms(1) post ADC/DMA initialization)"),
    0x08006004: (0xB510BD10, "pop {r4, pc} / push {r4, lr} (system_hw_init return / device_table_init prologue)"),
    0x0800601C: (0xB510BD10, "pop {r4, pc} / push {r4, lr} (device_table_init return / bsp_init prologue)"),
    0x08006020: (0xFFF1F7FF, "bl #0x8006006 (bsp_init call device_table_init)"),
    0x08006024: (0xF8EAF000, "bl #0x80061fc (bsp_init call protocol module 1 init)"),
    0x08006028: (0xF900F000, "bl #0x800622c (bsp_init call CAN1 RX circular queue init)"),
    0x08006034: (0xFBA6F003, "bl #0x8009784 (bsp_init call module 5 circular buffer init)"),

    # =========================================================================
    # BLOCK 25: Multi-Channel Dispatcher & Protocol Initialization (0x08006200 - 0x08006234)
    # =========================================================================
    0x08006200: (0xF000F8F9, "bl #0x80063f4 hw2 / bl #0x80063ac hw1 (Poll Channel 0 & CAN)"),
    0x08006204: (0xBF00F8D3, "bl #0x80063ac hw2 / nop (Complete CAN poll & align Channel 1)"),
    0x0800621C: (0x4B4E7822, "ldrb r2, [r4] / ldr r3, [pc, #0x138] (Load Channel 1 cmd_id & Table1 base)"),
    0x08006220: (0x1022F853, "ldr.w r1, [r3, r2, lsl #2] (Index Channel 1 handler table)"),
    0x08006224: (0xBD104788, "blx r1 / pop {r4, pc} (Execute Channel 1 handler & return)"),
    0x08006228: (0xBF00BD10, "pop {r4, pc} / nop (Channel 1 bypass return & function align)"),
    0x08006234: (0x4849FB23, "bl #0x800b87c hw2 / ldr r0, [pc, #0x124] (Call __aeabi_memclr & reload McuSettings)"),

    # =========================================================================
    # BLOCK 26: Channel 0 Dispatcher & Subsystem State Machine (0x08006400 - 0x08006434)
    # =========================================================================
    0x08006400: (0xDA05280C, "cmp r0, #0xc / bge #0x8006410 (Channel 0 bounds check: 12 commands)"),
    0x08006404: (0x78228860, "ldrh r0, [r4, #2] / ldrb r2, [r4] (Load Channel 0 cmd_id & payload len)"),
    0x0800641C: (0x21024608, "mov r0, r1 / movs r1, #2 (Subsystem struct 0x2000025A base & ch1 state)"),
    0x08006420: (0x21017081, "strb r1, [r0, #2] / movs r1, #1 (Channel 1 state = 2, substate = 1)"),
    0x08006424: (0x210270C1, "strb r1, [r0, #3] / movs r1, #2 (Channel 2 state = 2)"),
    0x08006428: (0x21017101, "strb r1, [r0, #4] / movs r1, #1 (Channel 2 substate = 1, prepare ch3)"),
    0x08006434: (0x210071C1, "strb r1, [r0, #7] / movs r1, #0 (Channel 3 substate = 1, timeout = 0)"),

    # =========================================================================
    # BLOCK 27: Channel 1 Command 0xA0 / 0xE1 Handlers & TBB Table (0x08006600 - 0x08006634)
    # =========================================================================
    0x08006600: (0x22007008, "strb r0, [r1] / movs r2, #0 (Command 0xA0 state update & event param)"),
    0x08006604: (0x46102101, "movs r1, #1 / mov r0, r2 (Command 0xA0 event type 1, channel 0)"),
    0x0800661C: (0xF000E8DF, "tbb [pc, r0] (Command 0xE1 jump table for subcommands 0-5)"),
    0x08006620: (0x0E0D0C1C, "table entries [0x1c, 0x0c, 0x0d, 0x0e] (Offsets for cases 0, 1, 2, 3)"),
    0x08006624: (0x2807100F, "table [0x0f, 0x10] / cmp r0, #7 (Offsets for cases 4, 5 & Case 7 test)"),
    0x08006628: (0x2808D00C, "beq #0x8006644 / cmp r0, #8 (Case 7 branch & Case 8 test)"),
    0x08006634: (0xE00FD009, "beq #0x800664a / b #0x8006658 (Case 0x7F branch & default break)"),

    # =========================================================================
    # BLOCK 28: Reversing Camera State Filter & Host UART Forwarder (0x08006800 - 0x08006834)
    # =========================================================================
    0x08006800: (0x48B5B968, "cbnz r0, #0x800681e / ldr r0, [pc, #0x2d4] (Check PDC low byte & load 0x2000003B)"),
    0x08006804: (0x28017800, "ldrb r0, [r0] / cmp r0, #1 (Check reverse active flag == 1)"),
    0x0800681C: (0x2009FFA3, "bl #0x8007764 hw2 / movs r0, #9 (Finish I2C video mux cmd & check status 9)"),
    0x08006820: (0x2F24EBB0, "cmp.w r0, r4, asr #8 (Compare status high byte with 9)"),
    0x08006824: (0xB2E0D103, "bne #0x800682e / uxtb r0, r4 (Skip if not 9 & extract status low byte)"),
    0x08006828: (0xD1002802, "cmp r0, #2 / bne #0x800682e (Check low byte == 2 for early return)"),
    0x08006834: (0xE7F9D100, "bne #0x8006838 / b #0x800682c (Bypass host packet forward if video route == 2)"),

    # =========================================================================
    # BLOCK 29: CAN1 RX Frame Unpacking & Command 4 Dispatch (0x08006A00 - 0x08006A34)
    # =========================================================================
    0x08006A00: (0x0002F88D, "strb.w r0, [sp, #2] (CAN1 RX payload byte 2 stack store)"),
    0x08006A04: (0x7E804838, "ldr r0, [pc, #0xe0] / ldrb r0, [r0, #0x1a] (CAN1 RX buffer + 0x1A offset load)"),
    0x08006A0C: (0x20000003, "strb.w r0, [sp, #3] completion & movs r0, #0 (Payload byte 3 & padding)"),
    0x08006A1C: (0xF7FF4668, "mov r0, sp / bl #0x8006668 hw1 (Call packet assembler)"),
    0x08006A20: (0xBD1CFE23, "bl #0x8006668 hw2 / pop {r2, r3, r4, pc} (Complete call & function return)"),
    0x08006A24: (0x4604B538, "push {r3, r4, r5, lr} / mov r4, r0 (Command 4 handler prologue)"),
    0x08006A28: (0x90002000, "movs r0, #0 / str r0, [sp] (Command 4 local frame buffer init)"),
    0x08006A34: (0xB1007A00, "ldrb r0, [r0, #8] / cbz r0, #0x8006a3a (Check command substate & bypass exit)"),

    # =========================================================================
    # BLOCK 30: GPIO Pin 5 & Pin 15 (PC15) State Controller Functions (0x08006C00 - 0x08006C34)
    # =========================================================================
    0x08006C00: (0x4604B510, "push {r4, lr} / mov r4, r0 (Pin 5 toggle prologue)"),
    0x08006C04: (0x2120B124, "cbz r4, #0x8006c10 / movs r1, #0x20 (Branch reset / Pin 5 mask 0x20)"),
    0x08006C1C: (0xB12C4604, "mov r4, r0 / cbz r4, #0x8006c2c (PC15 controller r4 store & zero branch)"),
    0x08006C20: (0x4100F44F, "mov.w r1, #0x8000 (PC15 mask 0x8000)"),
    0x08006C24: (0xFEF748E5, "ldr r0, [pc, #0x394] / bl #0x800503a hw1 (Load GPIOC & GPIO_SetBits hw1)"),
    0x08006C28: (0xE004FA08, "bl #0x800503a hw2 / b #0x8006c36 (GPIO_SetBits hw2 & exit branch)"),
    0x08006C34: (0xBD10FA04, "bl #0x800503e hw2 / pop {r4, pc} (GPIO_ResetBits hw2 & function return)"),

    # =========================================================================
    # BLOCK 31: Board Power-On PB8 Trigger & Power-Down GPIO Sequence (0x08006E00 - 0x08006E34)
    # =========================================================================
    0x08006E00: (0xF000D101, "bne.n #0x8006e06 / bl #0x8006e4a hw1 (PB8 camera trigger branch)"),
    0x08006E04: (0xBD08F822, "bl #0x8006e4a hw2 / pop {r3, pc} (Complete PB8 call & power-on exit)"),
    0x08006E1C: (0xFF44F7FF, "bl #0x8006ca8 (PB4 reset in power-down sequence)"),
    0x08006E20: (0xF7FF2000, "movs r0, #0 / bl #0x8006c8a hw1 (PC12 reset prep)"),
    0x08006E24: (0x2000FF32, "bl #0x8006c8a hw2 / movs r0, #0 (PC12 reset & PB3 reset prep)"),
    0x08006E28: (0xFF22F7FF, "bl #0x8006c70 (PB3 reset in power-down sequence)"),
    0x08006E34: (0xFEF1F7FF, "bl #0x8006c1a (PC15 reset in power-down sequence)"),

    # =========================================================================
    # BLOCK 32: PC6 Strapping Reader & Video Route Arbitrator (0x08007000 - 0x08007034)
    # =========================================================================
    0x08007000: (0x2140B510, "push {r4, lr} / movs r1, #0x40 (PC6 bit strapping prologue)"),
    0x08007004: (0xF7FD4892, "ldr r0, [pc, #0x248] / bl #0x8005006 hw1 (Load GPIOC & read PC6)"),
    0x0800701C: (0xFF76F7FF, "bl #0x8006f0c (Disable video mux pin 10)"),
    0x08007020: (0xF7FF2001, "movs r0, #1 / bl #0x8006f2a hw1 (Set video mux pin 15 prep)"),
    0x08007024: (0xE051FF82, "bl #0x8006f2a hw2 / b.n #0x80070cc (Complete pin 15 & exit branch)"),
    0x08007028: (0xF890488A, "ldr r0, [pc, #0x228] / ldrb.w r0, [r0, #0x43] hw1 (Load vehicle struct & offset 0x43)"),
    0x08007034: (0xB1587800, "ldrb r0, [r0] / cbz r0, #0x8007050 (Check 0x20000042 & branch if 0)"),

    # =========================================================================
    # BLOCK 33: Camera State Machine & Video Route Default Restorer (0x08007200 - 0x08007234)
    # =========================================================================
    0x08007200: (0x28017800, "ldrb r0, [r0] / cmp r0, #1 (Check 0x2000025A channel 0 state == 1)"),
    0x08007204: (0x4813D10C, "bne #0x8007220 / ldr r0, [pc, #0x4c] (Branch channel 1 / Load 0x200001BC)"),
    0x0800721C: (0xE011FE16, "bl #0x8006e4a hw2 / b.n #0x8007244 (Complete PB8 call & branch exit)"),
    0x08007220: (0x7880480E, "ldr r0, [pc, #0x38] / ldrb r0, [r0, #2] (Load 0x2000025A channel 1 state)"),
    0x08007224: (0xD1002801, "cmp r0, #1 / bne #0x800722a (Check channel 1 == 1 / fail branch)"),
    0x08007228: (0xE005E000, "b.n #0x800722c / b.n #0x8007238 (Bridge to camera off / default reset)"),
    0x08007234: (0xFD00F7FF, "bl #0x8006c38 (Enable PB2 video mute line)"),

    # --- BLOCK 34 (0x08007400 - 0x08007434): Sensor Debouncer Channels 0/2 ---
    0x08007400: (0x700448A0, "ldr r0, [pc, #0x280] / strb r4, [r0] (Channel 2 state writeback)"),
    0x08007404: (0xE7DEBF00, "nop / b.n #0x80073c6 (Function 1 shared epilogue bridge)"),
    0x0800741C: (0xEB04BF00, "nop / add.w r0, r4, r4, lsl #1 (hw1) (Loop element offset calc)"),
    0x08007420: (0x49990044, "add.w r0 (hw2) / ldr r1, [pc, #0x264] (Load channel 0 threshold table)"),
    0x08007424: (0x42A85C08, "ldrb r0, [r1, r0] / cmp r0, r5 (Channel 0 lower threshold check)"),
    0x08007428: (0xEB04DC08, "bgt #0x800743c / add.w r0, r4, r4, lsl #1 (hw1)"),
    0x08007434: (0x1C60DB02, "blt #0x800743c / adds r0, r4, #1 (Channel 0 window match / counter inc)"),

    # --- BLOCK 35 (0x08007600 - 0x08007634): Sensor Array Loop & Video/Display GPIO Init ---
    0x08007600: (0xDBE82808, "cmp r0, #8 / blt #0x80075d6 (Loop counter check & branch)"),
    0x08007604: (0xB5004770, "bx lr / push {lr} (Leaf return & function 0x08007606 prologue)"),
    0x0800761C: (0xA9100043, "strb.w r0, [sp, #0x43] / add r1, sp, #0x40 (PA1 IPU mode & init struct ptr)"),
    0x08007620: (0xF7FD481E, "ldr r0, [pc, #0x78] / bl #0x8004ee0 hw1 (Load GPIOA & call GPIO_Init)"),
    0x08007624: (0x2002FC5D, "bl #0x8004ee0 hw2 / movs r0, #2 (Complete PA1 init & prep PB1 mask)"),
    0x08007628: (0x0040F8AD, "strh.w r0, [sp, #0x40] (Store PB1 mask into init struct)"),
    0x08007634: (0xF7FD481A, "ldr r0, [pc, #0x68] / bl #0x8004ee0 hw1 (Load GPIOB & call GPIO_Init)"),

    # --- BLOCK 36 (0x08007800 - 0x08007834): Event Queue Upsert / Ring Buffer Indexing ---
    0x08007800: (0x1700EB00, "add.w r7, r0, r0, lsl #4 (Queue stride multiplier x17)"),
    0x08007804: (0x1780EB07, "add.w r7, r7, r0, lsl #6 (Queue stride multiplier x81)"),
    0x0800781C: (0x7017F81C, "ldrb.w r7, [ip, r7, lsl #1] (Queue head descriptor reload)"),
    0x08007820: (0x0680F007, "and r6, r7, #0x80 (Extract queue full flag)"),
    0x08007824: (0x1700EB00, "add.w r7, r0, r0, lsl #4 (Tail offset stride x17)"),
    0x08007828: (0x1780EB07, "add.w r7, r7, r0, lsl #6 (Tail offset stride x81)"),
    0x08007834: (0xB90E037F, "and.w r3 (hw2) / cbnz r6, #0x800783c (Extract tail idx & branch if full)"),

    # --- BLOCK 37 (0x08007A00 - 0x08007A34): Ring Buffer Prepend / Circular Buffer Wrap ---
    0x08007A00: (0x2227B90A, "cbnz r2, #0x8007a06 / movs r2, #0x27 (Tail index modulo 40 decrement)"),
    0x08007A04: (0x1E56E001, "b #0x8007a0a / subs r6, r2, #1 (Tail index decrement path)"),
    0x08007A1C: (0x9F000681, "add.w r6 (hw2) / ldr r7, [sp] (Load enqueue item from stack)"),
    0x08007A20: (0x42916037, "str r7, [r6] / cmp r1, r2 (Store item & check head == tail)"),
    0x08007A24: (0x2501D100, "bne #0x8007a28 / movs r5, #1 (Set full flag if wrapped)"),
    0x08007A28: (0xF041B10D, "cbz r5, #0x8007a2e / orr r1, r1, #0x80 (hw1) (Set full bit)"),
    0x08007A34: (0x4F371680, "add.w r6 (hw2) / ldr r7, [pc, #0xdc] (Load queue base ptr)"),

    # --- BLOCK 38 (0x08007C00 - 0x08007C34): Clock Latency, Sleep Timeout & Power State Jump Table ---
    0x08007C00: (0x2002FE31, "bl #0x8004864 hw2 / movs r0, #2 (FLASH_SetLatency 2 & prep PLL)"),
    0x08007C04: (0xFDA7F7FD, "bl #0x8005756 (RCC_SYSCLKConfig PLL)"),
    0x08007C1C: (0x3188F241, "movw r1, #0x1388 (Load 5000ms timeout threshold)"),
    0x08007C20: (0xDA014288, "cmp r0, r1 / bge #0x8007c28 (Check sleep timer < 5000ms)"),
    0x08007C24: (0x47702001, "movs r0, #1 / bx lr (Active timer return 1)"),
    0x08007C28: (0xE7FC2000, "movs r0, #0 / b #0x8007c26 (Expired timer return 0)"),
    0x08007C34: (0xE8DFD86C, "bhi #0x8007d10 / tbb [pc, r0] hw1 (State bound check & jump table)"),

    # --- BLOCK 39 (0x08007E00 - 0x08007E34): USART1 TX Ring Buffer Dequeue & Dispatch ---
    0x08007E00: (0xDD172800, "cmp r0, #0 / ble #0x8007e34 (Check tx_len == 0 & branch empty)"),
    0x08007E04: (0x0130F894, "ldrb.w r0, [r4, #0x130] (Reload tx_len for decrement)"),
    0x08007E1C: (0x2132F894, "ldrb.w r2, [r4, #0x132] (Reload tail index)"),
    0x08007E20: (0xF8841C50, "adds r0, r2, #1 / strb.w r0, [r4, #0x132] hw1 (Increment tail index)"),
    0x08007E24: (0xF2040132, "strb.w r0, [r4, #0x132] hw2 / addw r0, r4, #0x133 hw1 (Buffer base offset)"),
    0x08007E28: (0x5C811033, "addw r0 hw2 / ldrb r1, [r0, r2] (Load byte from buffer[tail])"),
    0x08007E34: (0x0131F894, "ldrb.w r0, [r4, #0x131] (Empty FIFO: load head index to reset tail)"),

    # --- BLOCK 40 (0x08008000 - 0x08008034): GPIOA PA3 & USART2 Peripheral Configuration ---
    0x08008000: (0xFF6EF7FC, "bl #0x8004ee0 (GPIO_Init for PA3 RX floating input)"),
    0x08008004: (0x04482101, "movs r1, #1 / lsls r0, r1, #17 (Prep APB1_USART2 0x20000 & ENABLE)"),
    0x0800801C: (0x0008F8AD, "strh.w r0, [sp, #8] (USART_InitStruct.USART_Parity = No)"),
    0x08008020: (0x000CF8AD, "strh.w r0, [sp, #0xc] (USART_InitStruct.USART_HardwareFlowControl = None)"),
    0x08008024: (0xF8AD200C, "movs r0, #0xc / strh.w r0, [sp, #0xa] hw1 (USART_Mode = Rx|Tx)"),
    0x08008028: (0x4669000A, "strh.w r0, [sp, #0xa] hw2 / mov r1, sp (Store Mode & pass &USART_InitStruct)"),
    0x08008034: (0x5125F240, "movw r1, #0x525 (USART_IT_RXNE interrupt configuration)"),

    # --- BLOCK 41 (0x08008200 - 0x08008234): Packet Queue Ring Dequeue & USART2 Space Helpers ---
    0x08008200: (0x78607060, "strb r0, [r4, #1] / ldrb r0, [r4, #1] (Store tail & reload tail)"),
    0x08008204: (0xDB01280A, "cmp r0, #10 / blt #0x800820c (Check tail < 10 modulo wrap)"),
    0x0800821C: (0xF890482E, "ldr r0, [pc, #0xb8] / ldrb.w r0, [r0, #0xf4] hw1 (UART2 buffer count)"),
    0x08008220: (0xB90800F4, "ldrb.w hw2 / cbnz r0, #0x8008228 (Check count == 0)"),
    0x08008224: (0x47702001, "movs r0, #1 / bx lr (Return 1 if empty)"),
    0x08008228: (0xE7FC2000, "movs r0, #0 / b #0x8008226 (Return 0 if not empty)"),
    0x08008234: (0xB2C000FA, "rsb.w hw2 (#0xfa) / uxtb r0, r0 (Compute 250 - count free space)"),

    # --- BLOCK 42 (0x08008400 - 0x08008434): USART1 Protocol DLE/STX Parser State Machine ---
    0x08008400: (0x49A82001, "movs r0, #1 / ldr r1, [pc, #0x2a0] (State 0: DLE matched -> state = 1)"),
    0x08008404: (0x78207008, "strb r0, [r1] / ldrb r0, [r4] (Write state 1 & load packet head slot)"),
    0x0800841C: (0x700849A1, "ldr r1, [pc, #0x284] / strb r0, [r1] (State 0 mismatch: state = 0)"),
    0x08008420: (0xE09F7160, "strb r0, [r4, #5] / b #0x8008564 (Clear checksum & exit to loop end)"),
    0x08008424: (0xD10D2D02, "cmp r5, #2 / bne #0x8008444 (State 1: check STX 0x02 header byte)"),
    0x08008428: (0x499E2002, "movs r0, #2 / ldr r1, [pc, #0x278] (State 1: STX matched -> state = 2)"),
    0x08008434: (0xEB001DA0, "adds r0, r4, #6 / add.w r0, r0, r1, lsl #1 hw1 (Compute packet[slot] address)"),

    # --- BLOCK 43 (0x08008600 - 0x08008634): UART3 / CAN FIFO Queue Packet Sender ---
    0x08008600: (0xB9154C25, "ldr r4, [pc, #0x94] / cbnz r5, #0x800860a (Load buffer & check len != 0)"),
    0x08008604: (0xE8BD2004, "movs r0, #4 / pop.w {r4, r5, r6, r7, r8, pc} hw1 (Return 4 if len == 0)"),
    0x0800861C: (0xF814E015, "b #0x800864a / ldrb r0, [r4, #0xf6]! hw1 (Branch to loop condition & entry)"),
    0x08008620: (0x1C400FF6, "ldrb hw2 / adds r0, r0, #1 (Increment queue count)"),
    0x08008624: (0x78607020, "strb r0, [r4] / ldrb r0, [r4, #1] (Store count & load head index)"),
    0x08008628: (0x28FA3CF6, "subs r4, #0xf6 / cmp r0, #0xfa (Restore r4 & check head < 250)"),
    0x08008634: (0x1B01F816, "ldrb r1, [r6], #1 (Fetch payload byte from user buffer)"),

    # --- BLOCK 44 (0x08008800 - 0x08008834): Vehicle State Switch & Hardware Dispatch ---
    0x08008800: (0xF004BF00, "nop / and.w r0, r4, #3 hw1 (Pad Case 3 & Case 2 entry and.w)"),
    0x08008804: (0x28020003, "and.w hw2 / cmp r0, #2 (Case 2 condition test)"),
    0x0800881C: (0xF0022000, "movs r0, #0 / bl #0x800b5e4 hw1 (Case 2 audio/display call 1)"),
    0x08008820: (0xF001FEE1, "bl #0x800b5e4 hw2 / bl #0x8009dac hw1 (Case 2 call 2)"),
    0x08008824: (0xF001FAC3, "bl #0x8009dac hw2 / bl #0x800a4ae hw1 (Case 2 call 3)"),
    0x08008828: (0xBF00FE42, "bl #0x800a4ae hw2 / nop (Case 2 call 3 complete & alignment nop)"),
    0x08008834: (0xF890484C, "ldr r0, [pc, #0x130] / ldrb.w r0, [r0, #0x36] hw1 (Case 1 state check)"),

    # --- BLOCK 45 (0x08008A00 - 0x08008A34): NVIC Interrupt Priority Configuration Table ---
    0x08008A00: (0xF88D2007, "movs r0, #7 / strb.w r0, [sp] hw1 (NVIC EXTI1_IRQn setup)"),
    0x08008A04: (0x20020000, "strb.w hw2 / movs r0, #2 (Preemption priority = 2, Sub = 2)"),
    0x08008A1C: (0xF88D2002, "movs r0, #2 / strb.w r0, [sp, #1] hw1 (NVIC EXTI2_IRQn Preemption = 2)"),
    0x08008A20: (0x20030001, "strb.w hw2 / movs r0, #3 (NVIC EXTI2_IRQn SubPriority = 3)"),
    0x08008A24: (0x0002F88D, "strb.w r0, [sp, #2] (Store SubPriority = 3)"),
    0x08008A28: (0xF7FA4668, "mov r0, sp / bl #0x80035e2 hw1 (NVIC_Init call for EXTI2)"),
    0x08008A34: (0xF88D2002, "movs r0, #2 / strb.w r0, [sp, #1] hw1 (NVIC EXTI3_IRQn Preemption = 2)"),

    # --- BLOCK 46 (0x08008C00 - 0x08008C34): USART3 Initialization & Transmit Task Prologue ---
    0x08008C00: (0x5125F240, "movw r1, #0x525 (USART3_IT_RXNE interrupt configuration)"),
    0x08008C04: (0xF7FD48A8, "ldr r0, [pc, #0x2a0] / bl #0x8005c7a hw1 (USART3 ITConfig)"),
    0x08008C1C: (0xB005F822, "bl #0x8005c62 hw2 / add sp, #0x14 (USART_Cmd complete & stack teardown)"),
    0x08008C20: (0xB510BD00, "pop {pc} / push {r4, lr} (Exit USART3_Init & enter USART3_Disable)"),
    0x08008C24: (0x48A02100, "movs r1, #0 / ldr r0, [pc, #0x280] (USART3 disable arguments)"),
    0x08008C28: (0xF81BF7FD, "bl #0x8005c62 (USART_Cmd(USART3, DISABLE))"),
    0x08008C34: (0x489C2140, "movs r1, #0x40 / ldr r0, [pc, #0x270] (Check USART3_FLAG_TC)"),

    # --- BLOCK 47 (0x08008E00 - 0x08008E34): UART4 / CAN2 FIFO Queue Packet Sender ---
    0x08008E00: (0x460D4606, "mov r6, r0 / mov r5, r1 (Save packet buf ptr & byte length)"),
    0x08008E04: (0xB9154C25, "ldr r4, [pc, #0x94] / cbnz r5, #0x8008e0e (Load FIFO & check len != 0)"),
    0x08008E1C: (0x4607FFE0, "bl #0x8008dde hw2 / mov r7, r0 (Get write ptr & save result)"),
    0x08008E20: (0xF814E015, "b #0x8008e4e / ldrb r0, [r4, #0xf4]! hw1 (Branch to loop test & entry)"),
    0x08008E24: (0x1C400FF4, "ldrb hw2 / adds r0, r0, #1 (Increment queue byte counter)"),
    0x08008E28: (0x78607020, "strb r0, [r4] / ldrb r0, [r4, #1] (Store counter & load head index)"),
    0x08008E34: (0x00F5F884, "strb.w r0, [r4, #0xf5] (Wrap head index to 0 modulo 250)"),

    # --- BLOCK 48 (0x08009000 - 0x08009034): CAN1 Filter 1 & CAN2 Filter 14 Hardware Setup ---
    0x08009000: (0x0006F8AD, "strh.w r0, [sp, #6] (Filter 1: store CAN_FilterIdLow)"),
    0x08009004: (0x6B0048BC, "ldr r0, [pc, #0x2f0] / ldr r0, [r0, #0x30] (Filter 1: load CAN ID 6 from table)"),
    0x0800901C: (0xF8AD2000, "movs r0, #0 / strh.w r0, [sp, #0xc] hw1 (Filter 1: assign FIFO 0)"),
    0x08009020: (0x2001000C, "strh.w r0, [sp, #0xc] hw2 / movs r0, #1 (Filter 1: activation = ENABLE)"),
    0x08009024: (0x0011F88D, "strb.w r0, [sp, #0x11] (Filter 1: store CAN_FilterActivation)"),
    0x08009028: (0xF7FAA801, "add r0, sp, #4 / bl #0x8003c4a hw1 (Filter 1: CAN_FilterInit call)"),
    0x08009034: (0xF88D2000, "movs r0, #0 / strb.w r0, [sp, #0xf] hw1 (Filter 14: CAN_FilterMode = IdMask)"),

    # --- BLOCK 49 (0x08009200 - 0x08009234): CAN 0x025 Steering Angle Sensor Decoder ---
    0x08009200: (0x46042000, "and.w r0, r1, r0, lsl #8 hw2 / mov r4, r0 (Save high byte shifted)"),
    0x08009204: (0x86884941, "ldr r1, [pc, #0x104] / strh r0, [r1, #0x34] (Store shifted byte to angle)"),
    0x0800921C: (0xF6404604, "mov r4, r0 / movw r1, #0xfff hw1 (Save combined word & prep mask)"),
    0x08009220: (0xEA0171FF, "movw r1, #0xfff hw2 / and.w r0, r1, r4 hw1 (Mask angle with 0x0FFF)"),
    0x08009224: (0x49390004, "and.w hw2 / ldr r1, [pc, #0xe4] (Load &g_VehicleState pointer)"),
    0x08009228: (0x46048688, "strh r0, [r1, #0x34] / mov r4, r0 (Store 12-bit angle to state)"),
    0x08009234: (0x49310080, "add.w r0, r0, r0, lsl #2 hw2 / ldr r1, [pc, #0xc4] (Compute msg address)"),

    # --- BLOCK 50 (0x08009400 - 0x08009434): CAN 0x396 Parking Radar Distance Decode ---
    0x08009400: (0xF89D0011, "strb.w r0, [sp, #0x11] hw2 / ldrb.w r0, [sp, #0x11] hw1 (RL raw save & reload)"),
    0x08009404: (0xF7FF0011, "ldrb.w hw2 / bl #0x80093a6 hw1 (Call map_radar_distance for RL)"),
    0x0800941C: (0x7B400080, "add.w hw2 / ldrb r0, [r0, #0xd] (Compute msg address & load Data[2])"),
    0x08009420: (0xF88D0900, "lsrs r0, r0, #4 / strb.w r0, [sp, #0x12] hw1 (Extract high nibble RC & store raw)"),
    0x08009424: (0xF89D0012, "strb.w hw2 / ldrb.w r0, [sp, #0x12] hw1 (Store complete & reload RC for map)"),
    0x08009428: (0xF7FF0012, "ldrb.w hw2 / bl #0x80093a6 hw1 (Call map_radar_distance for RC)"),
    0x08009434: (0x012DF890, "ldrb.w r0, [r0, #0x12d] (Load tail index for RR distance decode)"),

    # --- BLOCK 51 (0x08009600 - 0x08009634): CAN 0x622 Body / Headlights Handler & SWC Task ---
    0x08009600: (0x7048492F, "ldr r1, [pc, #0xbc] / strb r0, [r1, #1] (Store lights on state)"),
    0x08009604: (0x2000E002, "b #0x800960c / movs r0, #0 (Branch over lights off store)"),
    0x0800961C: (0x7008492B, "ldr r1, [pc, #0xac] / strb r0, [r1] (Update prev_lights state)"),
    0x08009620: (0x21012200, "movs r2, #0 / movs r1, #1 (Send CMD 0x01 lights update)"),
    0x08009624: (0xF7FE4610, "mov r0, r2 / bl #0x8007764 hw1 (Call uart_send_composite_status)"),
    0x08009628: (0xBD10F89D, "bl #0x8007764 hw2 / pop {r4, pc} (Complete call & function return)"),
    0x08009634: (0x4822BD10, "pop {r4, pc} / ldr r0, [pc, #0x88] (Function exit & reload state ptr)"),

    # --- BLOCK 52 (0x08009800 - 0x08009834): CAN_WaitFlag & Multi-Step Transfer Engine ---
    0x08009800: (0x4620D0F2, "beq #0x80097e8 / mov r0, r4 (Loop back or return status)"),
    0x08009804: (0xE8BDB2C0, "uxtb r0, r0 / pop.w {r4, r5, r6, r7, r8, pc} hw1 (Return from wait)"),
    0x0800981C: (0x49B0FD6A, "bl #0x80052f2 hw2 / ldr r1, [pc, #0x2c0] (Flag literal load)"),
    0x08009820: (0xF7FF4638, "mov r0, r7 / bl #0x80097d8 hw1 (Call CAN_WaitFlag)"),
    0x08009824: (0xF04FFFD9, "bl #0x80097d8 hw2 / mov.w sb, #0 hw1 (Init transfer step = 0)"),
    0x08009828: (0x28000900, "mov.w sb, #0 hw2 / cmp r0, #0 (Check wait result)"),
    0x08009834: (0x21012600, "movs r6, #0 / movs r1, #1 (Init err=0 & prep next step)"),

    # --- BLOCK 53 (0x08009A00 - 0x08009A34): CAN Transfer State Machine Cases 0 & 1 ---
    0x08009A00: (0x2701B908, "cbnz r0, #0x8009a06 / movs r7, #1 (Case 0 wait check & error code)"),
    0x08009A04: (0xF04FE0A4, "b #0x8009b50 / mov.w r8, #1 hw1 (Branch to exit / advance step 1)"),
    0x08009A1C: (0x46512200, "movs r2, #0 / mov r1, sl (Case 1 setup: arg2=0, arg1=sl)"),
    0x08009A20: (0xF7FB4648, "mov r0, sb / bl #0x800539e hw1 (Call fn_539e hw1)"),
    0x08009A24: (0x492FFCBC, "bl #0x800539e hw2 / ldr r1, [pc, #0xbc] (Complete call & load literal 0x08009AE4)"),
    0x08009A28: (0xF7FF4648, "mov r0, sb / bl #0x80097d8 hw1 (Call CAN_WaitFlag hw1)"),
    0x08009A34: (0x4648492C, "ldr r1, [pc, #0xb0] / mov r0, sb (Load literal 0x08009AE8 & prep fn_5532)"),

    # --- BLOCK 54 (0x08009C00 - 0x08009C34): CAN Multi-Byte Read State Machine Cases 2 & 3 ---
    0x08009C00: (0xFDEAF7FF, "bl #0x80097d8 (Case 2: CAN_WaitFlag call)"),
    0x08009C04: (0x2703B908, "cbnz r0, #0x8009c0a / movs r7, #3 (Case 2: check wait & error code 3)"),
    0x08009C1C: (0xF04FFC8A, "bl #0x8005532 hw2 / mov.w r8, #3 hw1 (Complete write & advance step 3)"),
    0x08009C20: (0xBF000803, "mov.w r8, #3 hw2 / nop (Complete step 3 advance)"),
    0x08009C24: (0x4648B2E9, "uxtb r1, r5 / mov r0, sb (Case 3: zero-extend low addr byte & prep call)"),
    0x08009C28: (0xFBB3F7FB, "bl #0x8005392 (Case 3: send low addr byte)"),
    0x08009C34: (0xB908FDD1, "bl #0x80097d8 hw2 / cbnz r0, #0x8009c3c (Case 3: wait & branch on success)"),

    # --- BLOCK 55 (0x08009E00 - 0x08009E34): Steering Wheel Angle Sensor Normalization & Scaling ---
    0x08009E00: (0x49D33850, "subs r0, #0x50 / ldr r1, [pc, #0x34c] (Range 3 offset & load &g_Steer)"),
    0x08009E04: (0xE0048148, "strh r0, [r1, #0xa] / b #0x8009e12 (Store normalized angle & branch to clamp)"),
    0x08009E1C: (0x7048F44F, "mov.w r0, #0x320 (Angle clamp to 800)"),
    0x08009E20: (0x814849CB, "ldr r1, [pc, #0x32c] / strh r0, [r1, #0xa] (Store clamped angle)"),
    0x08009E24: (0x788148C9, "ldr r0, [pc, #0x324] / ldrb r1, [r0, #2] (Load raw buffer & angle 2 low byte)"),
    0x08009E28: (0xF36178C0, "ldrb r0, [r0, #3] / bfi r0, r1, #2, #0x1e hw1 (Load angle 2 high & pack hw1)"),
    0x08009E34: (0xB3208980, "ldrh r0, [r0, #0xc] / cbz r0, #0x8009e82 (Check angle 2 zero & branch)"),

    # --- BLOCK 56 (0x0800A000 - 0x0800A034): Steering Direction Edge Detector & Status Packet ---
    0x0800A000: (0xF5C08980, "ldrh r0, [r0, #0xc] / rsb.w r0, r0, #0x1e0 hw1 (Invert angle 2 hw1)"),
    0x0800A004: (0xB28170F0, "rsb.w hw2 / uxth r1, r0 (Invert angle 2 & extend)"),
    0x0800A01C: (0x484C2101, "movs r1, #1 / ldr r0, [pc, #0x130] (Steering right edge detect)"),
    0x0800A020: (0x20007381, "strb r1, [r0, #0xe] / movs r0, #0 (Update steer status = 1)"),
    0x0800A024: (0x80084945, "ldr r1, [pc, #0x114] / strh r0, [r1] (Reset timeout counter 0x20000076)"),
    0x0800A028: (0x7B824849, "ldr r0, [pc, #0x124] / ldrb r2, [r0, #0xe] (Load status for packet)"),
    0x0800A034: (0x4846E00E, "b #0x800a054 / ldr r0, [pc, #0x118] (Skip branch 2 / steer left load)"),

    # --- BLOCK 57 (0x0800A200 - 0x0800A234): Steering Periodic Task Init Delay State Machine ---
    0x0800A200: (0xE05EDA00, "bge #0x800a204 / b #0x800a2c2 (Case 1: timer >= 200 check)"),
    0x0800A204: (0xFE86F7FF, "bl #0x8009f14 (Case 1: send CAN init commands)"),
    0x0800A21C: (0x485BE051, "b #0x800a2c2 / ldr r0, [pc, #0x16c] (Case 2: timer branch & load &g_Steer)"),
    0x0800A220: (0x1C407C80, "ldrb r0, [r0, #0x12] / adds r0, r0, #1 (Case 2: advance state to 3)"),
    0x0800A224: (0x74884959, "ldr r1, [pc, #0x164] / strb r0, [r1, #0x12] (Case 2: store state 3)"),
    0x0800A228: (0x485AE04B, "b #0x800a2c2 / ldr r0, [pc, #0x168] (Case 2 exit / Case 3 load timer)"),
    0x0800A234: (0x7C804855, "ldr r0, [pc, #0x154] / ldrb r0, [r0, #0x12] (Case 3: load &g_Steer & state)"),





    # --- BLOCK 58 (0x0800A400 - 0x0800A434): CAN Aux/HVAC Periodic Task State Machine ---
    0x0800A400: (0x48B8F9B1, "bl #0x8007764 hw2 / ldr r0, [pc, #0x2e0] (Case 1: complete call & load state ptr)"),
    0x0800A404: (0x1C407DC0, "ldrb r0, [r0, #0x17] / adds r0, r0, #1 (Case 1: advance state to 2)"),
    0x0800A41C: (0x1C407DC0, "ldrb r0, [r0, #0x17] / adds r0, r0, #1 (Case 2: advance state to 3)"),
    0x0800A420: (0x75C849B0, "ldr r1, [pc, #0x2c0] / strb r0, [r1, #0x17] (Case 2: store state 3)"),
    0x0800A424: (0x48B0E022, "b #0x800a46c / ldr r0, [pc, #0x2c0] (Case 2 exit / Case 3 load timer 0x2000007E)"),
    0x0800A428: (0x28C88800, "ldrh r0, [r0] / cmp r0, #0xc8 (Case 3: timer >= 200 ms check)"),
    0x0800A434: (0x780949AE, "ldr r1, [pc, #0x2b8] / ldrb r1, [r1] (Case 3: load prev val 0x20000080)"),

    # --- BLOCK 59 (0x0800A600 - 0x0800A634): Audio Mute Restore & Power Control Handler ---
    0x0800A600: (0xFB1AF7FC, "bl #0x8006c38 (Handler 1: restore mute_control state from 0x20001016)"),
    0x0800A604: (0xB510BD70, "pop {r4, r5, r6, pc} / push {r4, lr} (Handler 1 exit / Handler 2 entry 0x0800A607)"),
    0x0800A61C: (0x28017800, "ldrb r0, [r0] / cmp r0, #1 (Handler 2: check g_PowerState == 1)"),
    0x0800A620: (0x4930D1FA, "bne #0x800a618 / ldr r1, [pc, #0xc0] (Handler 2: exit on inactive & load &g_Audio)"),
    0x0800A624: (0xB2E4700C, "strb r4, [r1] / uxtb r4, r4 (Handler 2: store g_Audio[0]=power & zero-extend)"),
    0x0800A628: (0xD0F22C00, "cmp r4, #0 / beq #0x800a612 (Handler 2: if power==0, branch to mute & exit)"),
    0x0800A634: (0x814860FA, "mov.w r0, #0x7d0 hw2 / strh r0, [r1, #0xa] (Handler 2: 2000 ms unmute delay store)"),

    # --- BLOCK 60 (0x0800A800 - 0x0800A834): UART TX Checksum & HVAC/Radar Telemetry Preamble ---
    0x0800A800: (0xF80048C3, "ldr r0, [pc, #0x30c] / strb.w r8, [r0, r5] hw1 (Load tx_buf & store checksum hw1)"),
    0x0800A804: (0x1C688005, "strb.w hw2 / adds r0, r5, #1 (Store checksum hw2 & increment total len)"),
    0x0800A81C: (0xF7FC2003, "movs r0, #3 / bl #0x8007764 hw1 (Status 3 broadcast: arg0=3 & call hw1)"),
    0x0800A820: (0xBD10FFA1, "bl #0x8007764 hw2 / pop {r4, pc} (Complete status 3 call & return)"),
    0x0800A824: (0x2500B57C, "push {r2, r3, r4, r5, r6, lr} / movs r5, #0 (HVAC/radar reporter entry 0x0800A825)"),
    0x0800A828: (0xBF002600, "movs r6, #0 / nop (Zero r6 & 4-byte align payload buffer)"),
    0x0800A834: (0x0000F89D, "ldrb.w r0, [sp] (Load payload[0] for base flag OR 0x03)"),

    # --- BLOCK 61 (0x0800AA00 - 0x0800AA34): Rear Radar Channels 1 & 2 Bit-Packing ---
    0x0800AA00: (0x0005F89D, "ldrb.w r0, [sp, #5] (Channel 1: load payload[5] for level 3 OR)"),
    0x0800AA04: (0x000CF040, "orr r0, r0, #0xc (Channel 1: pack level 3 bits [3:2])"),
    0x0800AA1C: (0xE0070005, "strb.w r0, [sp, #5] hw2 / b #0x800aa30 (Store level 2 & branch to next ch)"),
    0x0800AA20: (0xDA052C0C, "cmp r4, #0xc / bge #0x800aa30 (Channel 1: level 1 threshold check)"),
    0x0800AA24: (0x0005F89D, "ldrb.w r0, [sp, #5] (Channel 1: load payload[5] for level 1 OR)"),
    0x0800AA28: (0x0004F040, "orr r0, r0, #4 (Channel 1: pack level 1 bits [3:2])"),
    0x0800AA34: (0xDA062C05, "cmp r4, #5 / bge #0x800aa46 (Channel 2: level 3 threshold check)"),

    # --- BLOCK 62 (0x0800AC00 - 0x0800AC34): SysTick Twin Driver Handlers & Wrap Counter ---
    0x0800AC00: (0x0007F000, "and.w r0, r0, #7 (fn_ab90: wrap counter modulo 8)"),
    0x0800AC04: (0xBD107008, "strb r0, [r1] / pop {r4, pc} (fn_ab90: store counter & exit)"),
    0x0800AC1C: (0x7008496F, "ldr r1, [pc, #0x1bc] / strb r0, [r1] (fn_ac08: load &0x20000087 & store masked val)"),
    0x0800AC20: (0x7801486C, "ldr r0, [pc, #0x1b0] / ldrb r1, [r0] (fn_ac08: load &0x20000089 & bit shift)"),
    0x0800AC24: (0x40882001, "movs r0, #1 / lsls r0, r1 (fn_ac08: shift mask)"),
    0x0800AC28: (0x00FFF080, "eor.w r0, r0, #0xff (fn_ac08: invert mask)"),
    0x0800AC34: (0xF7FC7008, "strb r0, [r1] / bl #0x8006ff4 hw1 (fn_ac08: store masked & call fn_6ff4 hw1)"),

    # --- BLOCK 63 (0x0800AE00 - 0x0800AE34): Periodic SysTick Countdown Timers Service ---
    0x0800AE00: (0xB1208800, "ldrh r0, [r0] / cbz r0, #0x800ae0e (Timer 2: read & zero check)"),
    0x0800AE04: (0x88004898, "ldr r0, [pc, #0x260] / ldrh r0, [r0] (Timer 2: reload ptr 0x20000096)"),
    0x0800AE1C: (0x48948008, "strh r0, [r1] / ldr r0, [pc, #0x250] (Timer 3: store & Timer 4 ptr 0x2000009A)"),
    0x0800AE20: (0xB1208800, "ldrh r0, [r0] / cbz r0, #0x800ae2e (Timer 4: read & zero check)"),
    0x0800AE24: (0x88004892, "ldr r0, [pc, #0x248] / ldrh r0, [r0] (Timer 4: reload ptr 0x2000009A)"),
    0x0800AE28: (0x49911E40, "subs r0, r0, #1 / ldr r1, [pc, #0x244] (Timer 4: decrement & store ptr)"),
    0x0800AE34: (0x8800488F, "ldr r0, [pc, #0x23c] / ldrh r0, [r0] (Timer 5: reload ptr 0x2000009C)"),

    # --- BLOCK 64 (0x0800B000 - 0x0800B034): Multi-Channel Event State Machine & Timer Checks ---
    0x0800B000: (0x8800481A, "ldr r0, [pc, #0x68] / ldrh r0, [r0] (Channel 3: load Timer 3 0x20000098 & read)"),
    0x0800B004: (0x2024B118, "cbz r0, #0x800b00e / movs r0, #0x24 (Channel 3: check timer & set action 0x24)"),
    0x0800B01C: (0xB1108800, "ldrh r0, [r0] / cbz r0, #0x800b026 (Timer 3 fallback: read & branch to Ch 4)"),
    0x0800B020: (0x49162024, "movs r0, #0x24 / ldr r1, [pc, #0x58] (Timer 3 fallback: action 0x24 & load &g_State)"),
    0x0800B024: (0x48147008, "strb r0, [r1] / ldr r0, [pc, #0x50] (Store state & Channel 4 load 0x20001088)"),
    0x0800B028: (0xB14069C0, "ldr r0, [r0, #0x1c] / cbz r0, #0x800b03e (Channel 4: check offset 0x1c & skip)"),
    0x0800B034: (0xB9108800, "ldrh r0, [r0] / cbnz r0, #0x800b03e (Channel 4: check Timer 4 & branch on active)"),

    # --- BLOCK 65 (0x0800B200 - 0x0800B234): I2C2 Peripheral Deinit & Slave Event Dispatcher ---
    0x0800B200: (0x48A161E0, "mov.w r1, #0x700 hw2 / ldr r0, [pc, #0x284] (I2C2 ITConfig mask & I2C2 base load)"),
    0x0800B204: (0xF8BCF7FA, "bl #0x8005380 (I2C_ITConfig(I2C2, 0x700, DISABLE))"),
    0x0800B21C: (0x1A814A9B, "ldr r2, [pc, #0x26c] / subs r1, r0, r2 (Load I2C_EVENT_SLAVE_BYTE_RECEIVED & delta)"),
    0x0800B220: (0xD0194290, "cmp r0, r2 / beq #0x800b258 (Check byte received & branch to handler 0x800b258)"),
    0x0800B224: (0x2810DC08, "bgt #0x800b238 / cmp r0, #0x10 (Branch to switch table & check STOP condition)"),
    0x0800B228: (0xF5B0D05C, "beq #0x800b2e4 / cmp.w r0, #0x400 hw1 (Branch to STOP handler & AF check hw1)"),
    0x0800B234: (0x4601D00A, "beq #0x800b24c / mov r1, r0 (Branch to ADDR matched handler & setup switch r1)"),

    # --- BLOCK 66 (0x0800B400 - 0x0800B434): I2C2 Slave Event Handlers (STOP & AF Recovery) ---
    0x0800B400: (0x4821492A, "ldr r1, [pc, #0xa8] / ldr r0, [pc, #0x84] (Load STOP flag mask & I2C2 base)"),
    0x0800B404: (0xF895F7FA, "bl #0x8005532 (I2C_ClearFlag(I2C2, STOPF))"),
    0x0800B41C: (0x49248008, "strh r0, [r1] / ldr r1, [pc, #0x90] (Reset counter & load AF mask 0x10000400)"),
    0x0800B420: (0xF7FA4819, "ldr r0, [pc, #0x64] / bl #0x8005532 hw1 (Load I2C2 base & clear AF hw1)"),
    0x0800B424: (0x4818F886, "bl #0x8005532 hw2 / ldr r0, [pc, #0x60] (Complete clear AF & load I2C2 base)"),
    0x0800B428: (0xFE8BF7FF, "bl #0x800b142 (Re-initialize I2C2 slave configuration)"),
    0x0800B434: (0xB510BD70, "pop {r4, r5, r6, pc} / push {r4, lr} (Function exit / Next function entry 0x0800B437)"),

    # --- BLOCK 67 (0x0800B600 - 0x0800B634): Mode Switch Table Case 3 & Packet Builder Header ---
    0x0800B600: (0x4A7B2102, "movs r1, #2 / ldr r2, [pc, #0x1ec] (Mode switch Case 3: set val 2 & load &g_Mode)"),
    0x0800B604: (0xE0037011, "strb r1, [r2] / b #0x800b610 (Store mode val 2 & branch to exit bx lr)"),
    0x0800B61C: (0x4974D1F9, "bne #0x800b612 / ldr r1, [pc, #0x1d0] (Check packet ready: exit if not & load 0x200000A0)"),
    0x0800B620: (0x29017849, "ldrb r1, [r1, #1] / cmp r1, #1 (Check state byte == 1)"),
    0x0800B624: (0x23FFD135, "bne #0x800b692 / movs r3, #0xff (Branch to State 2 & setup header byte 0xFF)"),
    0x0800B628: (0x1C424601, "mov r1, r0 / adds r2, r0, #1 (Index 0 prep & increment index r0)"),
    0x0800B634: (0x1C424601, "mov r1, r0 / adds r2, r0, #1 (Index 1 prep & increment index r0)"),

    # --- BLOCK 68 (0x0800B800 - 0x0800B834): IWDG Hardware Watchdog Init & Boot Auth Check ---
    0x0800B800: (0x2000025A, "Literal pointer to CAN RX message buffer 0x2000025A"),
    0x0800B804: (0xF245B510, "push {r4, lr} / movw r0, #0x5555 hw1 (IWDG_Init entry 0x0800B805 & write access key hw1)"),
    0x0800B81C: (0xFEB9F7F9, "bl #0x8005592 (IWDG_ReloadCounter: reload counter with 0xAAAA)"),
    0x0800B820: (0xFEBCF7F9, "bl #0x800559c (IWDG_Enable: start watchdog timer with 0xCCCC)"),
    0x0800B824: (0xB510BD10, "pop {r4, pc} / push {r4, lr} (IWDG_Init exit / Boot check_auth entry 0x0800B827)"),
    0x0800B828: (0xFFECF7FF, "bl #0x800b804 (check_auth: call IWDG_Init to arm watchdog)"),
    0x0800B834: (0xBF00D004, "beq #0x800b840 / nop (Signature match: jump to success return pop {r4, pc})"),

    # =========================================================================
    # BLOCK 69: CAN Dispatch Table & Master Audio Attenuation Curve (0x0800BA00 - 0x0800BA34)
    # =========================================================================
    0x0800BA00: (0x080091E1, "CAN ID 0x025 Steering Wheel Angle Handler (0x080091E0)"),
    0x0800BA04: (0x000001D0, "CAN ID 0x1D0 Transmission Gear Status (Handler 0x0800956B)"),
    0x0800BA1C: (0x00000622, "CAN ID 0x622 Body / Door Status & Camera Trigger (Handler 0x0800962D)"),
    0x0800BA20: (0x0800962D, "CAN ID 0x622 Secondary Camera / Door Trigger Handler (0x0800962C)"),
    0x0800BA24: (0x7FFFFFFF, "Master Audio Curve Step 0 [0x7FFFFF] (0 dB full scale)"),
    0x0800BA28: (0xE77FFFFF, "Master Audio Curve Step 1-2 [0x7FFFFF, 0x7FE720]"),
    0x0800BA34: (0xF57E904E, "Master Audio Curve Step 5-6 [0x7F4E90, 0x7EF590]"),

    # =========================================================================
    # BLOCK 70: Audio DSP Scale & Filter Table (0x0800BC00 - 0x0800BC34)
    # =========================================================================
    0x0800BC00: (0x00110000, "Audio DSP Scale Step [0x00, 0x11, 0x00, 0x00]"),
    0x0800BC04: (0x33000022, "Audio DSP Scale Step [0x22, 0x00, 0x00, 0x33]"),
    0x0800BC1C: (0x00000000, "Zero padding in DSP filter table"),
    0x0800BC20: (0x00000000, "Zero padding in DSP filter table"),
    0x0800BC24: (0x00000000, "Zero padding in DSP filter table"),
    0x0800BC28: (0x00000000, "Zero padding in DSP filter table"),
    0x0800BC34: (0x00000000, "Zero padding in DSP filter table"),

    # =========================================================================
    # BLOCK 71: Zero Padding Region (0x0800BE00 - 0x0800BE34)
    # =========================================================================
    0x0800BE00: (0x00000000, "Zero padding before coefficient tables"),
    0x0800BE04: (0x00000000, "Zero padding before coefficient tables"),
    0x0800BE1C: (0x00000000, "Zero padding before coefficient tables"),
    0x0800BE20: (0x00000000, "Zero padding before coefficient tables"),
    0x0800BE24: (0x00000000, "Zero padding before coefficient tables"),
    0x0800BE28: (0x00000000, "Zero padding before coefficient tables"),
    0x0800BE34: (0x00000000, "Zero padding before coefficient tables"),

    # =========================================================================
    # BLOCK 72: Q14 Unity-Gain Biquad Filter Array (0x0800C000 - 0x0800C034)
    # =========================================================================
    0x0800C000: (0x00000000, "Biquad filter coefficient zero padding"),
    0x0800C004: (0x00000000, "Biquad filter coefficient zero padding"),
    0x0800C01C: (0x00000000, "Biquad filter coefficient zero padding"),
    0x0800C020: (0x00000000, "Biquad filter coefficient zero padding"),
    0x0800C024: (0x00000000, "Biquad filter coefficient zero padding"),
    0x0800C028: (0x00000000, "Biquad filter coefficient zero padding"),
    0x0800C034: (0x00000000, "Biquad filter coefficient zero padding"),

    # =========================================================================
    # BLOCK 73: Periodic Coefficient Array [01 00 08] (0x0800C200 - 0x0800C234)
    # =========================================================================
    0x0800C200: (0x00010800, "Periodic sequence [00, 08, 01, 00]"),
    0x0800C204: (0x08000108, "Periodic sequence [08, 01, 00, 08]"),
    0x0800C21C: (0x08000108, "Periodic sequence [08, 01, 00, 08]"),
    0x0800C220: (0x01080001, "Periodic sequence [01, 00, 08, 01]"),
    0x0800C224: (0x00010800, "Periodic sequence [00, 08, 01, 00]"),
    0x0800C228: (0x08000108, "Periodic sequence [08, 01, 00, 08]"),
    0x0800C234: (0x08000108, "Periodic sequence [08, 01, 00, 08]"),

    # =========================================================================
    # BLOCK 74: Audio Filter Parameter Partition (0x0800C400 - 0x0800C434)
    # =========================================================================
    0x0800C400: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C404: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C41C: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C420: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C424: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C428: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C434: (0x00000000, "Zero padding in filter parameter table"),

    # =========================================================================
    # BLOCK 75: Audio Filter Parameter Partition (0x0800C600 - 0x0800C634)
    # =========================================================================
    0x0800C600: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C604: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C61C: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C620: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C624: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C628: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C634: (0x00000000, "Zero padding in filter parameter table"),

    # =========================================================================
    # BLOCK 76: Audio Filter Parameter Partition (0x0800C800 - 0x0800C834)
    # =========================================================================
    0x0800C800: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C804: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C81C: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C820: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C824: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C828: (0x00000000, "Zero padding in filter parameter table"),
    0x0800C834: (0x00000000, "Zero padding in filter parameter table"),

    # =========================================================================
    # BLOCK 77: Zero Padding Region (0x0800CA00 - 0x0800CA34)
    # =========================================================================
    0x0800CA00: (0x00000000, "Zero padding before data tables"),
    0x0800CA04: (0x00000000, "Zero padding before data tables"),
    0x0800CA1C: (0x00000000, "Zero padding before data tables"),
    0x0800CA20: (0x00000000, "Zero padding before data tables"),
    0x0800CA24: (0x00000000, "Zero padding before data tables"),
    0x0800CA28: (0x00000000, "Zero padding before data tables"),
    0x0800CA34: (0x00000000, "Zero padding before data tables"),

    # =========================================================================
    # BLOCK 78: 128-Point Equalization Curve (0x0800CC00 - 0x0800CC34)
    # =========================================================================
    0x0800CC00: (0x0B07330A, "Equalization curve Point 6-7 [0x0A3307, 0x0B...]"),
    0x0800CC04: (0x530DA6C3, "Equalization curve Point 7-8 [...C3A6, 0x0D53D0]"),
    0x0800CC1C: (0xBD196E33, "Equalization curve Point 15-16 [...336E, 0x19BD...]"),
    0x0800CC20: (0x3C471BD6, "Equalization curve Point 16-17 [...D6, 0x1B473C]"),
    0x0800CC24: (0x1E91CF1C, "Equalization curve Point 18-19 [0x1CCF91, 0x1E...]"),
    0x0800CC28: (0xDC1FCA56, "Equalization curve Point 19-20 [...56CA, 0x1FDCD0]"),
    0x0800CC34: (0xE8257067, "Equalization curve Point 23-24 [...6770, 0x25E840]"),

    # =========================================================================
    # BLOCK 79: Zero Padding Region (0x0800CE00 - 0x0800CE34)
    # =========================================================================
    0x0800CE00: (0x00000000, "Zero padding before literal tables"),
    0x0800CE04: (0x00000000, "Zero padding before literal tables"),
    0x0800CE1C: (0x00000000, "Zero padding before literal tables"),
    0x0800CE20: (0x00000000, "Zero padding before literal tables"),
    0x0800CE24: (0x00000000, "Zero padding before literal tables"),
    0x0800CE28: (0x00000000, "Zero padding before literal tables"),
    0x0800CE34: (0x00000000, "Zero padding before literal tables"),

    # =========================================================================
    # BLOCK 80: Zero Padding Region (0x0800D000 - 0x0800D034)
    # =========================================================================
    0x0800D000: (0x00000000, "Zero padding in calibration data partition"),
    0x0800D004: (0x00000000, "Zero padding in calibration data partition"),
    0x0800D01C: (0x00000000, "Zero padding in calibration data partition"),
    0x0800D020: (0x00000000, "Zero padding in calibration data partition"),
    0x0800D024: (0x00000000, "Zero padding in calibration data partition"),
    0x0800D028: (0x00000000, "Zero padding in calibration data partition"),
    0x0800D034: (0x00000000, "Zero padding in calibration data partition"),

    # =========================================================================
    # BLOCK 81: Factory Volume / Attenuation LUT (0x0800D200 - 0x0800D234)
    # =========================================================================
    0x0800D200: (0x39003838, "Volume level steps 38 and 39 [0x38, 0x38, 0x00, 0x39]"),
    0x0800D204: (0x40400039, "Volume level steps 39 and 40 [0x39, 0x00, 0x40, 0x40]"),
    0x0800D21C: (0x00000000, "Zero padding before BD37033 volume curve"),
    0x0800D220: (0x00000000, "Zero padding before BD37033 volume curve"),
    0x0800D224: (0x00000000, "Zero padding before BD37033 volume curve"),
    0x0800D228: (0xC6C9CCCF, "Attenuation table entry [0xCF, 0xCC, 0xC9, 0xC6] (BD37033 audio curve)"),
    0x0800D234: (0xABADAEAF, "Attenuation table entry [0xAF, 0xAE, 0xAD, 0xAB] (BD37033 audio curve)"),
}

def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def patch_factory_app(data: bytearray, base_address: int = 0x08003000) -> tuple[int, list[str]]:
    patched_count = 0
    log = []
    
    for addr, (expected_word, desc) in sorted(FACTORY_APP_PATCH_MAP.items()):
        offset = addr - base_address
        if offset < 0 or offset + 4 > len(data):
            log.append(f"[-] 0x{addr:08X}: outside dump bounds - skipped.")
            continue
        cur = struct.unpack_from("<I", data, offset)[0]
        if cur == expected_word:
            log.append(f"[=] 0x{addr:08X}: already authentic (0x{cur:08X}) | {desc}")
        else:
            struct.pack_into("<I", data, offset, expected_word)
            patched_count += 1
            log.append(f"[+] 0x{addr:08X}: patched 0x{cur:08X} -> 0x{expected_word:08X} | {desc}")
            
    return patched_count, log

def main():
    parser = argparse.ArgumentParser(description="Reconstruct stock factory Keil application from raw dump.")
    parser.add_argument("input_file", nargs="?", default="hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_app_52k_raw.bin")
    parser.add_argument("output_file", nargs="?", default="hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_app_52k_reconstructed.bin")
    parser.add_argument("--base", type=lambda x: int(x, 0), default=0x08003000, help="Base address")
    args = parser.parse_args()

    if not os.path.isfile(args.input_file):
        print(f"Error: {args.input_file} not found.", file=sys.stderr)
        return 1

    with open(args.input_file, "rb") as f:
        data = bytearray(f.read())

    print("=" * 72)
    print(" Factory Keil Application Silicon Reconstruction Patcher")
    print("=" * 72)
    print(f"Input:         {args.input_file} ({len(data)} bytes, {len(data)//4} words)")
    print(f"Input SHA256:  {hashlib.sha256(data).hexdigest()}")
    print("-" * 72)

    patched_count, log = patch_factory_app(data, args.base)
    for line in log:
        print(line)

    print("-" * 72)
    print(f"Patched {patched_count} / {len(FACTORY_APP_PATCH_MAP)} known factory silicon slots.")

    with open(args.output_file, "wb") as f:
        f.write(data)

    out_hash = hashlib.sha256(data).hexdigest()
    print(f"[+] Output:       {args.output_file} ({len(data)} bytes)")
    print(f"    Output SHA256: {out_hash}")
    print("=" * 72)
    return 0

if __name__ == "__main__":
    sys.exit(main())
