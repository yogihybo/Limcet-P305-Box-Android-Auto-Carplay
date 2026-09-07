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

/* Skip low-level CPU/DDR init — Stepldr initializes DDR before jumping to
 * UBOOT.BIN at 0x30000. Re-running cpu_init_crit here would corrupt the
 * already-configured memory controller and hang the system. */
#define CONFIG_SKIP_LOWLEVEL_INIT
#define CONFIG_SKIP_LOWLEVEL_INIT_ONLY

/* Kernel boot handoff via ATAGS — required by bootz/bootm when no DTB is
 * provided. These pass bootargs (cmdline), memory map, and initrd to the
 * kernel. Without these, U-Boot prints "FDT and ATAGS support not compiled
 * in - hanging" and halts. */
#define CONFIG_SETUP_MEMORY_TAGS	/* pass DRAM layout to kernel */
#define CONFIG_CMDLINE_TAG		/* pass bootargs to kernel     */
#define CONFIG_INITRD_TAG		/* pass initrd/initramfs info  */

/* L2 Cache(pl310) */
#ifndef CONFIG_SYS_L2CACHE_OFF
#define CONFIG_SYS_L2_PL310
#define CONFIG_SYS_PL310_BASE	0x70000000
#endif

/* Memory layout */
#define CONFIG_NR_DRAM_BANKS			1
#define CONFIG_SYS_SDRAM_BASE		0x00000000
#define CONFIG_SYS_SDRAM_SIZE		SZ_256M
#define CONFIG_SYS_MEM_TOP_HIDE		(76 * 1024 * 1024) /* Hide top 76MB (180M..256M) for OSD1/OSD2 carveouts */
/*
#define CONFIG_SYS_MALLOC_LEN		0x80000
*/

/* Init stack — placed above the U-Boot binary (loaded at 0x30000, ~370KB).
 * Previously was (SDRAM_BASE + 16K) = 0x3c00 which is INSIDE the binary! */
#define CONFIG_SYS_INIT_SP_ADDR		0x80000

/* SPL disabled — Stepldr already initializes DDR before jumping to UBOOT.BIN.
 * Keep these defines as guards in case any code still references them. */
#define CONFIG_SPL_TEXT_BASE		0xc0000000
#define CONFIG_SPL_MAX_SIZE		0x8000

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
	"nandfdt=" CONFIG_DEFAULT_FDT_FILE "\0" \
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
	"bootloaderupdate=if fatload ${update_dev_type} ${update_dev_part} ${loadaddr} u-boot.img; " \
		"then setenv bootloadersize ${filesize}; " \
		"nand erase.part bootloader; " \
		"nand write ${loadaddr} bootloader ${filesize}; fi\0" \
	"bootloaderupdate_back=if fatload ${update_dev_type} ${update_dev_part} ${loadaddr} u-boot.img; " \
		"then setenv bootloadersize ${filesize}; " \
		"nand erase.part bootloader_back; " \
		"nand write ${loadaddr} bootloader_back ${filesize}; fi\0" \
/*	"bootloaderupdate=Updatebootloader ${update_dev_type} ${update_dev_part}; \0" \
*/	"updatefromflash=UpdateFlash 0 0; \0" \
	"rootfsupdate=if fatload ${update_dev_type} ${update_dev_part} ${loadaddr} rootfs.ubi; " \
		"then setenv rootfssize ${filesize}; " \
		"nand erase.part rootfs; " \
		"nand write ${loadaddr} rootfs ${filesize}; fi\0" \
	"bootanimationupdate=if fatload ${update_dev_type} ${update_dev_part} ${loadaddr} bootanimation; " \
		"then setenv bootanimationsize ${filesize}; " \
		"nand erase.part bootanimation; " \
		"nand write ${loadaddr} bootanimation ${filesize}; " \
		"else setenv bootanimationsize 0; fi\0" \
	"screen=0\0" \
	/* rootdelay=3: blind test for a suspected mdev-vs-MsnCoreApp boot
	 * race on the stock kernel/rootfs path -- /dev/ark_display open()
	 * fails in userspace despite __disp_probe()'s own failure paths
	 * (all <3>/KERN_ERR, should be console-visible) not appearing in
	 * the boot log, suggesting probe succeeds but mdev -s (rcS line
	 * 10, creates the node from sysfs) may not have run yet by the
	 * time MsnCoreApp tries to open it -- plausible given this boot
	 * chain is now faster than before. rootdelay only pushes back the
	 * whole init timeline uniformly (no way to reach into the stock
	 * NAND rootfs's own rcS/inittab ordering from here), so this is a
	 * blind test, not a confirmed fix -- see
	 * docs/DEVICE_TEST_CHECKLIST_2026-07-18.md. Revert if it doesn't
	 * help. */ \
	"nandargs=setenv bootargs console=ttyS0,115200n8 mem=180M earlyprintk=serial ubi.mtd=6 root=ubi0:rootfs rootfstype=ubifs rootwait rootdelay=3 ro screen=0 ${mtdparts}\0" \
	/* switchecc 2: this chip's actual on-flash OOB layout for the
	 * kernel/rootfs/bootloader partitions is 1024-byte step / 13-byte /
	 * 7-bit BCH strength / eccpos starting at OOB offset 3 — matches
	 * neither of the driver's other two predefined ECC layouts (see
	 * nand_hw_eccoob_64_2seg13b in drivers/mtd/nand/ark_nand.c). Without
	 * it, `nand read ... kernel` fails with "err more than 8 bit" on
	 * every page. Confirmed twice live on real hardware — was briefly
	 * pulled back out on suspicion of causing a broader "all nand reads
	 * broken" regression, but reverting the whole driver to its
	 * pre-switchecc-2 baseline reproduced the exact same "err more than
	 * 8 bit" failure this fixes, showing that regression was unrelated
	 * (most likely bootstock's old NAND-read-at-0x30000 path, since
	 * removed — see ark1668_boot_cmds.c). */ \
	"nandboot=echo Booting stock kernel ...; " \
		"disconfig 0; " \
		/* disconfig 0 calls ark_display_init() (full LCDC re-init),
		 * which resets OSD1 layer state -- any bootlogofile call
		 * BEFORE this point gets silently wiped out before it's ever
		 * visible. Must come after. sleep gives it a moment on screen
		 * before the NAND reads/bootz below take over (2026-07-29,
		 * user reported not seeing the status change on bootnand). */ \
		"bootlogofile bootlogo_nand.raw; " \
		"sleep 1; " \
		"backcarcheck; " \
		"run nandargs; " \
		"switchecc 2; " \
		"setenv machid 1068; " \
		/* Stock's kernel (track_paint_init(), vmlinux.elf @ 0x802f0ebc)
		 * checks for a "RSTK" magic at fixed physical 0x0fd00000 (inside
		 * the LCDC's own 240-256MB carveout) before initializing the
		 * carback/reverse-camera track overlay -- confirmed via decompile
		 * plus byte-level verification of firmware_source/mtd10_reversingtrack/
		 * reversingtrack, which genuinely starts with "RSTK". Without this
		 * load, the check fails ("reservingtrack check failed!", <1>/ALERT,
		 * confirmed present in a failing boot and absent -- i.e. the magic
		 * check passing -- in a known-good baseline dmesg). Not established
		 * to be the cause of the separate /dev/ark_display open failure
		 * (this subsystem's own init failure path is self-contained and
		 * doesn't touch shared allocations), but it's a real, confirmed gap
		 * regardless -- see docs/DEVICE_TEST_CHECKLIST_2026-07-18.md. */ \
		"nand read 0xfd00000 reversingtrack; " \
		/* 2026-08-01: NAND is now tried FIRST, not the SD-staged
		 * zImage_stock copy -- bootnand exists specifically to work
		 * independent of the SD card (the last-resort path when USB/
		 * SD have already failed), so preferring an SD file here was
		 * backwards from its own purpose. The NAND "kernel" partition
		 * itself was never the problem; this SD-first order only ever
		 * existed because an earlier separate bootstockkernel command
		 * got merged straight into this script (commit 0e8528f48) and
		 * happened to keep its own SD-only logic first. switchecc 2
		 * just above already configures the correct ECC settings for
		 * NAND kernel/rootfs reads (docs/DEVICE_TEST_CHECKLIST_2026-07-18.md),
		 * so there's no known reason NAND itself would fail here. SD
		 * zImage_stock kept as a fallback in case a NAND read ever
		 * does fail for some other reason. */ \
		"if nand read ${kerneladdr} kernel; then " \
			"echo Loaded kernel from NAND; " \
		"else " \
			"echo NAND kernel read failed -- falling back to SD zImage_stock; " \
			"fatload mmc 0:1 ${kerneladdr} zImage_stock; " \
		"fi; " \
		"bootz ${kerneladdr}\0"

/* ATAGS buffer for the kernel boot params list (setup_start_tag() et al,
 * arch/arm/lib/bootm.c). Without this defined, initr_malloc_bootparams()
 * (common/board_r.c) never runs and gd->bd->bi_boot_params is never set
 * to a real heap address -- it stays at its zero-initialized value, so
 * ATAGS get written to (and the kernel is handed r2 pointing at) physical
 * address 0x0, colliding with the exception vector table there (SCTLR.V=0,
 * low vectors). Root-caused via a live register/ATAGS-pointer dump right
 * at the kernel jump instant -- see
 * docs/historical/HANDOFF_nand_ecc_uboot_vs_kernel.md §5. */
#define CONFIG_SYS_BOOTPARAMS_LEN	SZ_4K

/* Environment. CONFIG_ENV_IS_IN_MMC was tried 2026-07-31 (see
 * env/nowhere.c's lack of any save/load implementation -- the previous
 * CONFIG_ENV_IS_NOWHERE meant saveenv/env_save() never persisted
 * anything at all) but REVERTED same day: hardware-tested and found it
 * hangs the board completely before even "DRAM:"/"NAND:"/"MMC:" print.
 * Root cause not yet found -- see docs/DEVICE_TEST_CHECKLIST_2026-07-18.md
 * §83. CONFIG_ENV_OFFSET/CONFIG_SYS_MMC_ENV_DEV below are dead with
 * CONFIG_ENV_IS_NOWHERE active (kept, commented, for the next attempt --
 * see that comment for the reasoning behind the specific values: 0x40000
 * sits well inside the ~1MiB gap before partition 1, which starts at
 * 1MiB/0x100000 in build_bootable_sdcard.sh). Do NOT re-enable
 * CONFIG_ENV_IS_IN_MMC in the defconfig without understanding why this
 * hangs so early first. */
#define CONFIG_ENV_SIZE			0x40000	// 256K (2 erase blocks)
/* #define CONFIG_ENV_OFFSET		0x40000 */
/* #define CONFIG_SYS_MMC_ENV_DEV	0 */
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
#define CONFIG_CMD_UBI
#define CONFIG_CMD_UBIFS
*/

#define CONFIG_SYS_MALLOC_LEN     (CONFIG_ENV_SIZE+1024*1024)
#define CONFIG_SYS_GBL_DATA_SIZE   512

#define CONFIG_EXTRA_ENV_SETTINGS \
	"need_update=no\0" \
	"update_dev_type=mmc\0" \
	"update_dev_part=0\0" \
	"sd_dev_part="CONFIG_SD_DEV_PART"\0" \
	"loadaddr=0x1000000\0" \
	"kerneladdr=0x1000000\0" \
	"dtbaddr=0x2000000\0" \
	"bootanimationaddr=0xfc00000\0" \
	"bootanimationsize=0x400000\0" \
	"kernelsize=0x500000\0" \
	"stdout=serial,lcdconsole\0" \
	"stderr=serial,lcdconsole\0" \
	/* Tunable parts of bootmmc/bootusb (see ark1668_boot_cmds.c) —
	 * editable via setenv/uEnv.txt without recompiling. The C code
	 * supplies the sequencing/error-checking; these supply the values. */ \
	"kernelfile=zImage\0" \
	"dtbfile=ark1668_limcet_p305.dtb\0" \
	"mmcroot=/dev/mmcblk0p2\0" \
	"usbroot=/dev/sda2\0" \
	"hybridubootfile=uboot_hybrid.bin\0" \
	"stockubootfile=uboot_stock.bin\0" \
	"machid=1068\0" \
	/* REMOVED 2026-08-05: threadirqs, an AA audio-stutter investigation
	 * experiment added 2026-08-04 targeting non-threaded hard-IRQ
	 * contention on this single-core SoC. Confirmed absent from stock's
	 * real kernel at the time ("threadirqs"/"force_irqthreads" appear
	 * nowhere in stock's dumped vmlinux) -- a genuine new deviation, not
	 * a stock restoration. The real fix landed 2026-08-05 as a much more
	 * surgical, isolation-tested change: the cyclic-DMA tasklet now runs
	 * on HI softirq priority specifically (linux-arkmicro commit
	 * 886e8b56c), rather than threading every non-threaded IRQ handler
	 * system-wide. Reverted to stock's exact bootargs now that the
	 * targeted fix supersedes this broader hypothesis. */ \
	"bootargs_common=console=ttyS0,115200n8 mem=180M earlyprintk=serial rootfstype=ext4 rootwait rw screen=0 user_debug=8\0" \
	NANDARGS

/* Default (non-interrupted) autoboot, in priority order:
 *   1. bootusb     — kernel+DTB from a USB stick, rootfs on the SD card.
 *   2. boothybrid  — chainload the patched hybrid U-Boot (uboot_hybrid.bin)
 *      from the SD card FAT partition 1.
 *   3. bootstock   — chainload the original stock U-Boot (uboot_stock.bin)
 *      from the SD card FAT partition 1.
 *   4. run nandboot — last-resort direct NAND kernel boot using THIS build. */
/* Import uEnv.txt from the SD card first, if present — this can override
 * ANY env var (bootargs, kernelfile, mmcroot, bootcmd itself, etc.)
 * without recompiling. If bootcmd wasn't overridden by that import, this
 * falls through to the compiled-in default above.
 *
 * boothybrid/bootstock chainload the ORIGINAL, unmodifiable stock U-Boot
 * binary and hand control to it (see ark1668_boot_cmds.c). A bug inside
 * that binary itself -- not our own code -- has been seen (2026-07-29)
 * to intermittently HANG before the kernel loads, rather than failing
 * cleanly. Because they used to sit in this if/elif chain, a hang there
 * never returns, so the `nandboot` fallback below never ran either --
 * the unit would just be stuck until power-cycled. `nandboot` (this
 * build's own U-Boot booting the real stock kernel+rootfs directly from
 * NAND, no chainload, no black-box binary involved) was hardware-
 * confirmed 2026-07-24 to bring up the full stock kernel/MsnCoreApp/
 * CarPlay/BT/WiFi stack end to end -- so it's now the default instead,
 * removing this whole class of unfixable failure from the normal boot
 * path. `boothybrid`/`bootstock` remain available as manual commands at
 * the prompt for anyone who explicitly wants to test/compare them.
 *
 * 2026-07-31: gained a `bootcheck` gate (ark1668_boot_cmds.c) -- tracks
 * consecutive boots that never confirm success (userspace clears
 * `bootcount` via `fw_setenv bootcount 0` a short while after rcS
 * reaches a stable point, see firmware_overlay/etc/rc.d/rcS). After
 * `bootlimit` (default 3) consecutive unconfirmed attempts, skips
 * `bootusb` entirely and goes straight to `nandboot` (stock, the most
 * reliable known-good path) instead of retrying the same broken image
 * forever. The uEnv.txt import stays first/unconditional so a
 * user-overridden `bootlimit` there is already in the environment by
 * the time `bootcheck` reads it.
 *
 * 2026-07-31: `carbackcamcheck` (ark1668_display_cfg.c) -- an instant,
 * U-Boot-level reverse-camera preview (ITU656 hardware bypass straight
 * into the LCDC video layer, no Linux involvement) if the reverse-gear
 * GPIO is already asserted at power-on, matching stock's own real
 * boot-time behavior (ported from stock's disassembled binary; see
 * docs/DEVICE_TEST_CHECKLIST_2026-07-18.md) -- was wired in here as the
 * very first automatic step, but PULLED BACK OUT the same day: its
 * register sequence (and the MCU UART notify it also does) has never
 * actually been hardware-verified, even manually, and this project just
 * had one fully-locked-board scare from a different unverified change
 * in the same session. Still fully available as a manual command at the
 * prompt (`carbackcamcheck`, or `itu656` directly) -- re-add the call
 * below once confirmed working by hand. */
#define CONFIG_BOOTCOMMAND	\
	/* 2026-08-01: arm the watchdog as the very first thing autoboot
	 * does once it's confirmed proceeding unattended -- see wdtarm's
	 * own comment (ark1668_boot_cmds.c) and board_late_init()'s
	 * comment (ark1668.c) for why this replaced arming in board code
	 * directly (that fired even when the user stopped autoboot to
	 * investigate a hang manually, resetting them out of their own
	 * debugging session). Never runs at all if space is pressed to
	 * stop autoboot -- this whole string doesn't execute in that
	 * case, so nothing to disarm. */ \
	"wdtarm; " \
	/* 2026-08-01: see noctrlc's own comment (ark1668_boot_cmds.c) --
	 * a not-actively-read console RX line can pick up noise that
	 * occasionally decodes as Ctrl-C, spuriously aborting USB
	 * operations inside bootusb's own enumeration. Disabled only for
	 * the duration of this unattended automatic sequence; re-enabled
	 * at the very end as a safety net in case boot fails and falls
	 * through to the interactive prompt. Never touched at all if
	 * autoboot is stopped manually, same as wdtarm above. */ \
	"noctrlc; " \
	/* 2026-09-07: restored uEnv.txt automatic loading -- the earlier
	 * intermittent boot hang was traced to serial RX line electrical noise,
	 * mitigated by restricting autoboot interrupt to the spacebar and
	 * noctrlc. Allows overriding any env var and executing custom commands
	 * via uenvcmd without recompiling U-Boot. */ \
	"if fatload mmc 0:1 ${loadaddr} uEnv.txt; then " \
		"echo [uEnv] Loaded environment from uEnv.txt; " \
		"env import -t ${loadaddr} ${filesize}; " \
	"fi; " \
	"if test -n \"${uenvcmd}\"; then " \
		"echo [uEnv] Running uenvcmd ...; " \
		"run uenvcmd; " \
	"fi; " \
	"if bootcheck; then " \
		"if bootusb; then true; else run nandboot; fi; " \
	"else " \
		"echo [bootcheck] bootlimit exceeded -- going straight to nandboot; " \
		"run nandboot; " \
	"fi; " \
	"noctrlc 1"

#else

#endif

#endif
