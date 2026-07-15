#ifndef __ASM_ARCH_ARK1668_SYSREG_H
#define __ASM_ARCH_ARK1668_SYSREG_H

#include <asm/io.h>

#define SYS_REG_BASE	0xe4900000

#define SYS_CLK_SEL				0x40
#define SYS_DEVICE_CLK_CFG3		0x6c
#define SYS_PLLRFCK_CTL			0x14c
#define SYS_CPUPLL_CFG			0x150
#define SYS_SYSPLL_CFG			0x154
#define SYS_AUDPLL_CFG			0x158
#define SYS_DDR_IO_CFG			0x19c
#define SYS_IO_DRIVER01			0x1f4
#define SYS_IO_DRIVER02			0x1f8

static inline volatile unsigned int read_sys_reg(int offset)
{
	return readl(SYS_REG_BASE + offset);
}

static inline void write_sys_reg(unsigned int val, int offset)
{
	writel(val, SYS_REG_BASE + offset);
}

#endif
