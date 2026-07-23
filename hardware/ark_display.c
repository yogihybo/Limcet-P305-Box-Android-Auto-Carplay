// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal /dev/ark_display misc device — implements just enough of the
 * stock 3.4.0 "ark_display_drv" ioctl interface to unblock userspace.
 *
 * Root-cause chain (see docs/ARK1680_TS_REVERSE_ENGINEERING.md and
 * docs/MSNCOREAPP_DECONSTRUCTION.md-adjacent investigation): MsnCoreApp's
 * first substantive action in onFirstInit() is
 * arkapi_get_screen_info() (libarkcmn.so), which opens /dev/ark_display
 * and does ioctl(fd, 0xc004a01d, &info) — a vendor-specific "get screen
 * info" command from the stock ark_display_drv.c misc device
 * (compatible = "ark_display", registered via misc_register(), NOT part
 * of our upstream-style ark1668_lcdfb framebuffer driver). Our 4.19
 * kernel tree never ported ark_display_drv.c, so /dev/ark_display never
 * existed, the ioctl always failed, and MsnCoreApp fell into a ~9KB
 * fallback/default-init branch in its own binary that stock firmware
 * (which does have this device) never normally exercises — a very
 * plausible source of the immediate segfault on `start_msn`. A separate
 * binary, MsnFirstInit, hits the same missing device and falls back to
 * a wrong panel-size default ("set display inch: QSize(154, 86)
 * 6.94433" in the boot log), independently corroborating this.
 *
 * The exact command encoding and reply layout were recovered from
 * disassembling the real handler in the stock kernel
 * (`Prado firmware dump/mtd5_kernel/extracted/vmlinux.elf`,
 * `ark_disp_ioctl` @ 0x802d9fd8, case @ 0x802da7d4):
 *
 *   cmd    = 0xc004a01d  =>  _IOWR(0xa0 [ARKDISP_IOCTL_BASE], 29, <20 bytes>)
 *   reply  = 5 x u32, read from the kernel's `screeninfo_param` global
 *            (the same 120-byte runtime struct already identified in
 *            docs/boot_experiment_log.md's GT911/LCD-timing work,
 *            populated at boot by screen_id_setup() from screens[g_screen_id])
 *
 * `arkapi_get_screen_info()`'s own validity check only inspects the
 * *first* word (must be 0-7 — the screen id, matching
 * docs/SCREEN.md's `screen=N` bootarg / arkdata `ScreenId=0` for this
 * unit) — that's the field that actually matters for unblocking
 * MsnCoreApp's success path. The other four words are best-effort
 * width/height/mmWidth/mmHeight for an 800x480, ~5.5" panel (matches
 * docs/SCREEN.md's `ScreenId=0` entry and the original bootlog-v6
 * review's "real Prado is ~5.5in, stock logged QSize(120,72)"
 * reference) — field order not confirmed byte-exact against stock,
 * revisit if MsnFirstInit's computed QSize still looks wrong.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/string.h>

/* type=0xa0 (ARKDISP_IOCTL_BASE, see ArkPro Reference/userspace/display.h),
 * nr=29 (one past that reference header's last documented command — this
 * exact command isn't in the reference header, recovered from the stock
 * binary instead, see file header).
 *
 * The vendor's userspace macro uses `unsigned long` (4 bytes) as the
 * _IOWR() type argument for *every* ARKDISP_* command regardless of the
 * command's real payload size (confirmed: every entry in
 * ArkPro Reference/userspace/display.h does this) -- the encoded "size"
 * field in the ioctl command number is decoupled from what the driver
 * actually copy_to_user()s at runtime (20 bytes here, see
 * ark_disp_ioctl's real handler in the stock vmlinux). Matching this
 * exactly matters: using our own (larger) struct as the macro argument
 * silently changes the encoded command number and the switch(cmd) in
 * userspace's arkapi_get_screen_info() will never match this driver --
 * confirmed by v9 boot-log testing, where the device registered fine
 * but ioctl() still failed because of exactly this mismatch. */
#define ARK_DISPLAY_IOC_MAGIC		0xa0
#define ARKDISP_GET_SCREEN_INFO		_IOWR(ARK_DISPLAY_IOC_MAGIC, 29, unsigned long)
#define ARKDISP_GET_VDE_CFG		_IOWR(ARK_DISPLAY_IOC_MAGIC, 1, unsigned long)
#define ARKDISP_SET_VDE_CFG		_IOW(ARK_DISPLAY_IOC_MAGIC, 2, unsigned long)

/* Two more commands confirmed as real, live callers in this rootfs (raw
 * ioctl-number search across every rootfs shared library and binary,
 * 2026-07-20) -- unlike every other still-unimplemented ARKDISP_* command
 * (SET/GET_LAYER_CFG, TVOUT/ITU656 controls, etc, all confirmed to have
 * ZERO callers anywhere in this rootfs and deliberately left unimplemented
 * as not worth building against nothing), these two are genuinely called:
 *   - 0xa01b: MsnCoreApp (2 call sites). Stock's ark_disp_ioctl (@
 *     0x802d9fd8) just does `*(u32 *)(priv+0x1c88) = 1; return 0;` --
 *     no register access, an argument-less command (_IO, not _IOW/_IOWR --
 *     encodes to exactly 0xa01b with zero size/dir bits, confirmed by
 *     decoding the raw command number). Purpose not identified (nothing
 *     in ark_disp_ioctl itself reads this flag back), so implemented as a
 *     genuinely stored flag rather than a bare no-op, in case something
 *     else in stock depends on it being remembered.
 *   - 0x4004a000: libMcuCenter.so (1 call site). Stock's handler is a
 *     literal unconditional `return 0;` -- doesn't even touch the
 *     argument despite the command encoding as a 4-byte _IOW. */
#define ARKDISP_SET_UNKNOWN_FLAG_1C88	_IO(ARK_DISPLAY_IOC_MAGIC, 0x1b)
#define ARKDISP_NOOP_ACK		_IOW(ARK_DISPLAY_IOC_MAGIC, 0, unsigned long)
#define ARK_DISPLAY_LAYER_NUM		5

struct ark_screen_info {
	__u32 screen_id;	/* must be 0-7 -- the only field arkapi_get_screen_info() validates */
	__u32 width_px;
	__u32 height_px;
	__u32 mm_width;
	__u32 mm_height;
};

struct ark_disp_vde_cfg_arg {
	__u32 layer_id;
	__u32 hue;
	__u32 saturation;
	__u32 brightness;
	__u32 contrast;
};

/* ScreenId=0, 800x480 RGB888, ~5.5" -- see docs/SCREEN.md */
static const struct ark_screen_info ark_display_screen0 = {
	.screen_id  = 0,
	.width_px   = 800,
	.height_px  = 480,
	.mm_width   = 120,
	.mm_height  = 72,
};

static struct ark_disp_vde_cfg_arg ark_display_layers[ARK_DISPLAY_LAYER_NUM] = {
	{ .layer_id = 0, .hue = 0, .saturation = 128, .brightness = 128, .contrast = 128 },
	{ .layer_id = 1, .hue = 0, .saturation = 128, .brightness = 128, .contrast = 128 },
	{ .layer_id = 2, .hue = 0, .saturation = 128, .brightness = 128, .contrast = 128 },
	{ .layer_id = 3, .hue = 0, .saturation = 128, .brightness = 128, .contrast = 128 },
	{ .layer_id = 4, .hue = 0, .saturation = 128, .brightness = 128, .contrast = 128 },
};

/* VDE (video-display-enhancement) register plumbing -- real hardware
 * writes, recovered via Ghidra decompile of the stock kernel's
 * ark_disp_set_vde_cfg (@ 0x802db400) and the per-layer
 * ark_disp_set_osd_* / ark_disp_set_video_* register setters it dispatches
 * to (2026-07-19). Previously this whole device only ever touched the
 * ark_display_layers[] software cache above -- MsnCoreApp's contrast/
 * brightness/saturation/hue controls had zero effect on the actual
 * display hardware.
 *
 * Register base: same 4KB LCDC block already owned by the
 * ark1668_lcdfb platform driver (lcdc@e0500000 in the DTS) --
 * confirmed via the stock kernel's own static iotable_init() mapping
 * table (entry 7: phys 0xe0500000, length 0x1000). This device has no
 * DT binding of its own (plain misc_register(), not a platform
 * driver), so it ioremap()s this range directly rather than going
 * through devm_ioremap_resource() -- same pattern already used by
 * drivers/misc/ark_tool.c for /proc/arktool, which independently maps
 * LCD/pinmux registers also owned by another driver's DT node.
 *
 * Layer-id -> register offset, and OSD-vs-video routing: confirmed by
 * decompiling ark_disp_set_vde_cfg's own dispatch (layer_id < 3 -> OSD
 * setter with osd sub-index == layer_id; layer_id >= 3 -> video setter
 * with video sub-index == layer_id - 3), then decompiling each
 * individual ark_disp_set_osd_hue()/ark_disp_set_video_hue() etc. to
 * find the actual MMIO address each one is hardcoded to (this kernel
 * predates ioremap()-based mapping -- stock uses compile-time static
 * virtual addresses, converted back to physical offsets from the LCDC
 * base for this table). One 32-bit register per layer, all 5 layers
 * confirmed sharing the identical bit layout:
 *   bits [31:24] = hue, [23:16] = saturation, [15:8] = brightness,
 *   [7:0] = contrast -- direct 0-255 pass-through per field, no
 *   scaling in stock, matching this driver's existing ioctl-level
 *   0-255 bounds check exactly. */
#define ARK_LCDC_PHYS_BASE	0xe0500000
#define ARK_LCDC_MAP_SIZE	0x200		/* covers every offset below */

static const u32 ark_vde_reg_offset[ARK_DISPLAY_LAYER_NUM] = {
	[0] = 0x144,	/* OSD layer 0 */
	[1] = 0x14c,	/* OSD layer 1 */
	[2] = 0x154,	/* OSD layer 2 */
	[3] = 0x1d8,	/* video layer 0 */
	[4] = 0x134,	/* video layer 1 */
};

static void __iomem *ark_lcdc_base;

static void ark_display_write_vde_reg(const struct ark_disp_vde_cfg_arg *cfg)
{
	u32 reg;

	if (!ark_lcdc_base)
		return;

	reg = (cfg->hue & 0xff) << 24 | (cfg->saturation & 0xff) << 16 |
	      (cfg->brightness & 0xff) << 8 | (cfg->contrast & 0xff);

	writel(reg, ark_lcdc_base + ark_vde_reg_offset[cfg->layer_id]);
}

/* Stock's ark_disp_ioctl only rejects a NULL arg for the specific commands
 * that actually dereference it (confirmed per-command in the decompile) --
 * it has no blanket check. ARKDISP_SET_UNKNOWN_FLAG_1C88 and
 * ARKDISP_NOOP_ACK both ignore arg entirely in stock, so they're handled
 * before the NULL check below rather than after it. */
static bool ark_display_unknown_flag_1c88;

static long ark_display_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case ARKDISP_SET_UNKNOWN_FLAG_1C88:
		ark_display_unknown_flag_1c88 = true;
		pr_info("ark_display: ARKDISP_SET_UNKNOWN_FLAG_1C88\n");
		return 0;
	case ARKDISP_NOOP_ACK:
		pr_info("ark_display: ARKDISP_NOOP_ACK\n");
		return 0;
	default:
		break;
	}

	if (!arg)
		return -EINVAL;

	switch (cmd) {
	case ARKDISP_GET_SCREEN_INFO:
		if (copy_to_user((void __user *)arg, &ark_display_screen0,
				  sizeof(ark_display_screen0)))
			return -EFAULT;
		pr_info("ark_display: ARKDISP_GET_SCREEN_INFO -> screen_id=%u %ux%u %ux%umm\n",
			ark_display_screen0.screen_id, ark_display_screen0.width_px,
			ark_display_screen0.height_px, ark_display_screen0.mm_width,
			ark_display_screen0.mm_height);
		return 0;
	case ARKDISP_GET_VDE_CFG:
		{
			struct ark_disp_vde_cfg_arg input_arg;

			if (copy_from_user(&input_arg, (void __user *)arg, sizeof(input_arg)))
				return -EFAULT;

			if (input_arg.layer_id >= ARK_DISPLAY_LAYER_NUM) {
				pr_err("ark_display: ARKDISP_GET_VDE_CFG invalid layer_id=%u\n",
				       input_arg.layer_id);
				return -EINVAL;
			}

			input_arg = ark_display_layers[input_arg.layer_id];

			if (copy_to_user((void __user *)arg, &input_arg, sizeof(input_arg)))
				return -EFAULT;

			pr_info("ark_display: ARKDISP_GET_VDE_CFG -> layer_id=%u hue=%u saturation=%u brightness=%u contrast=%u\n",
				input_arg.layer_id, input_arg.hue, input_arg.saturation,
				input_arg.brightness, input_arg.contrast);
			return 0;
		}
	case ARKDISP_SET_VDE_CFG:
		{
			struct ark_disp_vde_cfg_arg input_arg;

			if (copy_from_user(&input_arg, (void __user *)arg, sizeof(input_arg)))
				return -EFAULT;

			if (input_arg.layer_id >= ARK_DISPLAY_LAYER_NUM) {
				pr_err("ark_display: ARKDISP_SET_VDE_CFG invalid layer_id=%u\n",
				       input_arg.layer_id);
				return -EINVAL;
			}

			if (input_arg.hue > 255 || input_arg.saturation > 255 ||
			    input_arg.brightness > 255 || input_arg.contrast > 255) {
				pr_err("ark_display: ARKDISP_SET_VDE_CFG invalid values (hue=%u, sat=%u, bri=%u, con=%u)\n",
				       input_arg.hue, input_arg.saturation, input_arg.brightness,
				       input_arg.contrast);
				return -EINVAL;
			}

			/* Stock's own ark_disp_set_vde_cfg only touches the real
			 * register when the value actually changed vs its cache --
			 * matched here rather than writing unconditionally on
			 * every call. */
			if (memcmp(&ark_display_layers[input_arg.layer_id], &input_arg,
				   sizeof(input_arg)))
				ark_display_write_vde_reg(&input_arg);

			ark_display_layers[input_arg.layer_id] = input_arg;

			pr_info("ark_display: ARKDISP_SET_VDE_CFG -> layer_id=%u hue=%u saturation=%u brightness=%u contrast=%u\n",
				input_arg.layer_id, input_arg.hue, input_arg.saturation,
				input_arg.brightness, input_arg.contrast);
			return 0;
		}
	default:
		pr_info("ark_display: unhandled ioctl cmd=0x%08x\n", cmd);
		return -ENOTTY;
	}
}

static int ark_display_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int ark_display_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations ark_display_fops = {
	.owner          = THIS_MODULE,
	.open           = ark_display_open,
	.release        = ark_display_release,
	.unlocked_ioctl = ark_display_ioctl,
};

static struct miscdevice ark_display_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "ark_display",
	.fops  = &ark_display_fops,
};

static int __init ark_display_init(void)
{
	int ret = misc_register(&ark_display_miscdev);

	if (ret) {
		pr_err("ark_display: misc_register failed (%d)\n", ret);
		return ret;
	}

	/* Not fatal if this fails -- ARKDISP_SET_VDE_CFG's cache-only
	 * behavior (matching what this whole device already did before
	 * today) is still a strictly better fallback than refusing to
	 * register the device at all. */
	ark_lcdc_base = ioremap(ARK_LCDC_PHYS_BASE, ARK_LCDC_MAP_SIZE);
	if (!ark_lcdc_base)
		pr_err("ark_display: ioremap of LCDC @ 0x%x failed -- "
		       "ARKDISP_SET_VDE_CFG will only update the software cache, "
		       "not real hardware registers\n", ARK_LCDC_PHYS_BASE);

	pr_info("ark_display: registered /dev/ark_display\n");
	return 0;
}

static void __exit ark_display_exit(void)
{
	if (ark_lcdc_base)
		iounmap(ark_lcdc_base);
	misc_deregister(&ark_display_miscdev);
}

module_init(ark_display_init);
module_exit(ark_display_exit);

MODULE_AUTHOR("Reconstructed from stock ark_display_drv.c / ark_disp_ioctl disassembly");
MODULE_DESCRIPTION("Minimal /dev/ark_display misc device -- implements every ARKDISP_* command confirmed to have a real caller in this rootfs (GET_SCREEN_INFO, GET/SET_VDE_CFG with real hardware register writes, SET_UNKNOWN_FLAG_1C88, NOOP_ACK); deliberately omits commands with zero confirmed callers (SET/GET_LAYER_CFG, TVOUT/ITU656 controls)");
MODULE_LICENSE("GPL");
