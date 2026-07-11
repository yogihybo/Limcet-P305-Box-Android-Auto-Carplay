#ifndef __ASM_ARCH_ARK1668E_SYSREG_H
#define __ASM_ARCH_ARK1668E_SYSREG_H

#include <asm/io.h>

#define SYS_REG_BASE	0xe4900000

#define SYS_CLK_SEL				0x40
#define SYS_DEVICE_CLK_CFG1		0x64
#define SYS_DEVICE_CLK_CFG3		0x6c
#define SYS_PLLRFCK_CTL			0x14c
#define SYS_CPUPLL_CFG			0x150
#define SYS_SYSPLL_CFG			0x154
#define SYS_AUDPLL_CFG			0x158
#define SYS_DDR_IO_CFG			0x19c
#define SYS_PAD_CTRL00			0x1c0
#define SYS_PAD_CTRL01			0x1c4
#define SYS_PAD_CTRL02			0x1c8
#define SYS_PAD_CTRL03			0x1cc
#define SYS_PAD_CTRL04			0x1d0
#define SYS_PAD_CTRL05			0x1d4
#define SYS_PAD_CTRL06			0x1d8
#define SYS_PAD_CTRL07			0x1dc
#define SYS_PAD_CTRL08			0x1e0
#define SYS_PAD_CTRL09			0x1e4
#define SYS_IO_DRIVER01			0x1f4
#define SYS_IO_DRIVER02			0x1f8
#define SYS_CPU_CFG2			0x208
#define SYS_DEVICE_CLK_CFG6		0x22c
#define SYS_CPUPLL_CFG_0		0x280
#define SYS_CPUPLL_CFG_1		0x284
#define SYS_CPUPLL_CFG_2		0x288
#define SYS_LCDPLL_CFG_0		0x28c
#define SYS_LCDPLL_CFG_1		0x290
#define SYS_LCDPLL_CFG_2		0x294
#define SYS_AXIPLL_CFG_0		0x298
#define SYS_AHBPLL_CFG_0		0x29c
#define SYS_APBPLL_CFG_0		0x2a0
#define SYS_AUDPLL_CFG_0		0x2a4
#define SYS_DDRPLL_CFG_0		0x2a8
#define SYS_TVPLL_CFG_0			0x2ac
#define SYS_MACPLL_CFG_0		0x2b4
#define SYS_MACPLL_CFG_1		0x2b8
#define SYS_MACPLL_CFG_2		0x2bc

static inline volatile unsigned int read_sys_reg(int offset)
{
	return readl(SYS_REG_BASE + offset);
}

static inline void write_sys_reg(unsigned int val, int offset)
{
	writel(val, SYS_REG_BASE + offset);
}

#endif
