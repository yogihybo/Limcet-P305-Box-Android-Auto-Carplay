#include <common.h>

DECLARE_GLOBAL_DATA_PTR;

#define MHZ		(1000*1000)

#if defined(CONFIG_ARK1668FAMILY)

#define rSYS_CLK_SEL			*((volatile unsigned int *)(0xe4900040))
#define rSYS_DEVICE_CLK_CFG3	*((volatile unsigned int *)(0xe490006c))
#define rSYS_PLLRFCK_CTL		*((volatile unsigned int *)(0xe490014c))
#define rSYS_CPUPLL_CFG			*((volatile unsigned int *)(0xe4900150))
#define rSYS_SYSPLL_CFG			*((volatile unsigned int *)(0xe4900154))
#define rSYS_AUDPLL_CFG			*((volatile unsigned int *)(0xe4900158))

static unsigned int ark_get_cpupll_clk(void)
{
	unsigned int refclk_sel = (rSYS_PLLRFCK_CTL >> 0) & 1;
	unsigned int cpupll = rSYS_CPUPLL_CFG;
	unsigned int refclk;

	if (refclk_sel)
		refclk = 12000000;
	else
		refclk = 6000000;

	return (refclk * (cpupll & 0xFF)) / (1 << ((cpupll >> 12) & 0x3));
}

static unsigned int ark_get_audiopll_clk(void)
{
	unsigned int refclk_sel = (rSYS_PLLRFCK_CTL >> 6) & 1;
	unsigned int cpupll = rSYS_AUDPLL_CFG;
	unsigned int refclk;

	if (refclk_sel)
		refclk = 12000000;
	else
		refclk = 6000000;

	return (refclk * (cpupll & 0xFF)) / (1 << ((cpupll >> 12) & 0x3));
}

unsigned long ark_get_syspll_clk(void)
{
	unsigned int refclk_sel = (rSYS_PLLRFCK_CTL >> 3) & 1;
	unsigned int cpupll = rSYS_SYSPLL_CFG;
	unsigned int refclk;

	if (refclk_sel)
		refclk = 12000000;
	else
		refclk = 6000000;

	return (unsigned long)((refclk * (cpupll & 0xFF)) / (1 << ((cpupll >> 12) & 0x3)));
}

unsigned long ark_get_cpu_clock(void)
{
	unsigned int clksel = (rSYS_CLK_SEL >> 19) & 7;
	unsigned int div = (rSYS_CLK_SEL >> 22) & 7;
	unsigned int clk_src;
	switch (clksel) {
	case 0:
		clk_src = ark_get_cpupll_clk();
		break;
	default:
		clk_src = 24000000;
		break;
	}
	return (unsigned long)(clk_src / (div ? div : 1));
}

unsigned long ark_get_axi_clock(void)
{
	unsigned int clksel = (rSYS_CLK_SEL >> 9) & 0xF;
	unsigned int div = (rSYS_CLK_SEL >> 13) & 7;
	unsigned int div2 = (rSYS_CLK_SEL >> 16) & 2;
	unsigned int clk_src;
	switch (clksel) {
	case 0:
		clk_src = ark_get_cpupll_clk();
		break;
	case 1:
		clk_src = ark_get_syspll_clk();
		break;
	default:
		clk_src = 24000000;
		break;
	}
	return (unsigned long)(clk_src / (div ? div : 1) / (1 << div2));
}

unsigned long ark_get_ahb_clock(void)
{
	unsigned int clksel = (rSYS_CLK_SEL >> 2) & 0xF;
	unsigned int div = (rSYS_CLK_SEL >> 6) & 7;
	unsigned int clk_src;
	switch (clksel) {
	case 0:
		clk_src = ark_get_cpupll_clk();
		break;
	case 1:
		clk_src = ark_get_syspll_clk();
		break;
	default:
		clk_src = 24000000;
		break;
	}
	return (unsigned long)(clk_src / (div ? div : 1));
}

unsigned long ark_get_apb_clock(void)
{
	unsigned int div = (rSYS_CLK_SEL >> 0) & 3;

	return ark_get_ahb_clock() / (1 << div);
}

unsigned long ark_get_ddr_clock(void)
{
	unsigned int clksel = (rSYS_DEVICE_CLK_CFG3 >> 18) & 0xF;
	unsigned int clk_src;
	switch (clksel) {
	case 0:
		clk_src = ark_get_cpupll_clk();
		break;
	case 1:
		clk_src = ark_get_syspll_clk();
		break;
	case 2:
		clk_src = ark_get_audiopll_clk();
		break;
	default:
		clk_src = 24000000;
		break;
	}
	return (unsigned long)clk_src;
}

#elif defined(CONFIG_ARKN141FAMILY)

#define rSYS_CLK_SEL			*((volatile unsigned int *)(0x40408040))
#define rSYS_PLLRFCK_CTL		*((volatile unsigned int *)(0x4040814c))
#define rSYS_CPUPLL_CFG			*((volatile unsigned int *)(0x40408150))
#define rSYS_SYSPLL_CFG			*((volatile unsigned int *)(0x40408154))
#define rSYS_AUDPLL_CFG			*((volatile unsigned int *)(0x40408158))

static unsigned int ark_get_cpupll_clk(void)
{
	unsigned int refclk_sel = (rSYS_PLLRFCK_CTL >> 0) & 1;
	unsigned int cpupll = rSYS_CPUPLL_CFG;
	unsigned int refclk;

	if (refclk_sel)
		refclk = 12000000;
	else
		refclk = 6000000;

	return (refclk * (cpupll & 0xFF)) / (1 << ((cpupll >> 12) & 0x3));
}

static unsigned int ark_get_audiopll_clk(void)
{
	unsigned int refclk_sel = (rSYS_PLLRFCK_CTL >> 6) & 1;
	unsigned int cpupll = rSYS_AUDPLL_CFG;
	unsigned int refclk;

	if (refclk_sel)
		refclk = 12000000;
	else
		refclk = 6000000;

	return (refclk * (cpupll & 0xFF)) / (1 << ((cpupll >> 12) & 0x3));
}

unsigned long ark_get_syspll_clk(void)
{
	unsigned int refclk_sel = (rSYS_PLLRFCK_CTL >> 3) & 1;
	unsigned int cpupll = rSYS_SYSPLL_CFG;
	unsigned int refclk;

	if (refclk_sel)
		refclk = 12000000;
	else
		refclk = 6000000;

	return (unsigned long)((refclk * (cpupll & 0xFF)) / (1 << ((cpupll >> 12) & 0x3)));
}

unsigned long ark_get_cpu_clock(void)
{
	unsigned int clksel = (rSYS_CLK_SEL >> 25) & 7;
	unsigned int div = ((rSYS_CLK_SEL >> 28) & 7) + 1;
	unsigned int clk_src;
	switch (clksel) {
	case 0:
		clk_src = ark_get_cpupll_clk();
		break;
	case 1:
		clk_src = 32768;
		break;
	case 2:
		clk_src = ark_get_syspll_clk();
		break;
	default:
		clk_src = 24000000;
		break;
	}
	return (unsigned long)(clk_src / div);
}

unsigned long ark_get_axi_clock(void)
{
	unsigned int clksel = (rSYS_CLK_SEL >> 0) & 0x7;
	unsigned int div = ((rSYS_CLK_SEL >> 4) & 7) + 1;
	unsigned int clk_src;
	switch (clksel) {
	case 0:
		clk_src = ark_get_cpupll_clk();
		break;
	case 1:
		clk_src = ark_get_syspll_clk();
		break;
	case 2:
		clk_src = ark_get_audiopll_clk();
		break;
	default:
		clk_src = 24000000;
		break;
	}
	return (unsigned long)(clk_src / div);
}

unsigned long ark_get_ahb_clock(void)
{
	unsigned int clksel = (rSYS_CLK_SEL >> 15) & 0x7;
	unsigned int div = ((rSYS_CLK_SEL >> 19) & 7) + 1;
	unsigned int clk_src;
	switch (clksel) {
	case 0:
		clk_src = ark_get_cpupll_clk();
		break;
	case 1:
		clk_src = ark_get_syspll_clk();
		break;
	case 2:
		clk_src = ark_get_audiopll_clk();
		break;
	default:
		clk_src = 24000000;
		break;
	}
	return (unsigned long)(clk_src / div);
}

unsigned long ark_get_apb_clock(void)
{
	unsigned int div = (rSYS_CLK_SEL >> 23) & 3;

	return ark_get_ahb_clock() / (1 << div);
}

unsigned long ark_get_ddr_clock(void)
{
	unsigned int clksel = (rSYS_CLK_SEL >> 8) & 0xF;
	unsigned int div = (rSYS_CLK_SEL >> 12) & 7;
	unsigned int clk_src;
	switch (clksel) {
	case 0:
		clk_src = ark_get_cpupll_clk();
		break;
	case 1:
		clk_src = ark_get_syspll_clk();
		break;
	case 2:
		clk_src = ark_get_audiopll_clk();
		break;
	default:
		clk_src = 24000000;
		break;
	}
	return (unsigned long)(clk_src / (div ? div : 1));
}

#elif defined(CONFIG_TARGET_ARK1668E_FPGA)

unsigned long ark_get_cpu_clock(void)
{
	return 24000000;
}

unsigned long ark_get_axi_clock(void)
{
	return 24000000;
}

unsigned long ark_get_ddr_clock(void)
{
	return 20000000;
}

unsigned long ark_get_timer_clock(void)
{
	return 24000000;
}

unsigned long ark_get_syspll_clk(void)
{
	return 24000000;
}

unsigned long ark_get_apb_clock(void)
{
	return 24000000;
}

#elif defined(CONFIG_TARGET_ARKN141S_FPGA)

unsigned long ark_get_cpu_clock(void)
{
	return 24000000;
}

unsigned long ark_get_axi_clock(void)
{
	return 24000000;
}

unsigned long ark_get_ddr_clock(void)
{
	return 20000000;
}

unsigned long ark_get_timer_clock(void)
{
	return 24000000;
}

unsigned long ark_get_syspll_clk(void)
{
	return 24000000;
}

unsigned long ark_get_apb_clock(void)
{
	return 24000000;
}

#elif defined(CONFIG_ARK1668EFAMILY)

#define rSYS_CLK_SEL		*((volatile unsigned int *)(0xe4900040))
#define rSYS_CPUPLL_CFG_0	*((volatile unsigned int *)(0xe4900280))
#define rSYS_CPUPLL_CFG_1	*((volatile unsigned int *)(0xe4900284))
#define rSYS_LCDPLL_CFG_0	*((volatile unsigned int *)(0xe490028c))
#define rSYS_LCDPLL_CFG_1	*((volatile unsigned int *)(0xe4900290))
#define rSYS_AXIPLL_CFG_0	*((volatile unsigned int *)(0xe4900298))
#define rSYS_AHBPLL_CFG_0	*((volatile unsigned int *)(0xe490029c))
#define rSYS_APBPLL_CFG_0	*((volatile unsigned int *)(0xe49002a0))
#define rSYS_DDRPLL_CFG_0	*((volatile unsigned int *)(0xe49002a8))
#define rSYS_MACPLL_CFG_0	*((volatile unsigned int *)(0xe49002b4))

#define XTAL_FREQ	24000000

unsigned long ark_get_cpu_clock(void)
{
	unsigned int fref = XTAL_FREQ;
	unsigned int nr, fint, fvco, nfx, nff, od, div;

	nr = (rSYS_CPUPLL_CFG_0 >> 15) & 0x7f;
	fint = fref / nr;
	nff = rSYS_CPUPLL_CFG_1 & 0x7fff;
	nfx = (rSYS_CPUPLL_CFG_1 >> 15) & 0x1ff;
	fvco = fint * nfx + (unsigned long long)fint * nff / 32768;
	od = (rSYS_CPUPLL_CFG_1 >> 24) & 7;
	div = ((rSYS_CLK_SEL >> 28) & 0xf) + 1;
	
	return fvco / (1 << od) / div;
}

unsigned long ark_get_lcdpll_clock(void)
{
	unsigned int fref = XTAL_FREQ;
	unsigned int nr, fint, fvco, nfx, nff, od;

	nr = (rSYS_LCDPLL_CFG_0 >> 15) & 0x7f;
	fint = fref / nr;
	nff = rSYS_LCDPLL_CFG_1 & 0x7fff;
	nfx = (rSYS_LCDPLL_CFG_1 >> 15) & 0x1ff;
	fvco = fint * nfx + (unsigned long long)fint * nff / 32768;
	od = (rSYS_LCDPLL_CFG_1 >> 24) & 7;

	return fvco / (1 << od);
}

enum {
	ARK_PLL_AXI = 0,
	ARK_PLL_AHB,
	ARK_PLL_APB,
	ARK_PLL_MAC,
	ARK_PLL_DDR,
	ARK_PLL_XTAL,
};

unsigned long ark_get_pll_freq(int pll)
{
	unsigned fref = XTAL_FREQ;
	unsigned ns, ms, ps;
	u32 val;

	switch (pll) {
	case ARK_PLL_AXI:
		val = rSYS_AXIPLL_CFG_0;
		break;
	case ARK_PLL_AHB:
		val = rSYS_AHBPLL_CFG_0;
		break;
	case ARK_PLL_APB:
		val = rSYS_APBPLL_CFG_0;
		break;
	case ARK_PLL_MAC:
		val = rSYS_MACPLL_CFG_0;
		break;
	case ARK_PLL_DDR:
		val = rSYS_DDRPLL_CFG_0;
		break;
	case ARK_PLL_XTAL:
		val = XTAL_FREQ;
		break;
	default:
		return 0;
	}

	ms = val & 0x7;
	ns = (val >> 3) & 0x1ff;
	ps = (val >> 12) & 0x1f;

	return fref / ms * ns / ps / 2;
}

unsigned long ark_get_axi_clock(void)
{
	unsigned div;
	int pll = ARK_PLL_XTAL;

	switch ((rSYS_CLK_SEL >> 16) & 0xf) {
		case 0:
			pll = ARK_PLL_AHB;
			break;
		case 1:
			pll = ARK_PLL_AXI;
			break;
		case 2:
			pll = ARK_PLL_MAC;
			break;
	}
	div = ((rSYS_CLK_SEL >> 20) & 0xf) + 1;

	return ark_get_pll_freq(pll) / div;
}

unsigned long ark_get_ahb_clock(void)
{
	unsigned div;
	int pll = ARK_PLL_XTAL;

	switch ((rSYS_CLK_SEL >> 8) & 0xf) {
		case 0:
			pll = ARK_PLL_AHB;
			break;
		case 1:
			pll = ARK_PLL_AXI;
			break;
		case 2:
			pll = ARK_PLL_MAC;
			break;
	}
	div = ((rSYS_CLK_SEL >> 12) & 0xf) + 1;

	return ark_get_pll_freq(pll) / div;
}

unsigned long ark_get_apb_clock(void)
{
	unsigned div;
	int pll = ARK_PLL_XTAL;

	switch ((rSYS_CLK_SEL >> 0) & 0xf) {
		case 0:
			pll = ARK_PLL_APB;
			break;
		case 1:
			pll = ARK_PLL_AXI;
			break;
		case 2:
			pll = ARK_PLL_MAC;
			break;
	}
	div = ((rSYS_CLK_SEL >> 4) & 0xf) + 1;

	return ark_get_pll_freq(pll) / div;
}

unsigned long ark_get_ddr_clock(void)
{
	return ark_get_pll_freq(ARK_PLL_DDR) * 2;
}

unsigned long ark_get_timer_clock(void)
{
	return 24000000;
}

#elif defined(CONFIG_TARGET_AMT630H)

unsigned long ark_get_cpu_clock(void)
{
   return 496000000;
}

unsigned long ark_get_axi_clock(void)
{
   return 480000000;
}

unsigned long ark_get_ddr_clock(void)
{
   return 240000000;
}

unsigned long ark_get_timer_clock(void)
{
   return 24000000;
}

unsigned long ark_get_syspll_clk(void)
{
   return 240000000;
}

unsigned long ark_get_apb_clock(void)
{
   return 120000000;
}

#endif


int ark_clock_init(void)
{
	gd->cpu_clk = ark_get_cpu_clock();
	gd->bus_clk = ark_get_axi_clock();
	gd->mem_clk = ark_get_ddr_clock();

	return 0;
}
