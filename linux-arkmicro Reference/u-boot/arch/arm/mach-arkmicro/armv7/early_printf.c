#include <common.h>
#include <stdarg.h>
#include <asm/io.h>

#define UART_BASE	CONFIG_DEBUG_UART_BASE
#define UART_DR		0x00
#define UART_FR		0x18


static void uart_puts(const char *str)
{
	int i = 0;

	while (str[i] != 0) {
		while (!(readl(UART_BASE + UART_FR) & 0x20)) {
			if (str[i] == '\n') {
				writel('\r', UART_BASE + UART_DR);
				while (readl(UART_BASE + UART_FR) & 0x20);
			}
			writel(str[i++], UART_BASE + UART_DR);
			if (str[i] == 0)
				return;
		}
		while (readl(UART_BASE + UART_FR) & 0x20);
	}	
}

int early_printf(const char *fmt, ...)
{
    va_list args;
    uint i;
    char printbuffer[CONFIG_SYS_PBSIZE];

    va_start(args, fmt);

    /*  
     * For this to work, printbuffer must be larger than
     * anything we ever want to print.
     */
    i = vscnprintf(printbuffer, sizeof(printbuffer), fmt, args);
    va_end(args);

    /* Handle error */
    if (i <= 0)
        return i;
    /* Print the string */
    uart_puts(printbuffer);
    return i;
}

