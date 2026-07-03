// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <spl.h>

u32 spl_boot_device(void)
{
#if defined(CONFIG_SYS_USE_MMC) || defined(CONFIG_SD_BOOT)
	return BOOT_DEVICE_MMC1;
#elif defined(CONFIG_SYS_USE_NANDFLASH) || defined(CONFIG_NAND_BOOT)
	return BOOT_DEVICE_NAND;
#elif defined(CONFIG_SYS_USE_SERIALFLASH) || \
	defined(CONFIG_SYS_USE_SPIFLASH) || \
	defined(CONFIG_SPI_BOOT)
	return BOOT_DEVICE_SPI;
#endif
	return BOOT_DEVICE_NONE;
}
