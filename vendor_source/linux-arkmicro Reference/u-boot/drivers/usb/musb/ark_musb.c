#include <common.h>
#include "musb_core.h"
#include "ark_musb.h"
#include <asm/gpio.h>

/* MUSB platform configuration */
struct musb_config musb_cfg = {
#if 0
#if CONFIG_USB_DEV_PART == 0
	.regs       = (struct musb_regs *)MUSB_ARK_USB_BASE,
#elif CONFIG_USB_DEV_PART == 1
	.regs       = (struct musb_regs *)MUSB_ARK_USB1_BASE,
#endif
#endif
	.regs       = (struct musb_regs *)MUSB_ARK_USB_BASE,
	.timeout    = 400000,//0x3FFFFFF,
	.musb_speed = 0,
};

#if 0
static void set_usb_power(unsigned int Enable)
{
	unsigned int reg;

	//GPIO126
	reg = (*(unsigned int *)(MUSB_ARK_SYS_BASE+0x1f0));
	reg &= ~(0x1<<(126-117));
	(*(unsigned int *)(MUSB_ARK_SYS_BASE+0x1f0)) = reg;

	//Set GPIO OutPut
	(*(unsigned int *)(MUSB_ARK_GPIO_BASE+0x60)) &= ~(1<<(126-96));

	//Set GPIO Data
	if(Enable)
	{
		(*(unsigned int *)(MUSB_ARK_GPIO_BASE+0x64)) |= (1<<(126-96));
	}
	else
	{
		(*(unsigned int *)(MUSB_ARK_GPIO_BASE+0x64)) &= ~(1<<(126-96));
	}	
}
#endif

static void reset_usb_phy(int index)
{
	unsigned int reg;

	//set_usb_power(0);
	//mdelay(20);
	//set_usb_power(1);
	
	if(index == 0) {
		reg = (*(unsigned int *)(MUSB_ARK_SYS_BASE+0x74));
		reg &= ~(0x3<<5);
		(*(unsigned int *)(MUSB_ARK_SYS_BASE+0x74)) = reg;
		
		udelay(20);
		
		reg |= (0x3<<5);
		(*(unsigned int *)(MUSB_ARK_SYS_BASE+0x74)) = reg;
	}else if(index == 1) {
		reg = (*(unsigned int *)(MUSB_ARK_SYS_BASE+0x78));
		reg &= ~(0x3<<6);
		(*(unsigned int *)(MUSB_ARK_SYS_BASE+0x78)) = reg;
		
		udelay(20);
		
		reg |= (0x3<<6);
		(*(unsigned int *)(MUSB_ARK_SYS_BASE+0x78)) = reg;	
	}
}

/*
 * CPU and board-specific MUSB initializations.  Aliased function
 * signals caller to move on.
 */
static void musb_ark_init(void)
{
	char *ch = CONFIG_USB_DEV_PART;
	int index = 0;

	index = ch ? (*ch - '0') : 0;

	if(index == 1)
		musb_cfg.regs = (struct musb_regs *)MUSB_ARK_USB1_BASE;

	if(CONFIG_USB_GPIO_SW >= 0)
		gpio_direction_output(CONFIG_USB_GPIO_SW, 0);
	if(CONFIG_USB_GPIO_PWR >= 0)
		gpio_direction_output(CONFIG_USB_GPIO_PWR, 1);
	if(CONFIG_USB_GPIO_ID >= 0)
		gpio_direction_output(CONFIG_USB_GPIO_ID, 0);
	udelay(20000);

	reset_usb_phy(index);
}

void board_musb_init(void) __attribute__((weak, alias("musb_ark_init")));

int musb_platform_init(void)
{
	/* board specific initialization */
	board_musb_init();

	return 0;
}

/*
 * This function performs platform specific deinitialization for usb.
*/
void musb_platform_deinit(void)
{
}
