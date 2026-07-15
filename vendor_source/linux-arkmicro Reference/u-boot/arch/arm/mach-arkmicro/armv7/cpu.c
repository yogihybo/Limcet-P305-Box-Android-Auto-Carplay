#include <common.h>
#include <asm/arch/clock.h>
#include <asm/io.h>
#include <asm/pl310.h>

int arch_cpu_init(void)
{
	return ark_clock_init();
}

static void enable_ca7_smp(void)
{
	u32 val;

	/* Read MIDR */
	asm volatile ("mrc p15, 0, %0, c0, c0, 0\n\t" : "=r"(val));
	val = (val >> 4);
	val &= 0xf;

	/* Only set the SMP for Cortex A7 */
	if (val == 0x7) {
		/* Read auxiliary control register */
		asm volatile ("mrc p15, 0, %0, c1, c0, 1\n\t" : "=r"(val));

		if (val & (1 << 6))
			return;

		/* Enable SMP */
		val |= (1 << 6);

		/* Write auxiliary control register */
		asm volatile ("mcr p15, 0, %0, c1, c0, 1\n\t" : : "r"(val));

		DSB;
		ISB;
	}
}

void enable_caches(void)
{
	/* Avoid random hang when download by usb */
	invalidate_dcache_all();

	/* Set ACTLR.SMP bit for Cortex-A7 */
	enable_ca7_smp();

	icache_enable();
	dcache_enable();
}

#ifndef CONFIG_SYS_L2CACHE_OFF
#ifdef CONFIG_SYS_L2_PL310
void v7_outer_cache_enable(void)
{
	struct pl310_regs *const pl310 = (struct pl310_regs *)CONFIG_SYS_PL310_BASE;
	unsigned int val;


	/*
	 * Must disable the L2 before changing the latency parameters
	 * and auxiliary control register.
	 */
	clrbits_le32(&pl310->pl310_ctrl, L2X0_CTRL_EN);

	/*
	 * Set bit 22 in the auxiliary control register. If this bit
	 * is cleared, PL310 treats Normal Shared Non-cacheable
	 * accesses as Cacheable no-allocate.
	 */
	setbits_le32(&pl310->pl310_aux_ctrl, L310_SHARED_ATT_OVERRIDE_ENABLE);

	writel(0x132, &pl310->pl310_tag_latency_ctrl);
	writel(0x132, &pl310->pl310_data_latency_ctrl);

	val = readl(&pl310->pl310_prefetch_ctrl);
	/* Turn on the L2 I/D prefetch */
	val |= 0x30000000;
	writel(val, &pl310->pl310_prefetch_ctrl);

	val = readl(&pl310->pl310_power_ctrl);
	val |= L2X0_DYNAMIC_CLK_GATING_EN;
	val |= L2X0_STNDBY_MODE_EN;
	writel(val, &pl310->pl310_power_ctrl);

	setbits_le32(&pl310->pl310_ctrl, L2X0_CTRL_EN);
}

void v7_outer_cache_disable(void)
{
	struct pl310_regs *const pl310 = (struct pl310_regs *)CONFIG_SYS_PL310_BASE;

	clrbits_le32(&pl310->pl310_ctrl, L2X0_CTRL_EN);
}
#endif /* !CONFIG_SYS_L2_PL310 */
#endif /* !CONFIG_SYS_L2CACHE_OFF */
