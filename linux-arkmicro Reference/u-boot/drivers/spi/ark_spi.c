/*
 * Copyright (C) 2008, Guennadi Liakhovetski <lg@denx.de>
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
 *
 */

#include <common.h>
#include <malloc.h>
#include <spi.h>
#include <errno.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <asm/arch/gpio.h>
#include <asm/arch/ark-spi.h>
#include <asm/arch/clock.h>

static unsigned long spi_bases[] = {
	CONFIG_SYS_SPI_BASE
};

#define OUT	ARK_GPIO_DIRECTION_OUT

#define reg_read readl
#define reg_write(a, v) writel(v, a)

struct ark_spi_slave {
	struct spi_slave slave;
	unsigned long	base;
	u32		ctrl_reg;
	u32		cfg_reg;

	int		ss_pol;
};

//static struct ark_spi_slave *ark_spi_ctxs = NULL;

static inline struct ark_spi_slave *to_ark_spi_slave(struct spi_slave *slave)
{
	return container_of(slave, struct ark_spi_slave, slave);
}

void spi_cs_activate(struct spi_slave *slave)
{
	struct ark_spi_slave *arks = to_ark_spi_slave(slave);

	if(slave->cs == 0){
		if(arks->ss_pol)
			gpio_set_value(CONFIG_SPI_CS0_GPIO, 1);
		else
			gpio_set_value(CONFIG_SPI_CS0_GPIO, 0);
	}else if(slave->cs == 1){

	}
}

void spi_cs_deactivate(struct spi_slave *slave)
{
	struct ark_spi_slave *arks = to_ark_spi_slave(slave);

	if(slave->cs == 0){
		if(arks->ss_pol)
			gpio_set_value(CONFIG_SPI_CS0_GPIO, 0);
		else
			gpio_set_value(CONFIG_SPI_CS0_GPIO, 1);
	}else if(slave->cs == 1){

	}
}

u32 get_cspi_div(u32 div)
{
	int i;

	for (i = 0; i < 8; i++) {
		if (div <= (4 << i))
			return i;
	}
	return i;
}

static s32 spi_cfg_ark(struct ark_spi_slave *arks, unsigned int cs,
		unsigned int max_hz, unsigned int mode)
{
	u32 clk_src = ark_get_syspll_clk();
	s32 pre_div = 0, post_div = 0, i, reg_ctrl, reg_config;
	struct cspi_regs *regs = (struct cspi_regs *)arks->base;

	if (max_hz == 0) {
		printf("Error: desired clock is 0\n");
		return -1;
	}

	//select syspll clk src
 #if defined (CONFIG_ARKN141FAMILY)
 #define rSYS_DEVICE_CLK_CFG0		*((volatile unsigned int *)(0x40408060))
	rSYS_DEVICE_CLK_CFG0 &= ~(0xFF << 16);
	rSYS_DEVICE_CLK_CFG0 |= (1 << 20);
#elif defined(CONFIG_ARK1668FAMILY)
#define rSYS_DEVICE_CLK_CFG0		*((volatile unsigned int *)(0xe4900060))
	rSYS_DEVICE_CLK_CFG0 &= ~(0xFF << 16);
	rSYS_DEVICE_CLK_CFG0 |= (1 << 20) | (2 << 16);
	clk_src = clk_src / (2 + 1) / 2;
#endif

	if(cs == 0){
#ifdef CONFIG_DM_GPIO
			gpio_request(CONFIG_SPI_CS0_GPIO, "cs0");
#endif
		/* Program GPIO 100 to Output mode and Set Output to HIGH */
		if (mode & SPI_CS_HIGH)
			gpio_direction_output(CONFIG_SPI_CS0_GPIO, 0);
		else
			gpio_direction_output(CONFIG_SPI_CS0_GPIO, 1);
	}else if(cs == 1){

	}

	reg_ctrl = reg_read(&regs->ctrl);

	/* Reset spi */
	reg_write(&regs->ctrl, 0);
	reg_write(&regs->ctrl, (reg_ctrl | ARK_ECSPI_CTRL_EN));

	/*
	 * The following computation is taken directly from Freescale's code.
	 */
	if (clk_src > max_hz) {
		pre_div = clk_src / max_hz;
		if (pre_div > 16) {
			post_div = pre_div / 16;
			pre_div = 15;
		}
		if (post_div != 0) {
			for (i = 0; i < 16; i++) {
				if ((1 << i) >= post_div)
					break;
			}
			if (i == 16) {
				printf("Error: no divider for the freq: %d\n",
					max_hz);
				return -1;
			}
			post_div = i;
		}
	}

	debug("pre_div = %d, post_div=%d\n", pre_div, post_div);
	reg_ctrl = (reg_ctrl & ~ARK_ECSPI_CTRL_SELCHAN(3)) |
		ARK_ECSPI_CTRL_SELCHAN(cs);
	reg_ctrl = (reg_ctrl & ~ARK_ECSPI_CTRL_PREDIV(0x0F)) |
		ARK_ECSPI_CTRL_PREDIV(pre_div);
	reg_ctrl = (reg_ctrl & ~ARK_ECSPI_CTRL_POSTDIV(0x0F)) |
		ARK_ECSPI_CTRL_POSTDIV(post_div);

	/* always set to master mode */
	reg_ctrl |= ARK_ECSPI_CTRL_MODE(cs);

	/* We need to disable SPI before changing registers */
	reg_ctrl &= ~ARK_ECSPI_CTRL_EN;

	reg_config = reg_read(&regs->cfg);

	reg_config = (reg_config & ~ARK_ECSPI_CONFIG_SSBPOL(cs));
	reg_config = (reg_config & ~ARK_ECSPI_CONFIG_SCLKPOL(cs));
	reg_config = (reg_config & ~ARK_ECSPI_CONFIG_SCLKPHA(cs));

	if (mode & SPI_CS_HIGH)
		reg_config |= ARK_ECSPI_CONFIG_SSBPOL(cs);

	if (mode & SPI_CPOL)
		reg_config |= ARK_ECSPI_CONFIG_SCLKPOL(cs);

	if (mode & SPI_CPHA)
		reg_config |= ARK_ECSPI_CONFIG_SCLKPHA(cs);

	reg_config |= ARK_ECSPI_CONFIG_SBBCTRL(cs);

	debug("reg_ctrl = 0x%x\n", reg_ctrl);
	reg_write(&regs->ctrl, reg_ctrl);
	debug("reg_config = 0x%x\n", reg_config);
	reg_write(&regs->cfg, reg_config);

	/* save config register and control register */
	arks->ctrl_reg = reg_ctrl;
	arks->cfg_reg = reg_config;

	/* clear interrupt reg */
	reg_write(&regs->intr, 0);
	reg_write(&regs->stat, ARK_ECSPI_STAT_TC | ARK_ECSPI_STAT_RO | ARK_ECSPI_STAT_REN);

	return 0;
}

int spi_xchg_single(struct spi_slave *slave, unsigned int bitlen,
	const u8 *dout, u8 *din, unsigned long flags)
{
	struct ark_spi_slave *arks = to_ark_spi_slave(slave);
	size_t nbytes = (bitlen + 7) / 8;
	u32 data, cnt, i;
	struct cspi_regs *regs = (struct cspi_regs *)arks->base;

	debug("%s: bitlen %d dout 0x%x din 0x%x\n",
		__func__, bitlen, (u32)dout, (u32)din);

	arks->ctrl_reg = (arks->ctrl_reg &
		~ARK_ECSPI_CTRL_BITCOUNT(ARK_ECSPI_CTRL_MAXBITS)) |
		ARK_ECSPI_CTRL_BITCOUNT(bitlen - 1);

	reg_write(&regs->ctrl, arks->ctrl_reg | ARK_ECSPI_CTRL_EN);
	reg_write(&regs->cfg, arks->cfg_reg);

	/* Clear interrupt register */
	reg_write(&regs->stat, ARK_ECSPI_STAT_TC | ARK_ECSPI_STAT_RO | ARK_ECSPI_STAT_REN);

	/*
	 * The SPI controller works only with words,
	 * check if less than a word is sent.
	 * Access to the FIFO is only 32 bit
	 */
	if (bitlen % 32) {
		data = 0;
		cnt = (bitlen % 32) / 8;
		if (dout) {
			for (i = 0; i < cnt; i++) {
#ifdef CONFIG_ARCH_ARKE
				data |= (*dout++ & 0xFF) << (8 * i);
#else
				data = (data >> 8) | ((*dout++ & 0xFF) << 24);
#endif
			}
		}
		debug("Sending SPI 0x%x\n", data);

		reg_write(&regs->txdata, data);
		nbytes -= cnt;
	}

	data = 0;

	while (nbytes > 0) {
		data = 0;
		if (dout) {
			/* Buffer is not 32-bit aligned */
			if ((unsigned long)dout & 0x03) {
				data = 0;
				for (i = 0; i < 4; i++)
#ifdef CONFIG_ARCH_ARKE
					data |= (*dout++ & 0xFF) << (8 * i);
#else
					data = (data >> 8) | ((*dout++ & 0xFF) << 24);
#endif
			} else {
				data = *(u32 *)dout;
			}
			dout += 4;
		}
		debug("Sending SPI 0x%x\n", data);
		reg_write(&regs->txdata, data);
		nbytes -= 4;
	}

	/* FIFO is written, now starts the transfer setting the XCH bit */
	reg_write(&regs->ctrl, arks->ctrl_reg |
		ARK_ECSPI_CTRL_EN | ARK_ECSPI_CTRL_XCH);

	/* Wait until the TC (Transfer completed) bit is set */
	while ((reg_read(&regs->stat) & ARK_ECSPI_STAT_TC) == 0)
		;


	nbytes = (bitlen + 7) / 8;

	cnt = nbytes % 32;

	if (bitlen % 32) {
		data = reg_read(&regs->rxdata);
		cnt = (bitlen % 32) / 8;
#ifndef CONFIG_ARCH_ARKE
		data = data >> ((sizeof(data) - cnt) * 8);
#endif
		debug("SPI Rx unaligned: 0x%x\n", data);
		if (din) {
			memcpy(din, &data, cnt);
			din += cnt;
		}
		nbytes -= cnt;
	}

	while (nbytes > 0) {
		u32 tmp;
		tmp = reg_read(&regs->rxdata);
		data = tmp;
		debug("SPI Rx: 0x%x 0x%x\n", tmp, data);
		cnt = min(nbytes, sizeof(data));
		if (din) {
			memcpy(din, &data, cnt);
			din += cnt;
		}
		nbytes -= cnt;
	}

	/* Transfer completed, clear any pending request */
	reg_write(&regs->stat, ARK_ECSPI_STAT_TC | ARK_ECSPI_STAT_RO);

	return 0;

}

int spi_xfer(struct spi_slave *slave, unsigned int bitlen, const void *dout,
		void *din, unsigned long flags)
{
	int n_bytes = (bitlen + 7) / 8;
	int n_bits;
	int ret;
	u32 blk_size;
	u8 *p_outbuf = (u8 *)dout;
	u8 *p_inbuf = (u8 *)din;

	if (!slave)
		return -1;

	if (flags & SPI_XFER_BEGIN)
		spi_cs_activate(slave);

	while (n_bytes > 0) {
		if (n_bytes < MAX_SPI_BYTES)
			blk_size = n_bytes;
		else
			blk_size = MAX_SPI_BYTES;

		n_bits = blk_size * 8;

		ret = spi_xchg_single(slave, n_bits, p_outbuf, p_inbuf, 0);

		if (ret)
			return ret;
		if (dout)
			p_outbuf += blk_size;
		if (din)
			p_inbuf += blk_size;
		n_bytes -= blk_size;
	}

	if (flags & SPI_XFER_END) {
		spi_cs_deactivate(slave);
	}

	return 0;
}

void spi_init(void)
{
}

struct spi_slave *spi_setup_slave(unsigned int bus, unsigned int cs,
			unsigned int max_hz, unsigned int mode)
{
	struct ark_spi_slave *arks;
	int ret;

	if (bus >= ARRAY_SIZE(spi_bases))
		return NULL;

	//if (ark_spi_ctxs)
	//	return &ark_spi_ctxs->slave;

	arks = spi_alloc_slave(struct ark_spi_slave, bus, cs);
	if (!arks) {
		puts("ark_spi: SPI Slave not allocated !\n");
		return NULL;
	}

	arks->base = spi_bases[bus];
	arks->ss_pol = (mode & SPI_CS_HIGH) ? 1 : 0;
	//ark_spi_ctxs = arks;

	ret = spi_cfg_ark(arks, cs, max_hz, mode);
	if (ret) {
		printf("ark_spi: cannot setup SPI controller\n");
		free(arks);
		return NULL;
	}

	return &arks->slave;
}

void spi_free_slave(struct spi_slave *slave)
{
	struct ark_spi_slave *arks = to_ark_spi_slave(slave);

	free(arks);
	//ark_spi_ctxs = NULL;
}

int spi_claim_bus(struct spi_slave *slave)
{
	struct ark_spi_slave *arks = to_ark_spi_slave(slave);
	struct cspi_regs *regs = (struct cspi_regs *)arks->base;

	reg_write(&regs->ctrl, arks->ctrl_reg);
	reg_write(&regs->intr, 0);

	return 0;
}

void spi_release_bus(struct spi_slave *slave)
{
	/* TODO: Shut the controller down */
}
