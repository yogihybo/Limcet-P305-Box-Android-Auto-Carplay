// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <spl.h>
#include <asm/arch/timer.h>
#include <asm/arch/ark1668-sysreg.h>
#include <asm/arch/ark-common.h>
#include <asm-generic/gpio.h>

#define SYSPLL_CLK  330
#define CPUPLL_CLK  696
#define AUDPLL_CLK  426

static void switch_to_main_crystal_osc(void)
{
	unsigned int val;

	/* set syspll */
	write_sys_reg(read_sys_reg(SYS_SYSPLL_CFG) & ~(1 << 14), SYS_SYSPLL_CFG);
	timer_delay_us(10);
	write_sys_reg(read_sys_reg(SYS_PLLRFCK_CTL) | (1 << 3), SYS_PLLRFCK_CTL);
	val = (SYSPLL_CLK / 6) | (0x1C << 8) | (1 << 14) | (1 << 15);
	write_sys_reg(val, SYS_SYSPLL_CFG);

	/* set audpll */
	write_sys_reg(read_sys_reg(SYS_AUDPLL_CFG) & ~(1 << 14), SYS_AUDPLL_CFG);
	timer_delay_us(10);
	write_sys_reg(read_sys_reg(SYS_PLLRFCK_CTL) | (1 << 6), SYS_PLLRFCK_CTL);
	val = (AUDPLL_CLK / 6) | (0x1C << 8) | (1 << 14) | (1 << 15);
	write_sys_reg(val, SYS_AUDPLL_CFG);

	/* set cpupll */
	write_sys_reg(read_sys_reg(SYS_CPUPLL_CFG) & ~(1 << 14), SYS_CPUPLL_CFG);
	timer_delay_us(10);
	write_sys_reg(read_sys_reg(SYS_PLLRFCK_CTL) | (1 << 0), SYS_PLLRFCK_CTL);
	val = (CPUPLL_CLK / 6) | (0x1C << 8) | (1 << 14) | (1 << 15);
	write_sys_reg(val, SYS_CPUPLL_CFG);

	timer_delay_us(6000);

	/* set system clock */
	val = read_sys_reg(SYS_CLK_SEL);
	val &= ~0x7ffff;
	val |= (1 << 18) | (0 << 16) | (1 << 13) | (1 << 9) | (2 << 6) | (1 << 2) | 1;
	write_sys_reg(val, SYS_CLK_SEL);
	
	timer_delay_us(10);

	/* set ddr3 clock */
	val = read_sys_reg(SYS_DEVICE_CLK_CFG3);
	val &= ~(0x7f << 18);
	val |= (2 << 18);	//select audpll
	write_sys_reg(val, SYS_DEVICE_CLK_CFG3);
	timer_delay_us(10);

	/* cpu clock switch to cpupll */
	val = read_sys_reg(SYS_CLK_SEL);;
	val &= ~(0x7 << 19);
	write_sys_reg(val, SYS_CLK_SEL);
	timer_delay_us(10);
}

void board_init_f(ulong dummy)
{
	write_sys_reg(0, SYS_IO_DRIVER01);
	write_sys_reg(0, SYS_IO_DRIVER02);

	timer_init_24M();
	write_sys_reg(read_sys_reg(SYS_DDR_IO_CFG) & ~7, SYS_DDR_IO_CFG);
	gpio_direction_output(8, 0);
	udelay(1);
	switch_to_main_crystal_osc();

	timer_init();

	board_early_init_f();

	preloader_console_init();

	mem_init();
}
