// SPDX-License-Identifier: GPL-2.0+
#include <common.h>
#include <spl.h>
#include <asm/arch/timer.h>
#include <asm/arch/ark1668e-sysreg.h>
#include <asm/arch/ark-common.h>
#include <asm-generic/gpio.h>
#define CPUPLL_CLK  800
#define LCDPLL_CLK	480
#define AXIPLL_CLK	432
#define AHBPLL_CLK	336
#define APBPLL_CLK	480
#define DDRPLL_CLK  360
#define MACPLL_CLK	1000
#define AUDPLL_CLK	480
enum sscg_clk_id {
	SSCG_CPU,
	SSCG_LCD,
	SSCG_MAC,
};
enum pll_clk_id {
	PLL_AXI,
	PLL_AHB,
	PLL_APB,
	PLL_AUD,
	PLL_TV,
	PLL_DDR,
};
/*
 * fref=24MHz, nr=2, fint=12MHz
 * fvco=1000~2000MHz
 * rs = 4 (fvcomax/fint=166)
 * nf[23:15] integer nf[14:0] fraction
*/
static void set_sscg_clk(int clk_id, unsigned int freq_khz)
{
	unsigned int nr = 2, fint = 12000, rs = 4;
	unsigned int od, fvco, nfx, nff, cpa, cpax;
	int i;
	unsigned int cfg0, cfg1, cfg2;
	unsigned int regval;
	if (clk_id == SSCG_MAC) {
		nr = 3;
		fint = 8000;
		rs = 7;
	}
	freq_khz = freq_khz / fint * fint;
	for (i = 0; i < 8; i++) {
		fvco = freq_khz * (1 << i);
		if (fvco >= 1000 * 1000 && fvco <= 2000 * 1000) {
			od = i;
			break;
		}
	}
	if (i == 8)
		goto fail;
	nfx = fvco / fint;
	if (nfx >= 50 && nfx <= 100) {
		cpa = nfx * 4;
		cpax = 3;
	} else if (nfx > 100 && nfx <= 200) {
		cpa = nfx * 2;
		cpax = 1;
	} else if (nfx > 200 && nfx <= 400) {
		cpa = nfx;
		cpax = 0;
	}
	else
		goto fail;
	nff = ((1 << 15) * (fvco / 1000) / (fint / 1000)) & ((1 << 15) - 1);
	if (clk_id == SSCG_CPU) {
		cfg0 = SYS_CPUPLL_CFG_0;
		cfg1 = SYS_CPUPLL_CFG_1;
		cfg2 = SYS_CPUPLL_CFG_2;
	} else if (clk_id == SSCG_LCD) {
		cfg0 = SYS_LCDPLL_CFG_0;
		cfg1 = SYS_LCDPLL_CFG_1;
		cfg2 = SYS_LCDPLL_CFG_2;
	} else if (clk_id == SSCG_MAC) {
		cfg0 = SYS_MACPLL_CFG_0;
		cfg1 = SYS_MACPLL_CFG_1;
		cfg2 = SYS_MACPLL_CFG_2;
	} else {
		puts("Invalid sscg clk_id.\n");
		return;
	}
	/* disable clk first */
	write_sys_reg(read_sys_reg(cfg1) | (1 << 27), cfg1);
	regval = read_sys_reg(cfg0);
	regval &= ~0x3FFFFF;
	regval |= (cpa << 0) | (cpax << 9) | (rs << 11) | (nr << 15);
	write_sys_reg(regval, cfg0);
	regval = read_sys_reg(cfg1);
	regval &= ~0x7FFFFFF;
	regval |= ((nfx<<15) | nff) | (od << 24);
	write_sys_reg(regval, cfg1);
	write_sys_reg(read_sys_reg(cfg2) | 1, cfg2);
	/* enable clk */
	write_sys_reg(read_sys_reg(cfg1) & ~(1 << 27), cfg1);
	return ;
fail:
	puts("Unsupported sscg freq.\n");
}
/*
 * vcoout = fref*(ns/ms)
 * clkout = vcoout/ps/2
*/
static void set_pll_clk(int clk_id, unsigned int freq_mhz)
{
	unsigned int ms;//[2:0]
	unsigned int ns;//[11:3]
	unsigned int ps;//[16:12]
	unsigned int vcoout;
	unsigned int fin;
	volatile unsigned int cfg0;
	unsigned int regval;
	ms = 2;
	fin = 24 / ms;
	freq_mhz = freq_mhz / fin * fin;
	for (ps = 0; ps < 32; ps++) {
		vcoout = freq_mhz * ps * 2;
		if (vcoout >= 1000 && vcoout <= 2000)
			break;
	}
	if (ps == 32)
		goto fail;
	ns = vcoout / fin;
	switch (clk_id) {
	case PLL_AXI:
		cfg0 = SYS_AXIPLL_CFG_0;
		break;
	case PLL_AHB:
		cfg0 = SYS_AHBPLL_CFG_0;
		break;
	case PLL_APB:
		cfg0 = SYS_APBPLL_CFG_0;
		break;
	case PLL_AUD:
		cfg0 = SYS_AUDPLL_CFG_0;
		break;
	case PLL_TV:
		cfg0 = SYS_TVPLL_CFG_0;
		break;
	case PLL_DDR:
		cfg0 = SYS_DDRPLL_CFG_0;
		break;
	default:
		puts("Invalid pll clk_id.\n");
		return;
	}
	/* disable clk first */
	write_sys_reg(read_sys_reg(cfg0) & ~(1 << 17), cfg0);
	regval = read_sys_reg(cfg0);
	regval &= ~0x1FFFF;
	regval |= (ms << 0) | (ns << 3) | (ps << 12);
	write_sys_reg(regval, cfg0);
	/* enable clk */
	write_sys_reg(read_sys_reg(cfg0) | (1 << 17), cfg0);
	return;
fail:
	puts("Unsupported pll freq.\n");
}
static void switch_to_main_crystal_osc(void)
{
	unsigned int cpu_freq = CPUPLL_CLK;
	unsigned int regval;
	/* switch to 24M clk first */
	write_sys_reg(0x04040404, SYS_CLK_SEL);
	udelay(50);
	set_sscg_clk(SSCG_CPU, CPUPLL_CLK * 1000);
	set_sscg_clk(SSCG_LCD, LCDPLL_CLK * 1000);
	set_sscg_clk(SSCG_MAC, MACPLL_CLK * 1000);
	set_pll_clk(PLL_AXI, AXIPLL_CLK);
	set_pll_clk(PLL_AHB, AHBPLL_CLK);
	set_pll_clk(PLL_APB, APBPLL_CLK);
	set_pll_clk(PLL_DDR, DDRPLL_CLK);
	set_pll_clk(PLL_AUD, AUDPLL_CLK);
	udelay(100);
	/* sync cpu bclk */
	regval = read_sys_reg(SYS_CPU_CFG2);
	regval &= ~(0x3<<12);
	if(cpu_freq > 1000)
		regval |= (2<<12);
	else if(cpu_freq > 500)
		regval |= (1<<12);
	write_sys_reg(regval, SYS_CPU_CFG2);
	/* switch to sys pll clk */
	/* ahb and pclk and pclk1 must have the same clk source */
	/* change the config will cause mfc working fail */
	/* ahb and apb clk */
	regval = 0x04040404;
	regval |= (2 << 12) | (1 << 8) | (5 << 4) | 1;
	write_sys_reg(regval, SYS_CLK_SEL);
	udelay(50);
	/* pclk1 */
	regval = 0x40;
	regval |= (1 << 4) | 5;
	write_sys_reg(regval, SYS_DEVICE_CLK_CFG6);
	udelay(50);
	/* switch from 24MHz to pll */
	regval = read_sys_reg(SYS_CLK_SEL);
	regval &= ~((1 << 26) | (1 << 18) | (1 << 10) | (1 << 2));
	write_sys_reg(regval, SYS_CLK_SEL);
	udelay(50);
	regval = read_sys_reg(SYS_DEVICE_CLK_CFG6);
	regval &= ~(1 << 6);
	write_sys_reg(regval, SYS_DEVICE_CLK_CFG6);
	udelay(50);
	/* mfc clk adjusting */
	/* the mfc clk can't reconfig at other place */
	regval = read_sys_reg(SYS_DEVICE_CLK_CFG1);
	regval &= ~(0x7 << 16);
	regval |= (2 << 16);
	write_sys_reg(regval, SYS_DEVICE_CLK_CFG1);
	udelay(50);
	regval = read_sys_reg(SYS_DEVICE_CLK_CFG1);
	regval &= ~(0xf << 19);
	regval |= (1 << 19);
	write_sys_reg(regval, SYS_DEVICE_CLK_CFG1);
	udelay(50);
	regval &= ~(0xf << 19);
	regval |= (3 << 19);
	write_sys_reg(regval, SYS_DEVICE_CLK_CFG1);
	udelay(50);
	/* switch to ddrpll clk */
	regval = read_sys_reg(SYS_DEVICE_CLK_CFG3);
	regval &= ~((0x7 << 18) | (0xf << 22));
	regval |= (2 << 18) | (0 << 22);
	write_sys_reg(regval, SYS_DEVICE_CLK_CFG3);
	udelay(50);
	return;
}
#define rWDT_CR		*(volatile unsigned int *)0xe4b00000
void board_init_f(ulong dummy)
{
	u32 tmp;
	/* Disable watch dog */
	rWDT_CR = 0;
	timer_init();
	switch_to_main_crystal_osc();
	board_early_init_f();
	preloader_console_init();
	//slect nand pad
	tmp = read_sys_reg(SYS_PAD_CTRL08);
	tmp &= ~((0x7<<27) | (0x7<<24)|(0x7<<21) | (0x7<<18) | (0x7<<15)|(0x7<<12)|(0x7<<9)|(0x7<<6));
	tmp |=((0x1<<27) | (0x1<<24)|(0x1<<21) | (0x1<<18) | (0x1<<15)|(0x1<<12)|(0x1<<9)|(0x1<<6));
	write_sys_reg(tmp, SYS_PAD_CTRL08);
	tmp = read_sys_reg(SYS_PAD_CTRL09);
	tmp &= ~((0x7<<15) | (0x7<<12) | (0x7<<9)|(0x7<<6)|(0x7<<3)|(0x7<<0));
	tmp |=((1<<15)|(1<<12)|(1<<9)|(1<<6)|(1<<3)|(1<<0));//enable nand cle, ale,ren,wen
	write_sys_reg(tmp, SYS_PAD_CTRL09);
//	mem_init();
	ddr3_sdramc_init();
	ark_watchdog_start(1500);
}
#ifndef CONFIG_SPL_LIBCOMMON_SUPPORT
void puts(const char *s)
{
	serial_puts(s);
}
void putc(const char c)
{
	serial_putc(c);
}
#endif
#ifndef CONFIG_SPL_LIBGENERIC_SUPPORT
void udelay(unsigned long usec)
{
	timer_delay_us(usec);
}
void hang(void)
{
	for (;;);
}
uint32_t __div64_32(uint64_t *n, uint32_t base)
{
    uint64_t rem = *n;
    uint64_t b = base;
    uint64_t res, d = 1;
    uint32_t high = rem >> 32;
    /* Reduce the thing a bit first */
    res = 0;
    if (high >= base) {
        high /= base;
        res = (uint64_t) high << 32;
        rem -= (uint64_t) (high*base) << 32;
    }
    while ((int64_t)b > 0 && b < rem) {
        b = b+b;
        d = d+d;
    }
    do {
        if (rem >= b) {
            rem -= b;
            res += d;
        }
        b >>= 1;
        d >>= 1;
    } while (d);
    *n = res;
    return rem;
}
void * memmove(void * dest,const void *src,size_t count)
{
    char *tmp, *s;
    if (dest <= src) {
        memcpy(dest, src, count);
    } else {
        tmp = (char *) dest + count;
        s = (char *) src + count;
        while (count--)
            *--tmp = *--s;
        }
    return dest;
}
#endif
