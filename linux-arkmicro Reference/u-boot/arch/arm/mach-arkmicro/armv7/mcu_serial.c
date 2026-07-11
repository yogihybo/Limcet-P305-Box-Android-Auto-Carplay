#include <common.h>
#include <errno.h>
#include <watchdog.h>
#include <asm/io.h>

struct pl01x_regs {
	u32	dr;		/* 0x00 Data register */
	u32	ecr;		/* 0x04 Error clear register (Write) */
	u32	pl010_lcrh;	/* 0x08 Line control register, high byte */
	u32	pl010_lcrm;	/* 0x0C Line control register, middle byte */
	u32	pl010_lcrl;	/* 0x10 Line control register, low byte */
	u32	pl010_cr;	/* 0x14 Control register */
	u32	fr;		/* 0x18 Flag register (Read only) */
#ifdef CONFIG_PL011_SERIAL_RLCR
	u32	pl011_rlcr;	/* 0x1c Receive line control register */
#else
	u32	reserved;
#endif
	u32	ilpr;		/* 0x20 IrDA low-power counter register */
	u32	pl011_ibrd;	/* 0x24 Integer baud rate register */
	u32	pl011_fbrd;	/* 0x28 Fractional baud rate register */
	u32	pl011_lcrh;	/* 0x2C Line control register */
	u32	pl011_cr;	/* 0x30 Control register */
};

#define UART_PL01x_RSR_OE               0x08
#define UART_PL01x_RSR_BE               0x04
#define UART_PL01x_RSR_PE               0x02
#define UART_PL01x_RSR_FE               0x01

#define UART_PL01x_FR_TXFE              0x80
#define UART_PL01x_FR_RXFF              0x40
#define UART_PL01x_FR_TXFF              0x20
#define UART_PL01x_FR_RXFE              0x10
#define UART_PL01x_FR_BUSY              0x08
#define UART_PL01x_FR_TMSK              (UART_PL01x_FR_TXFF + UART_PL01x_FR_BUSY)

/*
 *  PL011 definitions
 *
 */
#define UART_PL011_LCRH_SPS             (1 << 7)
#define UART_PL011_LCRH_WLEN_8          (3 << 5)
#define UART_PL011_LCRH_WLEN_7          (2 << 5)
#define UART_PL011_LCRH_WLEN_6          (1 << 5)
#define UART_PL011_LCRH_WLEN_5          (0 << 5)
#define UART_PL011_LCRH_FEN             (1 << 4)
#define UART_PL011_LCRH_STP2            (1 << 3)
#define UART_PL011_LCRH_EPS             (1 << 2)
#define UART_PL011_LCRH_PEN             (1 << 1)
#define UART_PL011_LCRH_BRK             (1 << 0)

#define UART_PL011_CR_CTSEN             (1 << 15)
#define UART_PL011_CR_RTSEN             (1 << 14)
#define UART_PL011_CR_OUT2              (1 << 13)
#define UART_PL011_CR_OUT1              (1 << 12)
#define UART_PL011_CR_RTS               (1 << 11)
#define UART_PL011_CR_DTR               (1 << 10)
#define UART_PL011_CR_RXE               (1 << 9)
#define UART_PL011_CR_TXE               (1 << 8)
#define UART_PL011_CR_LPE               (1 << 7)
#define UART_PL011_CR_IIRLP             (1 << 2)
#define UART_PL011_CR_SIREN             (1 << 1)
#define UART_PL011_CR_UARTEN            (1 << 0)

#define UART_PL011_IMSC_OEIM            (1 << 10)
#define UART_PL011_IMSC_BEIM            (1 << 9)
#define UART_PL011_IMSC_PEIM            (1 << 8)
#define UART_PL011_IMSC_FEIM            (1 << 7)
#define UART_PL011_IMSC_RTIM            (1 << 6)
#define UART_PL011_IMSC_TXIM            (1 << 5)
#define UART_PL011_IMSC_RXIM            (1 << 4)
#define UART_PL011_IMSC_DSRMIM          (1 << 3)
#define UART_PL011_IMSC_DCDMIM          (1 << 2)
#define UART_PL011_IMSC_CTSMIM          (1 << 1)
#define UART_PL011_IMSC_RIMIM           (1 << 0)

#if  defined(CONFIG_ARK1668FAMILY)
#define rSYS_PAD_CTRL08			*((volatile unsigned int *)(0xe49001e0))
#elif defined(CONFIG_ARKN141FAMILY)
#define rSYS_PAD_CTRL09			*((volatile unsigned int *)(0x404081e4))
#define rSYS_APB_CLK_EN			*((volatile unsigned int *)(0x40408048))
#define rSYS_PER_CLK_EN			*((volatile unsigned int *)(0x40408050))
#endif
static volatile unsigned char *const port[] = CONFIG_PL01x_PORTS;
static struct pl01x_regs *base_regs;

static int pl01x_putc(struct pl01x_regs *regs, unsigned char c)
{
	/* Wait until there is space in the FIFO */
	if (readl(&regs->fr) & UART_PL01x_FR_TXFF)
		return -EAGAIN;

	/* Send the character */
	writel(c, &regs->dr);

	return 0;
}

static int pl01x_getc(struct pl01x_regs *regs)
{
	unsigned int data;

	/* Wait until there is data in the FIFO */
	if (readl(&regs->fr) & UART_PL01x_FR_RXFE)
		return -EAGAIN;

	data = readl(&regs->dr);

	/* Check for an error flag */
	if (data & 0xFFFFFF00) {
		/* Clear the error */
		writel(0xFFFFFFFF, &regs->ecr);
		return -1;
	}

	return (int) data;
}

static int pl01x_generic_serial_init(struct pl01x_regs *regs)
{
	/* disable everything */
	writel(0, &regs->pl011_cr);

	return 0;
}

static int pl011_set_line_control(struct pl01x_regs *regs)
{
	unsigned int lcr;
	/*
	 * Internal update of baud rate register require line
	 * control register write
	 */
	lcr = UART_PL011_LCRH_WLEN_8 | UART_PL011_LCRH_FEN;
	writel(lcr, &regs->pl011_lcrh);
	return 0;
}

static int pl01x_generic_setbrg(struct pl01x_regs *regs, int clock, int baudrate)
{
	unsigned int temp;
	unsigned int divider;
	unsigned int remainder;
	unsigned int fraction;

	/*
	* Set baud rate
	*
	* IBRD = UART_CLK / (16 * BAUD_RATE)
	* FBRD = RND((64 * MOD(UART_CLK,(16 * BAUD_RATE)))
	*		/ (16 * BAUD_RATE))
	*/
	temp = 16 * baudrate;
	divider = clock / temp;
	remainder = clock % temp;
	temp = (8 * remainder) / baudrate;
	fraction = (temp >> 1) + (temp & 1);

	writel(divider, &regs->pl011_ibrd);
	writel(fraction, &regs->pl011_fbrd);

	pl011_set_line_control(regs);
	/* Finally, enable the UART */
	writel(UART_PL011_CR_UARTEN | UART_PL011_CR_TXE |
	       UART_PL011_CR_RXE | UART_PL011_CR_RTS, &regs->pl011_cr);

	return 0;
}

static void pl01x_serial_init_baud(int baudrate)
{
	int clock = 0;

	clock = CONFIG_PL011_CLOCK;

	base_regs = (struct pl01x_regs *)port[CONFIG_MCU_SERIAL_PORT];

	pl01x_generic_serial_init(base_regs);
	pl01x_generic_setbrg(base_regs, clock, baudrate);
}

int mcu_serial_init(void)
{
#if  defined(CONFIG_ARK1668FAMILY)
	//uartx pad select
	int offset = CONFIG_MCU_SERIAL_PORT * 4;
	unsigned int val = rSYS_PAD_CTRL08;
	val &= ~(0xf << offset);
	val |= (1 << (offset + 2)) | (1 << offset);
	rSYS_PAD_CTRL08 = val;
#elif defined(CONFIG_ARKN141FAMILY)
	//uartx pad select
	int offset = CONFIG_MCU_SERIAL_PORT * 4 + 16;
	unsigned int val = rSYS_PAD_CTRL09;
	val &= ~(0xf << offset);
	val |= (1 << (offset + 2)) | (1 << offset);
	rSYS_PAD_CTRL09 = val;

	//uartx APB CLK enable
	offset = CONFIG_MCU_SERIAL_PORT + 7;
	val = rSYS_APB_CLK_EN;
	val |= (1 << offset);
	rSYS_APB_CLK_EN = val;

	//uartx UART CLK enable
	offset = CONFIG_MCU_SERIAL_PORT + 19;
	val = rSYS_PER_CLK_EN;
	val |= (1 << offset);
	rSYS_PER_CLK_EN = val;
#endif
	pl01x_serial_init_baud(CONFIG_MCU_SERIAL_BAUDRATE);

	return 0;
}

void mcu_serial_putc(const unsigned char c)
{
	while (pl01x_putc(base_regs, c) == -EAGAIN);
}

void mcu_serial_puts(const unsigned char *s)
{
	while (*s)
		mcu_serial_putc(*s++);
}

int mcu_serial_getc(void)
{
	while (1) {
		int ch = pl01x_getc(base_regs);

		if (ch == -EAGAIN) {
			WATCHDOG_RESET();
			continue;
		}

		return ch;
	}
}

void mcu_serial_send(const unsigned char *buf, int len)
{
	int i;
	
	for (i = 0; i < len; i++)
		mcu_serial_putc(buf[i]);
}

int mcu_serial_receive(unsigned char *buf, int len)
{
	int length = 0;
	int i;

	for (i = 0; i < len; i++) {
		int ch = pl01x_getc(base_regs);
		if (ch == -EAGAIN) {
			break;
		}
		buf[length++] = ch;
	}

	return length;
}

int mcu_serial_read(unsigned char *buf) 
{
	int len = 0;

	while (1) {
		int ch = pl01x_getc(base_regs);

		if (ch == -EAGAIN) {
			break;
		}

		buf[len++] = ch;
	}

	return len;
}

