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

#define rSYS_PAD_CTRL08		*(volatile unsigned int*)0xe49001e0
#define rSYS_PAD_CTRL09		*(volatile unsigned int*)0xe49001e4

/* static void switch_to_main_crystal_osc(void)
{
	return;
} */

void board_init_f(ulong dummy)
{
	u32 tmp;

	timer_init();

	board_early_init_f();

	preloader_console_init();

	//slect nand pad
	tmp = rSYS_PAD_CTRL08;	
	tmp &= ~((0x7<<27) | (0x7<<24)|(0x7<<21) | (0x7<<18) | (0x7<<15)|(0x7<<12)|(0x7<<9)|(0x7<<6)); 
	tmp |=((0x1<<27) | (0x1<<24)|(0x1<<21) | (0x1<<18) | (0x1<<15)|(0x1<<12)|(0x1<<9)|(0x1<<6));
	rSYS_PAD_CTRL08 = tmp;

	tmp = rSYS_PAD_CTRL09;	
	tmp &= ~((0x7<<15) | (0x7<<12) | (0x7<<9)|(0x7<<6)|(0x7<<3)|(0x7<<0)); 
	tmp |=((1<<15)|(1<<12)|(1<<9)|(1<<6)|(1<<3)|(1<<0));//enable nand cle, ale,ren,wen
	rSYS_PAD_CTRL09 = tmp;

	mem_init();
}
