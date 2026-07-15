/*
 * Copyright(c) 2012 Hong Kong Applied Science and Technology
 * Research Institute Company Limited (ASTRI), all rights reserved
 * Proprietary and Confidential Information.
 *
 * This source file is the property of ASTRI, and may not be copied or
 * distributed in any isomorphic form without the prior written consent
 * of ASTRI.
 *
 * Name:
 *              ark_sys_regs.h
 *
 * Description:
 *              This file contains ark809 SOC system register definitions
 *
 * Author:
 *              Chan Man Chi
 *
 * Remarks:
 *
 */

#ifndef _ARK_SYS_REGS_H_
#define _ARK_SYS_REGS_H_

/* AHB system */
#define rSYS_BOOT_SAMPLE		*((volatile unsigned int *)(ARK1680_VA_SYS+0x0))
#define rSYS_CLK_SE			*((volatile unsigned int *)(ARK1680_VA_SYS+0x40))
#define rSYS_AHB_CLK_EN		*((volatile unsigned int *)(ARK1680_VA_SYS+0x44))
#define rSYS_APB_CLK_EN		*((volatile unsigned int *)(ARK1680_VA_SYS+0x48))
#define rSYS_AXI_CLK_EN		*((volatile unsigned int *)(ARK1680_VA_SYS+0x4c))
#define rSYS_PER_CLK_EN		*((volatile unsigned int *)(ARK1680_VA_SYS+0x50))
#define rSYS_LCD_CLK_CFG		*((volatile unsigned int *)(ARK1680_VA_SYS+0x54))
#define rSYS_SD_CLK_CFG		*((volatile unsigned int *)(ARK1680_VA_SYS+0x58))
#define rSYS_SD1_CLK_CFG		*((volatile unsigned int *)(ARK1680_VA_SYS+0x5c))
#define rSYS_DEVICE_CLK_CFG0	*((volatile unsigned int *)(ARK1680_VA_SYS+0x60))
#define rSYS_DEVICE_CLK_CFG1	*((volatile unsigned int *)(ARK1680_VA_SYS+0x64))
#define rSYS_DEVICE_CLK_CFG2	*((volatile unsigned int *)(ARK1680_VA_SYS+0x68))
#define rSYS_DEVICE_CLK_CFG3	*((volatile unsigned int *)(ARK1680_VA_SYS+0x6c))
#define rSYS_CLK_DLY_REG		*((volatile unsigned int *)(ARK1680_VA_SYS+0x70))
#define rSYS_SOFT_RSTNA		*((volatile unsigned int *)(ARK1680_VA_SYS+0x74))
#define rSYS_SOFT_RSTNB		*((volatile unsigned int *)(ARK1680_VA_SYS+0x78))
#define rSYS_SD2_CLK_CFG		*((volatile unsigned int *)(ARK1680_VA_SYS+0x7c))

#define rSYS_ANALOG_REG0		*((volatile unsigned int *)(ARK1680_VA_SYS+0x140))
#define rSYS_ANALOG_REG1		*((volatile unsigned int *)(ARK1680_VA_SYS+0x144))
#define rSYS_DDR2_PAD_REG	*((volatile unsigned int *)(ARK1680_VA_SYS+0x148))
#define rSYS_PLLRFCK_CTL		*((volatile unsigned int *)(ARK1680_VA_SYS+0x14c))
#define rSYS_CPUPLL_CFG		*((volatile unsigned int *)(ARK1680_VA_SYS+0x150))
#define rSYS_SYSPLL_CFG		*((volatile unsigned int *)(ARK1680_VA_SYS+0x154))
#define rSYS_AUDPLL_CFG		*((volatile unsigned int *)(ARK1680_VA_SYS+0x158))
#define rSYS_DDRDLL_RDCLK_CFG	*((volatile unsigned int *)(ARK1680_VA_SYS+0x15c))
#define rSYS_DDRDLL_WRCLK_CFG	*((volatile unsigned int *)(ARK1680_VA_SYS+0x160))
#define rSYS_DDRDLL_DQS_CFG0		*((volatile unsigned int *)(ARK1680_VA_SYS+0x164))
#define rSYS_DDRDLL_DQS_CFG1		*((volatile unsigned int *)(ARK1680_VA_SYS+0x168))
#define rSYS_DDRDLL_DQS_CFG2		*((volatile unsigned int *)(ARK1680_VA_SYS+0x16C))
#define rSYS_DDRDLL_BIAS_UP_TRIM	*((volatile unsigned int *)(ARK1680_VA_SYS+0x170))

#define rSYS_LVDS_CTRL_CFG	*((volatile unsigned int *)(ARK1680_VA_SYS+0x190))
#define rSYS_DDS_CLK_CFG		*((volatile unsigned int *)(ARK1680_VA_SYS+0x198))
#define rSYS_DDS_IO_CFG		*((volatile unsigned int *)(ARK1680_VA_SYS+0x19C))

#define rSYS_PAD_CTRL00		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1c0))
#define rSYS_PAD_CTRL01		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1c4))
#define rSYS_PAD_CTRL02		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1c8))
#define rSYS_PAD_CTRL03		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1cc))
#define rSYS_PAD_CTRL04		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1d0))
#define rSYS_PAD_CTRL05		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1d4))
#define rSYS_PAD_CTRL06		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1d8))
#define rSYS_PAD_CTRL07		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1dc))
#define rSYS_PAD_CTRL08		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1e0))
#define rSYS_PAD_CTRL09		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1e4))
#define rSYS_PAD_CTRL0A		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1e8))
#define rSYS_PAD_CTRL0B		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1ec))
#define rSYS_PAD_CTRL0C		*((volatile unsigned int *)(ARK1680_VA_SYS+0x1f0))


#endif  // #ifndef _SYS_REGS_H_

