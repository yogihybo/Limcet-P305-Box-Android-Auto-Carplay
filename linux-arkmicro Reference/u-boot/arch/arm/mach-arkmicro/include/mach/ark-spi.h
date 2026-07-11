#ifndef ARK_SPI_H
#define	ARK_SPI_H

/*
 * CSPI register definitions
 */
#define ARK_ECSPI_CTRL_EN	(1 << 0)
#define ARK_ECSPI_CTRL_HW	(1 << 1)
#define ARK_ECSPI_CTRL_XCH	(1 << 2)
#define ARK_ECSPI_CTRL_SMC	(1 << 3)
#define ARK_ECSPI_CTRL_MODE(x)	(1 << ((x) + 4))
#define ARK_ECSPI_CTRL_BITCOUNT(x)	(((x) & 0xfff) << 20)
#define ARK_ECSPI_CTRL_PREDIV(x)	(((x) & 0xF) << 12)
#define ARK_ECSPI_CTRL_POSTDIV(x)	(((x) & 0xF) << 8)
#define ARK_ECSPI_CTRL_SELCHAN(x)	(((x) & 0x3) << 18)
#define ARK_ECSPI_CTRL_MAXBITS		0xfff

#define ARK_ECSPI_CONFIG_SCLKPHA(cs)    (1 << ((cs) +  0))
#define ARK_ECSPI_CONFIG_SCLKPOL(cs)    (1 << ((cs) +  4))
#define ARK_ECSPI_CONFIG_SBBCTRL(cs)    (1 << ((cs) +  8))
#define ARK_ECSPI_CONFIG_SSBPOL(cs)     (1 << ((cs) + 12))

#define ARK_ECSPI_STAT_TE               (1 << 0)
#define ARK_ECSPI_STAT_TDR              (1 << 1)
#define ARK_ECSPI_STAT_TF               (1 << 2)
#define ARK_ECSPI_STAT_RR               (1 << 3)
#define ARK_ECSPI_STAT_RDR              (1 << 4)
#define ARK_ECSPI_STAT_RF               (1 << 5)
#define ARK_ECSPI_STAT_RO               (1 << 6)
#define ARK_ECSPI_STAT_TC               (1 << 7)
#define ARK_ECSPI_STAT_REN              (1 << 8)

#define ARK_ECSPI_PERIOD_32KHZ		(1 << 15)
#define MAX_SPI_BYTES   32

/* CSPI registers */
struct cspi_regs {
    u32 reserve0[2];
    u32 ctrl;
    u32 cfg;
    u32 intr;
    u32 dma;
    u32 stat;
    u32 period;
	u32 test;
	u32 msg;
	u32 reserve1[10];
	u32 rxdata;
	u32 reserve2[259];
	u32 txdata;
};

#endif /* ARK_SPI_H */
