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

static long ark_display_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
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

	if (ret)
		pr_err("ark_display: misc_register failed (%d)\n", ret);
	else
		pr_info("ark_display: registered /dev/ark_display\n");
	return ret;
}

static void __exit ark_display_exit(void)
{
	misc_deregister(&ark_display_miscdev);
}

module_init(ark_display_init);
module_exit(ark_display_exit);

MODULE_AUTHOR("Reconstructed from stock ark_display_drv.c / ark_disp_ioctl disassembly");
MODULE_DESCRIPTION("Minimal /dev/ark_display misc device (ARKDISP_GET_SCREEN_INFO only)");
MODULE_LICENSE("GPL");
