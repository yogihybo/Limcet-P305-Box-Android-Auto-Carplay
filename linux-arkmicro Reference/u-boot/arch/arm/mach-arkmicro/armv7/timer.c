/*
 * (C) Copyright 2012, ASTRI
 * Ho Ka Leung <kalho@astri.org>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */


#include <common.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <div64.h>
#include <linux/math64.h>

#if	defined(CONFIG_ARK1668FAMILY)
#define TIMER_CTL0	0x00
#define TIMER_PRS0	0x10
#define TIMER_MOD0	0x20
#define TIMER_CNT0	0x30
#define TIMER_STA0	0x40
#elif defined(CONFIG_ARKN141FAMILY)
#define TIMER_CTL0	0x00
#define TIMER_PRS0	0x0C
#define TIMER_MOD0	0x18
#define TIMER_CNT0	0x24
#endif

static  void __iomem *timer_reg = (void*)CONFIG_SYS_TIMERBASE;

static unsigned long long timestamp;
static unsigned long lastdec;

#define TIMER_PERIOD			1000000 /* 1MHz counter plus 1 = 1us  */
#define TIMER_LOAD_VAL		0x00ffffff

void reset_timer_masked (void)
{
	/* reset time */
	lastdec = readl(timer_reg + TIMER_CNT0);  /* capure current decrementer value time */
	timestamp = 0;         /* start "advancing" time stamp from 0 */

}

ulong get_timer_masked (void)
{
	unsigned long now = readl(timer_reg + TIMER_CNT0);            /* current tick value */

	if (lastdec >= now) {            /* normal mode (non roll) */
		/* normal mode */
		timestamp += lastdec - now; /* move stamp fordward with absoulte diff ticks */
	} else {                        /* we have overflow of the count down timer */
		/* nts = ts + ld + (TLV - now)
		 * ts=old stamp, ld=time that passed before passing through -1
		 * (TLV-now) amount of time after passing though -1
		 * nts = new "advancing time stamp"...it could also roll and cause problems.
		 */
		timestamp += lastdec + (TIMER_LOAD_VAL - now);
	}
	lastdec = now;

	return div_u64(timestamp, 1000);
}


int timer_init(void)
{
	writel(0, timer_reg + TIMER_CTL0);
	writel(ark_get_apb_clock() / TIMER_PERIOD - 1, timer_reg + TIMER_PRS0);
	writel(TIMER_LOAD_VAL, timer_reg + TIMER_MOD0);
	/* No timer interrupt */
	writel(0x03, timer_reg + TIMER_CTL0);

    return 0;
}

unsigned long long get_ticks(void)
{
        return 0;
}

ulong get_timer(ulong base)
{
	return get_timer_masked () - base;
}

void __udelay(unsigned long usec)
{
	long tmo = usec;
	unsigned long last = readl(timer_reg + TIMER_CNT0);
	unsigned long now;

	while (tmo > 0) {
		now = readl(timer_reg + TIMER_CNT0);
		if (last >= now)
			tmo -= last - now;
		else
			tmo -= last + (TIMER_LOAD_VAL - now);
		last = now;
	}
}

ulong get_tbclk(void)
{
	return CONFIG_SYS_HZ;
}

#ifdef CONFIG_SPL_BUILD
void timer_init_24M(void)
{
	writel(0, timer_reg + TIMER_CTL0);
	writel(23, timer_reg + TIMER_PRS0);
	writel(TIMER_LOAD_VAL, timer_reg + TIMER_MOD0);
	writel(3, timer_reg + TIMER_CTL0);
}

void timer_uninit(void)
{
	writel(0, timer_reg + TIMER_CTL0);
}

void timer_delay_us(unsigned int us)
{
	__udelay(us);
}
#endif

