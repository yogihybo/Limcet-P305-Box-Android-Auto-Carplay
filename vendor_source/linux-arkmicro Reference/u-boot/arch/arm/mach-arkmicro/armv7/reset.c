/*
 * (C) Copyright 2013 ASTRI
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <common.h>
#include <asm/io.h>

/* WDT */
#define WDT_CR	0x00
#define WDT_PSR	0x04
#define WDT_LDR	0x08
#define WDT_VLR	0x0C
#define WDT_ISR	0x10
#define WDT_RCR	0x14
#define WDT_TMR	0x18
#define WDT_TCR	0x1C

#define rSYS_SOFT_RSTNA			0x74
#define rSYS_SOFT_RSTNB			0x78

extern unsigned long ark_get_apb_clock(void);

#define div_round_up(x, div) (((x) + (div) - 1) / (div))

static void ark_wdt_reset(u32 timeout_ms)
{
	u32 regbase = CONFIG_WATCHDOG_BASEADDR;
	u32 freq = ark_get_apb_clock();
	u32 count, divisor = 1;

	freq = div_round_up(freq, 128);
	count = timeout_ms * (u64)freq / 1000;
	if (count > 0xffff) {
		divisor = div_round_up(count, 0xffff);
		if (divisor > 0xffff) {
			printf("timeout %d too big\n", divisor);
			return;
		}
	}
	count = div_round_up(count, divisor);
	writel(0, regbase + WDT_CR);
	writel(divisor, regbase + WDT_PSR);
	writel(count, regbase + WDT_LDR);
	writel((0x3 << 4) | 3, regbase + WDT_CR);
}
static void ark1668e_softreset(void)
{
	u32 sysregbase = 0xe4900000;
	writel(0, sysregbase + rSYS_SOFT_RSTNA);
	writel(0, sysregbase + rSYS_SOFT_RSTNB);
}

void reset_cpu(ulong addr)
{
	/* TODO: Program the system controller to reset */
#ifdef CONFIG_ARK1668EFAMILY
	ark_wdt_reset(2000);
	ark1668e_softreset();
#else
	ark_wdt_reset(100);
#endif
	/* loop for waiting reset */
	while(1);
}
