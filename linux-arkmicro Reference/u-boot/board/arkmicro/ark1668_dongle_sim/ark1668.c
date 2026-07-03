#include <common.h>
#include <dwmmc.h>
#include <malloc.h>
#include <debug_uart.h>
#include <asm-generic/gpio.h>
#include <asm/arch/ark-common.h>

DECLARE_GLOBAL_DATA_PTR;

#define ARK1668_UPDATE_MAGIC	"ada7f0c6-7c86-11e9-8f9e-2a86e4085a59"

#define rSYS_SD_CLK_CFG			*((volatile unsigned int *)(0xe4900058))
#define rSYS_SD1_CLK_CFG		*((volatile unsigned int *)(0xe490005c))
#define rSYS_SOFT_RSTNA			*((volatile unsigned int *)(0xe4900074))
#define rSYS_SOFT_RSTNB			*((volatile unsigned int *)(0xe4900078))
#define rSYS_DDR_STATUS			*((volatile unsigned int *)(0xe4900180))
#define rSYS_PAD_CTRL05			*((volatile unsigned int *)(0xe49001d4))
#define rSYS_PAD_CTRL06			*((volatile unsigned int *)(0xe49001d8))
#define rSYS_PAD_CTRL0B			*((volatile unsigned int *)(0xe49001ec))

#define rREMAP					(*(volatile unsigned int *)(0xe4400020))

static void dwmci_select_pad(void)
{
	unsigned int val;
	/* use sd/mmc 0 */
	val = rSYS_PAD_CTRL0B;
	val &= ~((0xF << 0) | (0x1 << 4));
	val |= ((0xF << 0) | (0x1 << 4));
	rSYS_PAD_CTRL0B = val;

	val = rSYS_PAD_CTRL05;
	val &= ~((0xF << 24) | (0xF << 28));
	val |= ((0x2 << 24) | (0x2 << 28));
	rSYS_PAD_CTRL05 = val;

	rSYS_SD_CLK_CFG = 0x00000420;

	/* use sd/mmc 1 */
	val = rSYS_PAD_CTRL06;
	val &= ~0x2222222;
	val |= 0x2222222;
	rSYS_PAD_CTRL06 = val;

	rSYS_SD1_CLK_CFG = 0x00000420;
}

static void dwmci_reset(void)
{
	rSYS_SOFT_RSTNA &= ~((1 << 29) | (1 << 16));
	rSYS_SOFT_RSTNB &= ~(1 << 3);
	udelay(100);
	rSYS_SOFT_RSTNA |= ((1 << 29) | (1 << 16));
	rSYS_SOFT_RSTNB |= (1 << 3);
}

#define ARK_MMC_CLK     	24000000
int ark_dwmci_init(char *name, u32 regbase, int bus_width, int index)
{
	struct dwmci_host *host = NULL;
	host = malloc(sizeof(struct dwmci_host));
	if (!host) {
		printf("dwmci_host malloc fail!\n");
		return 1;
	}
	memset(host, 0, sizeof(struct dwmci_host));

	dwmci_select_pad();
	dwmci_reset();

	host->name = name;
	host->ioaddr = (void *)regbase;
	host->buswidth = bus_width;
	host->dev_index = index;
	host->bus_hz = ARK_MMC_CLK;
	host->fifo_mode = 1;

	add_dwmci(host, host->bus_hz, 400000);

	return 0;
}

int board_mmc_init(bd_t * bis)
{
	ark_dwmci_init("ARK_MMC0", 0xec400000, 4, 0);
	ark_dwmci_init("ARK_MMC1", 0xec800000, 4, 0);
	//ark_dwmci_init("ARK_MMC2",SDHC2_BASE, 4, 2);

	return 0;
}

int dram_init(void)
{
	gd->ram_size = get_ram_size((void *)CONFIG_SYS_SDRAM_BASE, CONFIG_SYS_SDRAM_SIZE);

	return 0;
}

int board_init(void)
{
	return 0;
}

#ifdef CONFIG_BOARD_EARLY_INIT_F
int board_early_init_f(void)
{
#ifdef CONFIG_DEBUG_UART
	debug_uart_init();
#endif
	return 0;
}
#endif

int board_late_init(void)
{
	char cmd[128];
	char *need_update,*update_flash;
	unsigned int loadaddr;
	int do_update = 0, update_from_mmc = 1;
	update_flash = env_get("update_from_flash");
        printf("++++++++++%s+++++++\n",update_flash);
	need_update = env_get("need_update");
	if (!strcmp(need_update, "yes")) {
		loadaddr = env_get_hex("loadaddr", 0);

		sprintf(cmd, "fatload %s %s %s update-magic", "mmc", env_get("sd_dev_part"), env_get("loadaddr"));
		run_command(cmd, 0);
		if (loadaddr && !memcmp((void *)loadaddr, ARK1668_UPDATE_MAGIC, strlen(ARK1668_UPDATE_MAGIC))) {
			do_update = 1;
			goto update_done;
		} else {
			printf("Wrong update magic, do not update from mmc.\n");
		}

#ifdef CONFIG_USB_MUSB_HCD
		//use old musb driver
		run_command("usb start", 0);
#endif
		sprintf(cmd, "fatload %s %s %s update-magic", "usb", "0", env_get("loadaddr"));
		run_command(cmd, 0);
		if (loadaddr && !memcmp((void *)loadaddr, ARK1668_UPDATE_MAGIC, strlen(ARK1668_UPDATE_MAGIC))) {
			do_update = 1;
			update_from_mmc = 0;
		} else {
			printf("Wrong update magic, do not update from usb.\n");
		}
	}	
	else if (!strcmp(update_flash, "yes")){

	    sprintf(cmd, "run updatefromflash");
		printf("cmd=%s\n", cmd);	
		run_command(cmd, 0);
 
	}

update_done:
	if (do_update) {
		run_command("nand erase.part userdata", 0);
		env_set("need_update", "no");
		env_set("do_update", "yes");
		if (update_from_mmc) {
			printf("update form mmc...\n");
			env_set("update_dev_type", "mmc");
			env_set("update_dev_part", env_get("sd_dev_part"));
		} else {
			printf("update form usb...\n");
			env_set("update_dev_type", "usb");
			env_set("update_dev_part", "0");
		}
	} else {
		env_set("do_update", "no");
	}

	return 0;
}

#ifdef CONFIG_SPL_BUILD
void mem_init(void)
{
	int ret;
	gpio_direction_output(8, 1);
	udelay(10);

reset:
	/* controller reset */
	rSYS_SOFT_RSTNA |= 1 << 8;
	rSYS_SOFT_RSTNB |= 0x3 << 10;
	udelay(1);
	rSYS_SOFT_RSTNA &= ~(1 << 8);
	rSYS_SOFT_RSTNB &= ~(0x3 << 10);
	udelay(10);
	rSYS_SOFT_RSTNA |= 1 << 8;
	rSYS_SOFT_RSTNB |= 0x3 << 10;
	udelay(10);

	ddr3_sdramc_init();
	udelay(1500);
	if (rSYS_DDR_STATUS & (1 << 2))	//train error
		goto reset;
	udelay(1);
	ret = ddr3_data_training(0);
	if (ret) {
		udelay(1);
		goto reset;
	}
	udelay(1);

	rREMAP = 1;
	udelay(10);
	printf("remap...\n");
}
#endif
