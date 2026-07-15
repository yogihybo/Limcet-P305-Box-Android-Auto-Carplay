/*
 * (C) Copyright 2012 Stephen Warren
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <linux/sizes.h>

/* L2 Cache(pl310) */
#ifndef CONFIG_SYS_L2CACHE_OFF
#define CONFIG_SYS_L2_PL310
#define CONFIG_SYS_PL310_BASE	0x70000000
#endif

/* Memory layout */
#define CONFIG_NR_DRAM_BANKS			1
#define CONFIG_SYS_SDRAM_BASE		0x00000000
#define CONFIG_SYS_SDRAM_SIZE		SZ_64M
/*
#define CONFIG_SYS_MALLOC_LEN		0x80000
*/

#ifdef CONFIG_SPL_BUILD
#define CONFIG_SYS_INIT_SP_ADDR		0xc0008000
#else
#define CONFIG_SYS_INIT_SP_ADDR \
	(CONFIG_SYS_SDRAM_BASE + 16 * 1024 - GENERATED_GBL_DATA_SIZE)
#endif

/* SPL */
#define CONFIG_SPL_TEXT_BASE		0xc0000000
#define CONFIG_SPL_MAX_SIZE			0x8000

/* timer base address */
#define CONFIG_SYS_TIMERBASE		0xe4a00000

/* watchdog base address */
#define CONFIG_WATCHDOG_BASEADDR	0xe4b00000

/*
 * NAND Flash controller Configuration
 */
#define CONFIG_SYS_USE_NANDFLASH
#define CONFIG_SYS_NAND_BASE		0xec000000
#define CONFIG_SYS_MAX_NAND_DEVICE      1
#define CONFIG_SYS_NAND_ONFI_DETECTION
#define CONFIG_SYS_NAND_USE_FLASH_BBT
#define CONFIG_MTD_PARTITIONS
#define CONFIG_MTD_DEVICE
#define NANDARGS \
	"mtdids=" CONFIG_MTDIDS_DEFAULT "\0" \
	"mtdparts=" CONFIG_MTDPARTS_DEFAULT "\0" \
	"bootstrapupdate=if fatload ${update_dev_type} ${update_dev_part} ${loadaddr} ubootspl.bin; " \
		"then nand erase.part bootstrap; " \
		"switchecc 1; " \
		"nand write ${loadaddr} bootstrap ${filesize}; " \
		"switchecc 0; fi\0" \
	"fdtupdate=if fatload ${update_dev_type} ${update_dev_part} ${loadaddr} "CONFIG_DEFAULT_FDT_FILE"; " \
		"then setenv fdtsize ${filesize}; " \
		"nand erase.part fdt; " \
		"nand write ${loadaddr} fdt ${filesize}; fi\0" \
	"kernelupdate=if fatload ${update_dev_type} ${update_dev_part} ${loadaddr} zImage; " \
		"then setenv kernelsize ${filesize}; " \
		"nand erase.part kernel; " \
		"nand write ${loadaddr} kernel ${filesize}; fi\0" \
/*	"bootloaderupdate=if fatload ${update_dev_type} ${update_dev_part} ${loadaddr} u-boot.img; " \
		"then setenv bootloadersize ${filesize}; " \
		"nand erase.part bootloader; " \
		"nand write ${loadaddr} bootloader ${filesize}; fi\0" \
*/	"bootloaderupdate=Updatebootloader ${update_dev_type} ${update_dev_part}; \0" \
	"updatefromflash=UpdateFlash 0 0; \0" \
	"rootfsupdate=if fatload ${update_dev_type} ${update_dev_part} ${loadaddr} rootfs.ubi; " \
		"then setenv rootfssize ${filesize}; " \
		"nand erase.part rootfs; " \
		"nand write ${loadaddr} rootfs ${filesize}; fi\0" \
	"bootanimationupdate=if fatload ${update_dev_type} ${update_dev_part} ${loadaddr} bootanimation; " \
		"then setenv bootanimationsize ${filesize}; " \
		"nand erase.part bootanimation; " \
		"nand write ${loadaddr} bootanimation ${filesize}; " \
		"else setenv bootanimationsize 0; fi\0" \
	"nandargs=setenv bootargs console=ttyS0,115200 " \
		"earlyprintk  loglevel=3 clk_ignore_unused lpj=2285568 enable_console " \
		"${mtdparts} " \
		"root=${nandroot} " \
		"rootfstype=${nandrootfstype}\0" \
	"nandroot=ubi0:rootfs rw ubi.mtd=rootfs ubi.fm_autoconvert=1\0" \
	"nandrootfstype=ubifs rootwait\0" \
	"nandboot=echo Booting from nand ...; " \
		"run nandargs; " \
		"nand read ${fdtaddr} fdt ${fdtsize}; " \
		"nand read ${kerneladdr} kernel ${kernelsize}; " \
		"nand read ${bootanimationaddr} bootanimation ${bootanimationsize}; " \
		"bootz ${kerneladdr} - ${fdtaddr}\0"

/* Environment */
#define CONFIG_ENV_SIZE			4096	//CONFIG_PARTITION_UBOOT_ENV_SIZE
/*
#define CONFIG_ENV_OFFSET		0xA0000
*/
#define CONFIG_ENV_OFFSET		0x160000	//0xA0000

#define CONFIG_SYS_LOAD_ADDR	(CONFIG_SYS_SDRAM_BASE + 0x1000000)

/* Console UART */
#define CONFIG_PL011_CLOCK		24000000
#define CONFIG_PL01x_PORTS      {(void *)CONFIG_SYS_SERIAL0, (void *)CONFIG_SYS_SERIAL1, (void *)CONFIG_SYS_SERIAL2, (void *)CONFIG_SYS_SERIAL3}
#define CONFIG_BAUDRATE			115200
#define CONFIG_SYS_SERIAL0      0xe4200000
#define CONFIG_SYS_SERIAL1      0xe4e00000
#define CONFIG_SYS_SERIAL2      0xe8000000
#define CONFIG_SYS_SERIAL3      0xe8100000

/* Console configuration */
#define CONFIG_SYS_CBSIZE		512
#define CONFIG_SYS_PBSIZE		(CONFIG_SYS_CBSIZE + sizeof(CONFIG_SYS_PROMPT) + 16)
#define CONFIG_BOUNCE_BUFFER
#ifdef CONFIG_NAND_BOOT
/*
#ifndef CONFIG_CMD_UBI
#define CONFIG_CMD_UBI
#endif
#ifndef CONFIG_CMD_UBIFS
#define CONFIG_CMD_UBIFS
#endif
*/

#define CONFIG_SYS_MALLOC_LEN     (CONFIG_ENV_SIZE+1024*1024)
#define CONFIG_SYS_GBL_DATA_SIZE   512

#define CONFIG_EXTRA_ENV_SETTINGS \
	"need_update=yes\0" \
	"update_dev_type=mmc\0" \
	"update_dev_part=0\0" \
	"sd_dev_part="CONFIG_SD_DEV_PART"\0" \
	"loadaddr=0x4000000\0" \
	"fdtaddr=0x2000000\0" \
	"kerneladdr=0x2100000\0" \
	"bootanimationaddr=0xfc00000\0" \
	"bootanimationsize=0\0" \
	NANDARGS

#define CONFIG_BOOTCOMMAND	\
	"if test ${do_update} = yes; then " \
		"disconfig 0; "\
		"mtdparts default; " \
		"disconfig 10; "\
		"echo update bootloader ...; " \
		"run bootloaderupdate; " \
		"disconfig 15; "\
		"echo update fdt ...; " \
		"run fdtupdate; " \
		"disconfig 25; "\
		"echo update kernel ...; " \
		"run kernelupdate; " \
		"disconfig 45; "\
		"echo update rootfs ...; " \
		"run rootfsupdate; " \
		"disconfig 80; "\
		"echo update bootanimation ...; " \
		"run bootanimationupdate; " \
		"disconfig 90; "\
		"setenv do_update no; " \
		"disconfig 95; "\
		"saveenv; " \
		"disconfig 100; "\
		"run nandboot; " \
	"else " \
		"run nandboot; " \
	"fi"

#else

#endif

#endif
