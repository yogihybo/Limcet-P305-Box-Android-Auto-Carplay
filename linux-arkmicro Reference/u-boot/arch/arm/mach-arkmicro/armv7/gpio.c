#include <common.h>
#include <asm/io.h>

#define GPIO_MOD			0x00
#define	GPIO_RDATA			0x04
#define GPIO_INTEN			0x08
#define GPIO_INTLEVEL		0x0C
#define GPIO_INTGEN			0x10
#define GPIO_DEBOUNCE_EN	0xA0
#define GPIO_DEBOUNCE_CNT0	0x80

#define GPIO_BANK_NUM       32

static unsigned long gpio_bases[] = {
	CONFIG_GPIO_BASEADDR,
    CONFIG_GPIO_BASEADDR + 0x20,
    CONFIG_GPIO_BASEADDR + 0x40,
    CONFIG_GPIO_BASEADDR + 0x60,
};

static inline int GPIO_BANK(unsigned gpio)
{
    return gpio >> 5;
}

static inline int GPIO_OFFSET(unsigned gpio)
{
    return gpio & 0x1F;
}

static inline void *GPIO_MODREG(unsigned gpio)
{
    return (void*)(gpio_bases[GPIO_BANK(gpio)] + GPIO_MOD);
}

static inline void *GPIO_RDATAREG(unsigned gpio)
{
    return (void*)(gpio_bases[GPIO_BANK(gpio)] + GPIO_RDATA);
}

int gpio_request(unsigned gpio, const char *label)
{
	return 0;
}

int gpio_free(unsigned gpio)
{
	return 0;
}

int gpio_direction_input(unsigned gpio)
{
    writel(readl(GPIO_MODREG(gpio)) | (1 << GPIO_OFFSET(gpio)), GPIO_MODREG(gpio));

    return 0;
}

int gpio_direction_output(unsigned gpio, int value)
{
    writel(readl(GPIO_MODREG(gpio)) & ~(1 << GPIO_OFFSET(gpio)), GPIO_MODREG(gpio));
    if (value)
        writel(readl(GPIO_RDATAREG(gpio)) | (1 << GPIO_OFFSET(gpio)), GPIO_RDATAREG(gpio));
    else
        writel(readl(GPIO_RDATAREG(gpio)) & ~(1 << GPIO_OFFSET(gpio)), GPIO_RDATAREG(gpio));

    return 0;
}

int gpio_get_value(unsigned gpio)
{
    return !!(readl(GPIO_RDATAREG(gpio)) & (1 << GPIO_OFFSET(gpio)));
}

int gpio_set_value(unsigned gpio, int value)
{
    if (value)
        writel(readl(GPIO_RDATAREG(gpio)) | (1 << GPIO_OFFSET(gpio)), GPIO_RDATAREG(gpio));
    else
        writel(readl(GPIO_RDATAREG(gpio)) & ~(1 << GPIO_OFFSET(gpio)), GPIO_RDATAREG(gpio));

    return 0;
}
