#include "musb_core.h"
#define rSYS_SOFT_RSTNA			*((volatile unsigned int *)(0xe4900074))
void ark_usb_controller_reset(void)
{
	printf("ark_usb_phy_reset\n");
	rSYS_SOFT_RSTNA &= ~(3 << 5);
	udelay(100);
	rSYS_SOFT_RSTNA |= 3 << 5;
	udelay(10);
}
static int ark_musb_init(struct musb *musb)
{
	return 0;
}
static int ark_musb_enable(struct musb *musb)
{
	return 0;
}
static void ark_musb_disable(struct musb *musb)
{

}
static int ark_musb_exit(struct musb *musb)
{
	return 0;
}
const struct musb_platform_ops ark_musb_ops = {
	.init		= ark_musb_init,
	.exit		= ark_musb_exit,
	.enable		= ark_musb_enable,
	.disable	= ark_musb_disable,
};
