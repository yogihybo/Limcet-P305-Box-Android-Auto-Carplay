/*
 * Copyright(c) 2013 Hong Kong Applied Science and Technology
 * Research Institute Company Limited (ASTRI)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 * 
 * Name:
 *      ark_display_drv.c
 *
 * Description:
 *
 * 
 * Author:
 *      Chan Man Chi
 *
 * Remarks:
 *
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/proc_fs.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include <linux/videodev2.h>
#include <media/videobuf-core.h>
#include <media/v4l2-common.h>
#include <linux/delay.h>
#include <linux/ark/ringpair.h>
#include <linux/ark/ark_display_io.h>
#include <mach/va_map.h>
#include <linux/miscdevice.h>
#include "ark_display.h"
#include "ark_display_priv.h"
#include "ark_lcd_regs.h"

/* This driver is written to work for kernel 2.6.16 or above */

MODULE_AUTHOR("Chan Man Chi");
MODULE_DESCRIPTION("ArkMicro Display Driver");
MODULE_LICENSE("GPL");

#define ARK_DISP_DEV_NAME    "dev"

extern int boot_complete;
struct platform_device *ark_disp_platform_device = NULL;

struct ark_disp_dev *disp_dev;

#define SYS_BASE  0xE4900000

extern unsigned int uboot_backcar_status;

extern struct screen_info screens [];
	
static void ark_disp_io_init(void)
{	
	/*backcar detect gpio init*/
	if(0 > gpio_request(BACKCAR_GPIO, "backcar"))
		printk("ERROR: gpio%d %s fail\n", BACKCAR_GPIO, __func__);
	gpio_direction_input(BACKCAR_GPIO);
	gpio_set_debounce(BACKCAR_GPIO, 20);
	
	if(0 > gpio_request(BACKLIGHT_GPIO, "blacklight"))
		printk("ERROR: gpio%d %s fail\n", GPIO81, __func__);
	gpio_direction_output(BACKLIGHT_GPIO, 1);
}

#ifdef ARK_DISP_USE_PROC_DIR_ENTRY
/* ===========================================================================
 * Description:
 *		Procfs read and write function
 *
 * Arguments:
 *
 * Return value:
 *
 * Remarks:
 *
 * ===========================================================================
 */

static int ark_disp_read_proc(
		char *buf, char **start, off_t offset, int count, int *eof, void *data)
{
	struct ark_disp_context *context = (struct ark_disp_context *)data;
	int len = 0;

	//    ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	len += sprintf(buf+len,
			"\nUsage: [Operation code] [arg 1] ...\n"
			"\tRead Register  - 0  offset[hex]\n"
			"\tWrite Register - 1  offset[hex] data[hex]\n"
			"\tGet VDE Config - 2  layer_id[hex]\n"
			"\tSet VDE Config - 3  layer_id[hex] hue[hex] sat[hex] bri[hex]\n"
			"\t                      con[hex]\n"
			"\tRead System Register  - 4  offset[hex]\n"
			"\tWrite System Register - 5  offset[hex] data[hex]\n"
			"\tView buffer info - 6 layer_id[dec]\n"
			"\tView layer info  - 7 layer_id[dec]\n"
			"\tSet layer geometry - 8 layer_id[dec] enable[0/1]\n"
			"\t                         src_xres[dec] src_yres[dec]\n"
			"\t                         xres[dec] yres[dec] xpos[dec] ypos[dec]\n"
			"\t                         crop_xres[dec] crop_yres[dec]\n"
			"\t                         crop_xpos[dec] crop_ypos[dec]\n"
			"\tSet colorkey - 9 layer_id[dec] colorkey_en[0/1]\n"
			"\t                   key_y[hex] key_cb[hex] key_cr[hex]\n"
			"\t                   y_thld[hex] cb_thld[hex] cr_thld[hex]\n"
			"\tSet blending control - 10 layer_id[dec] alpha_blend_en[0/1]\n"
			"\t                            per_pix_alpha_blend_en[0/1]\n"
			"\t                            blend_mode[hex] alpha[hex]\n"
			"\t         blend_mode: 0=whole_blend, 1=whole_overwrt\n"
			"\t                     2=keycol_trans_abs_other_overwrt\n"
			"\t                     3=keycol_trans_abs_other_trans\n"
			"\t                     4=keycol_overwrt_other_trans\n"
			"\t                     5=keycol_overwrt_other_trans_abs\n"
			"\t                     6=keycol_trans_other_overwrt\n"
			"\t                     7=keycol_trans_other_trans_abs\n"
			"\t                     8=whole_blend_alpha\n"
			"\t                     9=whole_overwrt_alpha\n"
			"\t                     10=keycol_trans_other_overwrt_alpha\n"
			"\t                     11=keycol_trans_other_blend\n"
			"\t                     12=keycol_overwrt_alpha_other_blend\n"
			"\t                     13=keycol_overwrt_alpha_other_trans\n"
			"\t                     14=keycol_blend_other_overwrt_alpha\n"
			"\t                     15=keycol_blend_other_trans\n"
			"\tSet lcd layer priority - 11 pri_osd0[dec] pri_osd1[dec]\n"
			"\t                              pri_osd2[dec] pri_vid[dec]\n"
			"\t                              pri_vid2[dec]\n"
			"\tSet tvenc layer priority - 12 pri_osd0[dec] pri_osd1[dec]\n"
			"\t                                pri_osd2[dec] pri_vid[dec]\n"
			"\t                                pri_vid2[dec]\n"
			"\tSet layer format - 13 layer_id[dec] interlaced[0/1] pixfmt[dec]\n"
			"\t                        rgb_order[dec] yuyv_order[dec]\n"
			"\t         pixfmt: 0=palette, 1=bmp24, 2=yuv422, 3=yuv420, 4=yuyv,\n"
			"\t                 5=yuv, 6=rgbi555, 7=rgb565, 8=rgba888, 9=rgb888,\n"
			"\t                 10=rgba1555, 11=rgba1888, 12=rgba4888,\n"
			"\t                 13=rgb666, 14=rgba1666, 15=y_uv422, 16=y_uv420\n"
			"\t         rgb_order: 0=rgb, 1=rbg, 2=grb, 3=gbr, 4=brg, 5=bgr\n"
			"\t         yuyv_order: 0=yuyv, 1=yvyu, 2=uyvy, 3=vyuy\n"
			"\tView v4l2 driver info - 14 layer_id[dec]\n"
			"\tView TVENC config     - 15\n"
			"\tSet TVENC config      - 16 enable[0/1] out_mode[dec]\n"
			"\t                             background_color_y[hex]\n"
			"\t                             background_color_cb[hex]\n"
			"\t                             background_color_cr[hex]\n"
			"\t         out_mode: 0=ARKDISP_TVENC_OUT_YPBPR_I480HZ60\n"
			"\t         out_mode: 1=ARKDISP_TVENC_OUT_YPBPR_I576HZ50\n"
			"\t         out_mode: 2=ARKDISP_TVENC_OUT_YPBPR_P480HZ60\n"
			"\t         out_mode: 3=ARKDISP_TVENC_OUT_YPBPR_P576HZ50\n"
			"\t         out_mode: 4=ARKDISP_TVENC_OUT_YPBPR_P720HZ60\n"
			"\t         out_mode: 5=ARKDISP_TVENC_OUT_YPBPR_P720HZ50\n"
			"\t         out_mode: 6=ARKDISP_TVENC_OUT_YPBPR_I1080HZ60\n"
			"\t         out_mode: 7=ARKDISP_TVENC_OUT_YPBPR_I1080HZ50\n"
			"\t         out_mode: 8=ARKDISP_TVENC_OUT_YPBPR_I1080HZ50_1250\n"
			"\t         out_mode: 9=ARKDISP_TVENC_OUT_YPBPR_P1080HZ60\n"
			"\t         out_mode: 10=ARKDISP_TVENC_OUT_YPBPR_P1080HZ50\n"
			"\t         out_mode: 11=ARKDISP_TVENC_OUT_CVBS_PAL\n"
			"\t         out_mode: 12=ARKDISP_TVENC_OUT_CVBS_NTSC\n"
			"\t         out_mode: 13=ARKDISP_TVENC_OUT_ITU656_PAL\n"
			"\t         out_mode: 14=ARKDISP_TVENC_OUT_ITU656_NTSC\n"
			"\t         out_mode: 15=ARKDISP_TVENC_OUT_VGA_640x480HZ60\n"
			"\t         out_mode: 16=ARKDISP_TVENC_OUT_VGA_800x600HZ60\n"
			"\t         out_mode: 17=ARKDISP_TVENC_OUT_VGA_1024x768HZ60\n"
			"\t         out_mode: 18=ARKDISP_TVENC_OUT_VGA_1280x1024HZ60\n"
			"\t         out_mode: 19=ARKDISP_TVENC_OUT_VGA_1900x1200HZ60\n"
			"\t         out_mode: 20=ARKDISP_TVENC_OUT_VGA_1280x1024HZ75\n"
			"\t         out_mode: 21=ARKDISP_TVENC_OUT_VGA_1280x960HZ85\n"
			"\t         out_mode: 22=ARKDISP_TVENC_OUT_VGA_1280x720HZ60\n"
			"\tView LCD config     - 17\n"
			"\tSet LCD config      - 18 enable[0/1] panel_type[dec]\n"
			"\t                             background_color_y[hex]\n"
			"\t                             background_color_cb[hex]\n"
			"\t                             background_color_cr[hex]\n"
			"\t         panel_type: %d=ARKDISP_LCD_PANEL_PARALLEL_24BIT\n"
			"\t         panel_type: %d=ARKDISP_LCD_PANEL_PARALLEL_18BIT\n"
			"\t         panel_type: %d=ARKDISP_LCD_PANEL_PARALLEL_16BIT\n"
			"\t         panel_type: %d=ARKDISP_LCD_PANEL_SRGB\n"
			"\t         panel_type: %d=ARKDISP_LCD_PANEL_CPU_SCREEN\n"
			"\tConnect to ring pair ARK_RP_DI_WILD - 19\n"
			"\tDisconnect from input ring pair     - 20\n"
			"\tSet ITU656 direct data in enable    - 21 enable[0/1]\n"
			"\n",
		ARKDISP_LCD_PANEL_PARALLEL_24BIT,
		ARKDISP_LCD_PANEL_PARALLEL_18BIT,
		ARKDISP_LCD_PANEL_PARALLEL_16BIT,
		ARKDISP_LCD_PANEL_SRGB,
		ARKDISP_LCD_PANEL_CPU_SCREEN
			);

	len += sprintf(buf+len,
			"\nStatus and Statistics:\n"
			"\t deinterlace_en=%d prescale_en=%d itu656in_en=%d\n"
			"\t palette_color_num=%d\n"
			"\t free_list_entry_count=%d\n"
			"\t free_vbuf_list_entry_count=%d\n"
			"\t tasklet_run_count=%d\n"
			"\n",
			context->deinterlace_en,
			context->prescale_en,
			context->itu656in_en,
			context->palette_color_num,
			context->free_list_entry_count,
			context->free_vbuf_list_entry_count,
			context->tasklet_run_count
		      );

	return len;
}

char * pixfmt_string[] = {
	"palette",
	"bmp24bit",
	"yuv422",
	"yuv420",
	"yuyv",
	"yuv",
	"rgbi555",
	"rgb565",
	"rgba888",
	"rgb888",
	"rgba1555",
	"rgba1888",
	"rgba4888",
	"rgba666",
	"rgba1666",
	"y_uv422",
	"y_uv420",
};

char * blend_mode_string[] = {
	"whole_blend",
	"whole_overwrt",
	"keycol_trans_abs_other_overwrt",
	"keycol_trans_abs_other_trans",
	"keycol_overwrt_other_trans",
	"keycol_overwrt_other_trans_abs",
	"keycol_trans_other_overwrt",
	"keycol_trans_other_trans_abs",
	"whole_blend_alpha",
	"whole_overwrt_alpha",
	"keycol_trans_other_overwrt_alpha",
	"keycol_trans_other_blend",
	"keycol_overwrt_alpha_other_blend",
	"keycol_overwrt_alpha_other_trans",
	"keycol_blend_other_overwrt_alpha",
	"keycol_blend_other_trans",
};

static int ark_disp_write_proc(
		struct file *file, const char *buffer, unsigned long count, void *data)
{
	struct ark_disp_context *context = (struct ark_disp_context *)data;
	ssize_t result;
	int op_code;

	//    ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	if (count == 0) {
		ARKDISP_DBGPRTK("%s %d: count=0\n", __FUNCTION__, __LINE__);
		return 0;
	}

	/* Format: 	[Operation code] [arg 1] ...
	 *
	 * Read Register 	- 	0  offset[hex]
	 * Write Register 	-	1  offset[hex]  data[hex]
	 */

	result = sscanf(buffer, "%d", &op_code);

	switch (op_code) {
		case 0:	/* read register */
			{
				unsigned int offset, val32;

				sscanf(buffer, "%*d %x", &offset);

				if (offset % 4) {
					printk(KERN_ALERT
							"invalid reg offset 0x%02x (should be multiples of 4)\n",
							offset);
					break;
				}
				if (offset > ARK_DISP_REG_SIZE) {
					printk(KERN_ALERT
							"invalid reg offset 0x%02x (exceed range 0x%08x)\n",
							offset, ARK_DISP_REG_SIZE);
					break;
				}

#ifdef ARK_DISP_USE_HW_EMULATION
				val32 = *((unsigned int *)(ARK1680_VA_LCD + offset));
#else
				val32 = *((volatile unsigned int *)(ARK1680_VA_LCD + offset));
#endif
				printk(KERN_ALERT
						"succeed to read "
						"reg = 0x%02x, val = 0x%08x\n",
						offset,
						val32
				      );
			}
			break;
		case 1:	/* write register */
			{
				unsigned int offset, val32;

				sscanf(buffer, "%*d %x %x", &offset, &val32);

				if (offset % 4) {
					printk(KERN_ALERT
							"invalid reg offset 0x%02x (should be multiples of 4)\n",
							offset);
					break;
				}
				if (offset > ARK_DISP_REG_SIZE) {
					printk(KERN_ALERT
							"invalid reg offset 0x%02x (exceed range 0x%08x)\n",
							offset, ARK_DISP_REG_SIZE);
					break;
				}

#ifdef ARK_DISP_USE_HW_EMULATION
				*((unsigned int *)(ARK1680_VA_LCD + offset)) = val32;
#else
				*((volatile unsigned int *)(ARK1680_VA_LCD + offset)) = val32;
#endif
				printk(KERN_ALERT
						"succeed to write "
						"reg = 0x%02x, val = 0x%08x\n",
						offset, val32
				      );
			}
			break;
		case 2: /* get vde config */
			{
				unsigned int layer_id;

				sscanf(buffer, "%*d %x", &layer_id);

				if (layer_id >= ARK_DISP_LAYER_NUM) {
					printk(KERN_ALERT
							"invalid layer_id=%d (should be less than %d)\n",
							layer_id, ARK_DISP_LAYER_NUM);
					break;
				}

				printk(KERN_ALERT
						"layer%d.hue = 0x%02x\n"
						"layer%d.saturation = 0x%02x\n"
						"layer%d.brightness = 0x%02x\n"
						"layer%d.contrast = 0x%02x\n",
						layer_id, context->disp_obj[layer_id].layer_info.hue,
						layer_id, context->disp_obj[layer_id].layer_info.saturation,
						layer_id, context->disp_obj[layer_id].layer_info.brightness,
						layer_id, context->disp_obj[layer_id].layer_info.contrast
				      );
			}
			break;
		case 3: /* set vde config */
			{
				unsigned int layer_id, hue, saturation, brightness, contrast;
				struct ark_disp_vde_cfg_arg arg;
				int error;

				sscanf(buffer, "%*d %x %x %x %x %x",
						&layer_id, &hue, &saturation, &brightness, &contrast);

				if (layer_id >= ARK_DISP_LAYER_NUM) {
					printk(KERN_ALERT
							"invalid layer_id=%d (should be less than %d)\n",
							layer_id, ARK_DISP_LAYER_NUM);
					break;
				}
				if (hue > 0xFF) {
					printk(KERN_ALERT
							"invalid hue=%d (should be less than 0xFF)\n", hue);
					break;
				}
				if (saturation > 0xFF) {
					printk(KERN_ALERT
							"invalid saturation=%d (should be less than 0xFF)\n",
							saturation);
					break;
				}
				if (brightness > 0xFF) {
					printk(KERN_ALERT
							"invalid brightness=%d (should be less than 0xFF)\n",
							brightness);
					break;
				}
				if (contrast > 0xFF) {
					printk(KERN_ALERT
							"invalid contrast=%d (should be less than 0xFF)\n",
							contrast);
					break;
				}

				arg.layer_id   = layer_id;
				arg.hue        = hue;
				arg.saturation = saturation;
				arg.brightness = brightness;
				arg.contrast   = contrast;

				spin_lock(&context->disp_obj[layer_id].lock);
				error = ark_disp_set_vde_cfg(&arg);
				spin_unlock(&context->disp_obj[layer_id].lock);

				if (error == 0)
					printk(KERN_ALERT "vde parameters set done\n");
				else
					printk(KERN_ALERT "vde parameters set failed!\n");
			}
			break;
		case 4:	/* read system register */
			{
				unsigned int offset, val32;

				sscanf(buffer, "%*d %x", &offset);

				if (offset % 4) {
					printk(KERN_ALERT
							"invalid reg offset 0x%02x (should be multiples of 4)\n",
							offset);
					break;
				}
				if (offset > ARK_SYS_REG_SIZE) {
					printk(KERN_ALERT
							"invalid reg offset 0x%02x (exceed range 0x%08x)\n",
							offset, ARK_SYS_REG_SIZE);
					break;
				}

				val32 =
					*((volatile unsigned int *)(ARK1680_VA_SYS + offset));

				printk(KERN_ALERT
						"succeed to read "
						"reg = 0x%02x, val = 0x%02x\n",
						offset,
						val32
				      );
			}
			break;
		case 5:	/* write system register */
			{
				unsigned int offset, val32;

				sscanf(buffer, "%*d %x %x", &offset, &val32);

				if (offset % 4) {
					printk(KERN_ALERT
							"invalid reg offset 0x%02x (should be multiples of 4)\n",
							offset);
					break;
				}
				if (offset > ARK_SYS_REG_SIZE) {
					printk(KERN_ALERT
							"invalid reg offset 0x%02x (exceed range 0x%08x)\n",
							offset, ARK_SYS_REG_SIZE);
					break;
				}

				*((volatile unsigned int *)(ARK1680_VA_SYS + offset))
					= val32;

				printk(KERN_ALERT
						"succeed to write "
						"reg = 0x%02x, val = 0x%02x\n",
						offset, val32
				      );
			}
			break;
		case 6: /* view buffer info */
			{
				unsigned int layer_id, i;

				sscanf(buffer, "%*d %x", &layer_id);

				if (layer_id >= ARK_DISP_LAYER_NUM) {
					printk(KERN_ALERT
							"invalid layer_id=%d (should be less than %d)\n",
							layer_id, ARK_DISP_LAYER_NUM);
					break;
				}

				printk(KERN_ALERT
						"buffer info of layer%d:\n"
						"\tmin_buf_num = %d\n"
						"\tbuf_num = %d\n"
						"\tmin_buf_size = 0x%x (%d)\n"
						"\tbuf_size = 0x%x (%d)\n"
						"\tbuf_pool_size = 0x%x (%d)\n"
						"\tbuf_pool_base_virt = 0x%08x\n"
						"\tbuf_pool_base_phys = 0x%08x\n"
						"\tdisp_list_entry_count = %d\n"
						"\twait_list_entry_count = %d\n"
						"\tdone_list_entry_count = %d\n"
						"\tcur_buf_addr_phys = 0x%08x\n"
						"\tcbcr_offset = 0x%08x (%d)\n",
						layer_id,
						context->disp_obj[layer_id].buf_info.min_buf_num,
						context->disp_obj[layer_id].buf_info.buf_num,
						context->disp_obj[layer_id].buf_info.min_buf_size,
						context->disp_obj[layer_id].buf_info.min_buf_size, //dec
						context->disp_obj[layer_id].buf_info.buf_size,
						context->disp_obj[layer_id].buf_info.buf_size, //dec
						context->disp_obj[layer_id].buf_info.buf_pool_size,
						context->disp_obj[layer_id].buf_info.buf_pool_size, //dec
						context->disp_obj[layer_id].buf_info.buf_pool_base_virt,
						context->disp_obj[layer_id].buf_info.buf_pool_base_phys,
						context->disp_obj[layer_id].buf_info.disp_list_entry_count,
						context->disp_obj[layer_id].buf_info.wait_list_entry_count,
						context->disp_obj[layer_id].buf_info.done_list_entry_count,
						context->disp_obj[layer_id].buf_info.cur_buf_addr_phys,
						context->disp_obj[layer_id].buf_info.cbcr_offset,
						context->disp_obj[layer_id].buf_info.cbcr_offset //dec
							);

				for (i = 0; i < 2; i++) {
					printk(KERN_ALERT
							"\t prescal_buf_addr_virt[%d] = 0x%08x\n"
							"\t prescal_buf_addr_phys[%d] = 0x%08x\n",
							i,
							context->disp_obj[layer_id].buf_info.prescal_buf_addr_virt[i],
							i,
							context->disp_obj[layer_id].buf_info.prescal_buf_addr_phys[i]
					      );
				}
				printk(KERN_ALERT "\tprescal_disp_buf_ind = %d\n",
						context->disp_obj[layer_id].buf_info.prescal_disp_buf_ind
				      );
			}
			break;
		case 7: /* view layer info */
			{
				unsigned int layer_id;

				sscanf(buffer, "%*d %x", &layer_id);

				if (layer_id >= ARK_DISP_LAYER_NUM) {
					printk(KERN_ALERT
							"invalid layer_id=%d (should be less than %d)\n",
							layer_id, ARK_DISP_LAYER_NUM);
					break;
				}

				printk(KERN_ALERT
						"layer info of layer%d:\n"
						"\tenable = %d\n"
						"\tinterlaced = %d\n"
						"\talpha_blend_en = %d\n"
						"\tper_pix_alpha_blend_en = %d\n"
						"\ttype = %s\n"
						"\tscreen = %s\n"
						"\tpixfmt = %s\n"
						"\tsrc_xres = %d\n"
						"\tsrc_yres = %d\n"
						"\txres = %d\n"
						"\tyres = %d\n"
						"\txpos = %d\n"
						"\typos = %d\n"
						"\tcrop_xres = %d\n"
						"\tcrop_yres = %d\n"
						"\tcrop_xpos = %d\n"
						"\tcrop_ypos = %d\n"
						"\tline_length = %d\n"
						"\tblend_mode = %s\n"
						"\talpha = %d\n"
						"\tcolorkey_enable = %d\n"
						"\tcolorkey_y  = 0x%02x\n"
						"\tcolorkey_cb = 0x%02x\n"
						"\tcolorkey_cr = 0x%02x\n"
						"\tcolorkey_y_thld  = 0x%02x\n"
						"\tcolorkey_cb_thld = 0x%02x\n"
						"\tcolorkey_cr_thld = 0x%02x\n"
						"\tpriority = %d\n"
						"\tleft_cut_num   = %d\n"
						"\tright_cut_num  = %d\n"
						"\ttop_cut_num    = %d\n"
						"\tbottom_cut_num = %d\n"
						"\trgb_order  = %s\n"
						"\tyuyv_order = %s\n"
						"\tbits_per_pixel = %d\n"
						"\tdisp_frame_cnt = %d\n"
						"\n",
					layer_id,
					context->disp_obj[layer_id].layer_info.enable,
					context->disp_obj[layer_id].layer_info.interlaced,
					context->disp_obj[layer_id].layer_info.alpha_blend_en,
					context->disp_obj[layer_id].layer_info.per_pix_alpha_blend_en,
					(context->disp_obj[layer_id].layer_info.type
					 == ARKDISP_LAYER_OSD)? "osd" : "video",
					(context->disp_obj[layer_id].layer_info.screen
					 == ARKDISP_SCREEN_LCD)? "lcd" : "tvenc_out",
					pixfmt_string[context->disp_obj[layer_id].layer_info.pixfmt],
					context->disp_obj[layer_id].layer_info.src_xres,
					context->disp_obj[layer_id].layer_info.src_yres,
					context->disp_obj[layer_id].layer_info.xres,
					context->disp_obj[layer_id].layer_info.yres,
					context->disp_obj[layer_id].layer_info.xpos,
					context->disp_obj[layer_id].layer_info.ypos,
					context->disp_obj[layer_id].layer_info.crop_xres,
					context->disp_obj[layer_id].layer_info.crop_yres,
					context->disp_obj[layer_id].layer_info.crop_xpos,
					context->disp_obj[layer_id].layer_info.crop_ypos,
					context->disp_obj[layer_id].layer_info.line_length,
					blend_mode_string[
						context->disp_obj[layer_id].layer_info.blend_mode],
						context->disp_obj[layer_id].layer_info.alpha,
						context->disp_obj[layer_id].layer_info.colorkey_enable,
						context->disp_obj[layer_id].layer_info.colorkey_y,
						context->disp_obj[layer_id].layer_info.colorkey_cb,
						context->disp_obj[layer_id].layer_info.colorkey_cr,
						context->disp_obj[layer_id].layer_info.colorkey_y_thld,
						context->disp_obj[layer_id].layer_info.colorkey_cb_thld,
						context->disp_obj[layer_id].layer_info.colorkey_cr_thld,
						context->disp_obj[layer_id].layer_info.priority,
						context->disp_obj[layer_id].layer_info.left_cut_num,
						context->disp_obj[layer_id].layer_info.right_cut_num,
						context->disp_obj[layer_id].layer_info.top_cut_num,
						context->disp_obj[layer_id].layer_info.bottom_cut_num,
						(context->disp_obj[layer_id].layer_info.rgb_order
						 == ARKDISP_RGB_ORDER_RGB)? "rgb" :
							(context->disp_obj[layer_id].layer_info.rgb_order
							 == ARKDISP_RGB_ORDER_RBG)? "rbg" :
							(context->disp_obj[layer_id].layer_info.rgb_order
							 == ARKDISP_RGB_ORDER_GRB)? "grb" :
							(context->disp_obj[layer_id].layer_info.rgb_order
							 == ARKDISP_RGB_ORDER_GBR)? "gbr" :
							(context->disp_obj[layer_id].layer_info.rgb_order
							 == ARKDISP_RGB_ORDER_BRG)? "brg" : "bgr",
						(context->disp_obj[layer_id].layer_info.yuyv_order
						 == ARKDISP_YUYV_ORDER_YUYV)? "yuyv" :
							(context->disp_obj[layer_id].layer_info.yuyv_order
							 == ARKDISP_YUYV_ORDER_YVYU)? "yvyu" :
							(context->disp_obj[layer_id].layer_info.yuyv_order
							 == ARKDISP_YUYV_ORDER_UYVY)? "uyvy" :
							"vyuy",
						context->disp_obj[layer_id].layer_info.bits_per_pixel,
						context->disp_obj[layer_id].disp_frame_cnt
							);
			}
			break;
		case 8: /* set layer enable, resolution, size, position */
			{
				unsigned int layer_id, src_xres, src_yres, xres, yres, xpos, ypos;
				unsigned int enable, crop_xres, crop_yres, crop_xpos, crop_ypos;
				struct ark_disp_layer_info *layer;
				struct ark_disp_layer_cfg_arg arg;
				int error;

				sscanf(buffer, "%*d %d %d %d %d  %d %d  %d %d  %d %d  %d %d",
						&layer_id, &enable,
						&src_xres, &src_yres, &xres, &yres, &xpos, &ypos,
						&crop_xres, &crop_yres, &crop_xpos, &crop_ypos);

				if (layer_id >= ARK_DISP_LAYER_NUM) {
					printk(KERN_ALERT
							"invalid layer_id=%d (should be less than %d)\n",
							layer_id, ARK_DISP_LAYER_NUM);
					break;
				}

				layer = &context->disp_obj[layer_id].layer_info;

				ark_disp_copy_layer_info_to_cfg_arg(layer_id, &arg);

				arg.enable = enable;
				arg.src_xres = src_xres;
				arg.src_yres = src_yres;
				arg.xres = xres;
				arg.yres = yres;
				arg.xpos = xpos;
				arg.ypos = ypos;
				arg.crop_xres = crop_xres;
				arg.crop_yres = crop_yres;
				arg.crop_xpos = crop_xpos;
				arg.crop_ypos = crop_ypos;

				printk(KERN_ALERT "enable=%d\n", enable);
				printk(KERN_ALERT
						"src_xres=%d src_yres=%d xres=%d yres=%d xpos=%d ypos=%d\n",
						src_xres, src_yres, xres, yres, xpos, ypos);
				printk(KERN_ALERT
						"crop_xres=%d crop_yres=%d crop_xpos=%d crop_ypos=%d\n",
						crop_xres, crop_yres, crop_xpos, crop_ypos);

				spin_lock(&context->disp_obj[layer_id].lock);
				error = ark_disp_try_layer_cfg(&arg);
				spin_unlock(&context->disp_obj[layer_id].lock);

				if (error) {
					printk(KERN_ALERT "layer format try failed\n");
					break;
				}
				printk(KERN_ALERT "layer format try done\n");

				spin_lock(&context->disp_obj[layer_id].lock);
				error = ark_disp_set_layer_cfg(&arg);
				spin_unlock(&context->disp_obj[layer_id].lock);

				if (error == 0)
					printk(KERN_ALERT "layer geometry parameters set done\n");
				else
					printk(KERN_ALERT "layer geometry parameters set failed!\n");
			}
			break;
		case 9: /* set colorkey enable, colorkey y/cb/cr, colorkey y/cb/cr thld  */
			{
				unsigned int layer_id, colorkey_enable, colorkey_y, colorkey_cb,
					     colorkey_cr, colorkey_y_thld, colorkey_cb_thld,
					     colorkey_cr_thld;
				struct ark_disp_layer_info *layer;
				struct ark_disp_layer_cfg_arg arg;
				int error;

				sscanf(buffer, "%*d %d  %x  %x %x %x  %x %x %x",
						&layer_id,
						&colorkey_enable,
						&colorkey_y, &colorkey_cb, &colorkey_cr,
						&colorkey_y_thld, &colorkey_cb_thld, &colorkey_cr_thld);

				if (layer_id >= ARK_DISP_LAYER_NUM) {
					printk(KERN_ALERT
							"invalid layer_id=%d (should be less than %d)\n",
							layer_id, ARK_DISP_LAYER_NUM);
					break;
				}

				layer = &context->disp_obj[layer_id].layer_info;

				ark_disp_copy_layer_info_to_cfg_arg(layer_id, &arg);

				arg.colorkey_enable = colorkey_enable;
				arg.colorkey_y  = colorkey_y;
				arg.colorkey_cb = colorkey_cb;
				arg.colorkey_cr = colorkey_cr;
				arg.colorkey_y_thld  = colorkey_y_thld;
				arg.colorkey_cb_thld = colorkey_cb_thld;
				arg.colorkey_cr_thld = colorkey_cr_thld;

				printk(KERN_ALERT
						"colorkey_enable=%d\n"
						"colorkey_y=%02x colorkey_cb=%02x colorkey_cr=%02x\n"
						"colorkey_y_thld=%02x colorkey_cb_thld=%02x "
						"colorkey_cr_thld=%02x\n",
						colorkey_enable,
						colorkey_y, colorkey_cb, colorkey_cr,
						colorkey_y_thld, colorkey_cb_thld, colorkey_cr_thld);

				spin_lock(&context->disp_obj[layer_id].lock);
				error = ark_disp_try_layer_cfg(&arg);
				spin_unlock(&context->disp_obj[layer_id].lock);

				if (error) {
					printk(KERN_ALERT "layer format try failed\n");
					break;
				}
				printk(KERN_ALERT "layer format try done\n");


				spin_lock(&context->disp_obj[layer_id].lock);
				error = ark_disp_set_layer_cfg(&arg);
				spin_unlock(&context->disp_obj[layer_id].lock);

				if (error == 0)
					printk(KERN_ALERT "colorkey parameters set done\n");
				else
					printk(KERN_ALERT "colorkey parameters set failed!\n");
			}
			break;
		case 10: /* set alpha_blend_en, per_pix_alpha_blend_en, blend_mode, alpha */
			{
				unsigned int layer_id, alpha_blend_en, per_pix_alpha_blend_en,
					     blend_mode, alpha;
				struct ark_disp_layer_info *layer;
				struct ark_disp_layer_cfg_arg arg;
				int error;

				sscanf(buffer, "%*d %d  %x %x %x %x",
						&layer_id,
						&alpha_blend_en, &per_pix_alpha_blend_en, &blend_mode, &alpha);

				if (layer_id >= ARK_DISP_LAYER_NUM) {
					printk(KERN_ALERT
							"invalid layer_id=%d (should be less than %d)\n",
							layer_id, ARK_DISP_LAYER_NUM);
					break;
				}

				layer = &context->disp_obj[layer_id].layer_info;

				ark_disp_copy_layer_info_to_cfg_arg(layer_id, &arg);

				arg.alpha_blend_en = alpha_blend_en;
				arg.per_pix_alpha_blend_en  = per_pix_alpha_blend_en;
				arg.blend_mode = blend_mode;
				arg.alpha = alpha;

				printk(KERN_ALERT
						"alpha_blend_en=%d per_pix_alpha_blend_en=%d "
						"blend_mode=%02x alpha=%02x\n",
						alpha_blend_en, per_pix_alpha_blend_en, blend_mode, alpha);

				spin_lock(&context->disp_obj[layer_id].lock);
				error = ark_disp_try_layer_cfg(&arg);
				spin_unlock(&context->disp_obj[layer_id].lock);

				if (error) {
					printk(KERN_ALERT "layer format try failed\n");
					break;
				}
				printk(KERN_ALERT "layer format try done\n");

				spin_lock(&context->disp_obj[layer_id].lock);
				error = ark_disp_set_layer_cfg(&arg);
				spin_unlock(&context->disp_obj[layer_id].lock);

				if (error == 0)
					printk(KERN_ALERT "blend control parameters set done\n");
				else
					printk(KERN_ALERT "blend control parameters set failed!\n");
			}
			break;
		case 11: /* set lcd layer priority */
			{
				unsigned int prority_layer_0, prority_layer_1, prority_layer_2,
					     prority_layer_3, prority_layer_4;
				int error;

				sscanf(buffer, "%*d  %d %d %d %d %d",
						&prority_layer_0,
						&prority_layer_1,
						&prority_layer_2,
						&prority_layer_3,
						&prority_layer_4
				      );

				printk(KERN_ALERT
						"lcd layer priority:\n"
						"prority_layer_0=%d prority_layer_1=%d prority_layer_2=%d "
						"prority_layer_3=%d prority_layer_4=%d\n",
						prority_layer_0,
						prority_layer_1,
						prority_layer_2,
						prority_layer_3,
						prority_layer_4
				      );

				spin_lock(&context->disp_obj[0].lock);
				spin_lock(&context->disp_obj[1].lock);
				spin_lock(&context->disp_obj[2].lock);
				spin_lock(&context->disp_obj[3].lock);
				spin_lock(&context->disp_obj[4].lock);
				error =ark_disp_set_layer_priority_lcd(
						prority_layer_0,
						prority_layer_1,
						prority_layer_2,
						prority_layer_3,
						prority_layer_4
						);
				spin_unlock(&context->disp_obj[0].lock);
				spin_unlock(&context->disp_obj[1].lock);
				spin_unlock(&context->disp_obj[2].lock);
				spin_unlock(&context->disp_obj[3].lock);
				spin_unlock(&context->disp_obj[4].lock);

				if (error == 0)
					printk(KERN_ALERT "lcd layer priority set done\n");
				else
					printk(KERN_ALERT "lcd layer priority set failed!\n");
			}
			break;
		case 12: /* set tvenc layer priority */
			{
				unsigned int prority_layer_0, prority_layer_1, prority_layer_2,
					     prority_layer_3, prority_layer_4;
				int error;

				sscanf(buffer, "%*d  %d %d %d %d %d",
						&prority_layer_0,
						&prority_layer_1,
						&prority_layer_2,
						&prority_layer_3,
						&prority_layer_4
				      );

				printk(KERN_ALERT
						"tvenc layer priority:\n"
						"prority_layer_0=%d prority_layer_1=%d prority_layer_2=%d "
						"prority_layer_3=%d prority_layer_4=%d\n",
						prority_layer_0,
						prority_layer_1,
						prority_layer_2,
						prority_layer_3,
						prority_layer_4
				      );

				spin_lock(&context->disp_obj[0].lock);
				spin_lock(&context->disp_obj[1].lock);
				spin_lock(&context->disp_obj[2].lock);
				spin_lock(&context->disp_obj[3].lock);
				spin_lock(&context->disp_obj[4].lock);
				error =ark_disp_set_layer_priority_tvenc(
						prority_layer_0,
						prority_layer_1,
						prority_layer_2,
						prority_layer_3,
						prority_layer_4
						);
				spin_unlock(&context->disp_obj[0].lock);
				spin_unlock(&context->disp_obj[1].lock);
				spin_unlock(&context->disp_obj[2].lock);
				spin_unlock(&context->disp_obj[3].lock);
				spin_unlock(&context->disp_obj[4].lock);

				if (error == 0)
					printk(KERN_ALERT "tvenc layer priority set done\n");
				else
					printk(KERN_ALERT "tvenc layer priority set failed!\n");
			}
			break;
		case 13: /* set layer format: interlaced, pixfmt, rgb_order, yuyv_order */
			{
				unsigned int layer_id, interlaced, pixfmt, rgb_order, yuyv_order;
				struct ark_disp_layer_info *layer;
				struct ark_disp_layer_cfg_arg arg;
				int error;

				sscanf(buffer, "%*d %d  %d %d %d %d",
						&layer_id, &interlaced, &pixfmt, &rgb_order, &yuyv_order);

				if (layer_id >= ARK_DISP_LAYER_NUM) {
					printk(KERN_ALERT
							"invalid layer_id=%d (should be less than %d)\n",
							layer_id, ARK_DISP_LAYER_NUM);
					break;
				}

				layer = &context->disp_obj[layer_id].layer_info;

				ark_disp_copy_layer_info_to_cfg_arg(layer_id, &arg);

				arg.interlaced = interlaced;
				arg.pixfmt = pixfmt;
				arg.rgb_order = rgb_order;
				arg.yuyv_order = yuyv_order;

				printk(KERN_ALERT
						"layer_id=%d "
						"interlaced=%d pixfmt=%d rgb_order=%d yuyv_order=%d\n",
						layer_id, interlaced, pixfmt, rgb_order, yuyv_order);

				spin_lock(&context->disp_obj[layer_id].lock);
				error = ark_disp_try_layer_cfg(&arg);
				spin_unlock(&context->disp_obj[layer_id].lock);

				if (error) {
					printk(KERN_ALERT "layer format try failed\n");
					break;
				}
				printk(KERN_ALERT "layer format try done\n");

				spin_lock(&context->disp_obj[layer_id].lock);
				error = ark_disp_set_layer_cfg(&arg);
				spin_unlock(&context->disp_obj[layer_id].lock);

				if (error == 0)
					printk(KERN_ALERT "layer format set done\n");
				else
					printk(KERN_ALERT "layer format set failed!\n");
			}
			break;
		case 14: /* view v4l2 driver info */
			{
				unsigned int layer_id;

				sscanf(buffer, "%*d %x", &layer_id);

				if (layer_id >= ARK_DISP_LAYER_NUM) {
					printk(KERN_ALERT
							"invalid layer_id=%d (should be less than %d)\n",
							layer_id, ARK_DISP_LAYER_NUM);
					break;
				}

				printk(KERN_ALERT
						"v4l2 driver info of layer%d:\n"
						"\t memory = %s\n"
						//                "\t prio = %s\n"
						"\t pix_fmt.width  = %d\n"
						"\t pix_fmt.height = %d\n"
						"\t pix_fmt.pixelformat = %s\n"
						"\t pix_fmt.field = %s\n"
						"\t pix_fmt.bytesperline = %d\n"
						"\t pix_fmt.sizeimage = %d\n"
						"\t pix_fmt.colorspace = %s\n"
						"\t io_usrs = %d\n"
						"\t field_id = %d\n"
						"\t started = %d\n"
						"\t output_queue_entry_count = %d\n"
						"\t vbuf_list_entry_count = %d\n"
						"\t out_buf_cnt = %d\n"
						"\t done_buf_cnt = %d\n"
						"\n",
						layer_id,
						(context->disp_obj[layer_id].vid_info.memory == V4L2_MEMORY_MMAP)? "V4L2_MEMORY_MMAP" :
							(context->disp_obj[layer_id].vid_info.memory == V4L2_MEMORY_USERPTR)? "V4L2_MEMORY_USERPTR" :
							"unknown",
						//                (context->disp_obj[layer_id].vid_info.prio == V4L2_PRIORITY_UNSET)? "V4L2_PRIORITY_UNSET" :
						//                (context->disp_obj[layer_id].vid_info.prio == V4L2_PRIORITY_BACKGROUND)? "V4L2_PRIORITY_BACKGROUND" :
						//                (context->disp_obj[layer_id].vid_info.prio == V4L2_PRIORITY_INTERACTIVE)? "V4L2_PRIORITY_INTERACTIVE" :
						//                (context->disp_obj[layer_id].vid_info.prio == V4L2_PRIORITY_RECORD)? "V4L2_PRIORITY_RECORD" :
						//                (context->disp_obj[layer_id].vid_info.prio == V4L2_PRIORITY_DEFAULT)? "V4L2_PRIORITY_DEFAULT" :
						//                "unknown",
						context->disp_obj[layer_id].vid_info.pix_fmt.width,
						context->disp_obj[layer_id].vid_info.pix_fmt.height,
						(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_RGB332)? "V4L2_PIX_FMT_RGB332" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_RGB444)? "V4L2_PIX_FMT_RGB444" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_RGB555)? "V4L2_PIX_FMT_RGB555" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_RGB565)? "V4L2_PIX_FMT_RGB565" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_RGB555X)? "V4L2_PIX_FMT_RGB555X" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_RGB565X)? "V4L2_PIX_FMT_RGB565X" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_BGR24)? "V4L2_PIX_FMT_BGR24" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_RGB24)? "V4L2_PIX_FMT_RGB24" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_BGR32)? "V4L2_PIX_FMT_BGR32" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_RGB32)? "V4L2_PIX_FMT_RGB32" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_GREY)? "V4L2_PIX_FMT_GREY" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_Y16)? "V4L2_PIX_FMT_Y16" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_PAL8)? "V4L2_PIX_FMT_PAL8" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YVU410)? "V4L2_PIX_FMT_YVU410" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YVU420)? "V4L2_PIX_FMT_YVU420" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YUYV)? "V4L2_PIX_FMT_YUYV" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_UYVY)? "V4L2_PIX_FMT_UYVY" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_VYUY)? "V4L2_PIX_FMT_VYUY" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YUV422P)? "V4L2_PIX_FMT_YUV422P" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YUV411P)? "V4L2_PIX_FMT_YUV411P" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_Y41P)? "V4L2_PIX_FMT_Y41P" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YUV444)? "V4L2_PIX_FMT_YUV444" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YUV555)? "V4L2_PIX_FMT_YUV555" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YUV565)? "V4L2_PIX_FMT_YUV565" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YUV32)? "V4L2_PIX_FMT_YUV32" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_NV12)? "V4L2_PIX_FMT_NV12" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_NV21)? "V4L2_PIX_FMT_NV21" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_NV16)? "V4L2_PIX_FMT_NV16" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YUV410)? "V4L2_PIX_FMT_YUV410" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YUV420)? "V4L2_PIX_FMT_YUV420" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YYUV)? "V4L2_PIX_FMT_YYUV" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_HI240)? "V4L2_PIX_FMT_HI240" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_HM12)? "V4L2_PIX_FMT_HM12" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_SBGGR8)? "V4L2_PIX_FMT_SBGGR8" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_SGBRG8)? "V4L2_PIX_FMT_SGBRG8" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_SGRBG10)? "V4L2_PIX_FMT_SGRBG10" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_SGRBG10DPCM8)? "V4L2_PIX_FMT_SGRBG10DPCM8" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_SBGGR16)? "V4L2_PIX_FMT_SBGGR16" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_MJPEG)? "V4L2_PIX_FMT_MJPEG" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_JPEG)? "V4L2_PIX_FMT_JPEG" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_DV)? "V4L2_PIX_FMT_DV" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_MPEG)? "V4L2_PIX_FMT_MPEG" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_WNVA)? "V4L2_PIX_FMT_WNVA" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_SN9C10X)? "V4L2_PIX_FMT_SN9C10X" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_PWC1)? "V4L2_PIX_FMT_PWC1" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_PWC2)? "V4L2_PIX_FMT_PWC2" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_ET61X251)? "V4L2_PIX_FMT_ET61X251" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_SPCA501)? "V4L2_PIX_FMT_SPCA501" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_SPCA505)? "V4L2_PIX_FMT_SPCA505" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_SPCA508)? "V4L2_PIX_FMT_SPCA508" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_SPCA561)? "V4L2_PIX_FMT_SPCA561" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_PAC207)? "V4L2_PIX_FMT_PAC207" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_PJPG)? "V4L2_PIX_FMT_PJPG" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.pixelformat == V4L2_PIX_FMT_YVYU)? "V4L2_PIX_FMT_YVYU" :
							"unknown",
						(context->disp_obj[layer_id].vid_info.pix_fmt.field == V4L2_FIELD_ANY)? "V4L2_FIELD_ANY" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.field == V4L2_FIELD_NONE)? "V4L2_FIELD_NONE" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.field == V4L2_FIELD_TOP)? "V4L2_FIELD_TOP" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.field == V4L2_FIELD_BOTTOM)? "V4L2_FIELD_BOTTOM" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.field == V4L2_FIELD_INTERLACED)? "V4L2_FIELD_INTERLACED" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.field == V4L2_FIELD_SEQ_TB)? "V4L2_FIELD_SEQ_TB" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.field == V4L2_FIELD_SEQ_BT)? "V4L2_FIELD_SEQ_BT" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.field == V4L2_FIELD_ALTERNATE)? "V4L2_FIELD_ALTERNATE" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.field == V4L2_FIELD_INTERLACED_TB)? "V4L2_FIELD_INTERLACED_TB" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.field == V4L2_FIELD_INTERLACED_BT)? "V4L2_FIELD_INTERLACED_BT" :
							"unknown",
						context->disp_obj[layer_id].vid_info.pix_fmt.bytesperline,
						context->disp_obj[layer_id].vid_info.pix_fmt.sizeimage,
						(context->disp_obj[layer_id].vid_info.pix_fmt.colorspace == V4L2_COLORSPACE_SMPTE170M)? "V4L2_COLORSPACE_SMPTE170M" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.colorspace == V4L2_COLORSPACE_SMPTE240M)? "V4L2_COLORSPACE_SMPTE240M" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.colorspace == V4L2_COLORSPACE_REC709)? "V4L2_COLORSPACE_REC709" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.colorspace == V4L2_COLORSPACE_BT878)? "V4L2_COLORSPACE_BT878" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.colorspace == V4L2_COLORSPACE_470_SYSTEM_M)? "V4L2_COLORSPACE_470_SYSTEM_M" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.colorspace == V4L2_COLORSPACE_470_SYSTEM_BG)? "V4L2_COLORSPACE_470_SYSTEM_BG" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.colorspace == V4L2_COLORSPACE_JPEG)? "V4L2_COLORSPACE_JPEG" :
							(context->disp_obj[layer_id].vid_info.pix_fmt.colorspace == V4L2_COLORSPACE_SRGB)? "V4L2_COLORSPACE_SRGB" :
							"unknown",
						context->disp_obj[layer_id].vid_info.io_usrs,
						context->disp_obj[layer_id].vid_info.field_id,
						context->disp_obj[layer_id].vid_info.started,
						context->disp_obj[layer_id].vid_info.output_queue_entry_count,
						context->disp_obj[layer_id].vid_info.vbuf_list_entry_count,
						context->disp_obj[layer_id].vid_info.out_buf_cnt,
						context->disp_obj[layer_id].vid_info.done_buf_cnt
							);
			}
			break;
		case 15: /* get tvenc config */
			{
				struct ark_disp_tvenc_cfg_arg input_arg;

				if (ark_disp_get_tvenc_cfg(&input_arg) != 0) {
					printk(KERN_ALERT "%s %d: get cfg error\n",
							__FUNCTION__, __LINE__);
					break;
				}

				printk(KERN_ALERT
						"tvenc_config.enable       = %d\n"
						"tvenc_config.out_mode     = %s\n"
						"tvenc_config.backcolor_y  = 0x%02x\n"
						"tvenc_config.backcolor_cb = 0x%02x\n"
						"tvenc_config.backcolor_cr = 0x%02x\n",
						input_arg.enable,
						(input_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_I480HZ60)? "ARKDISP_TVENC_OUT_YPBPR_I480HZ60" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_I576HZ50)? "ARKDISP_TVENC_OUT_YPBPR_I576HZ50" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_P480HZ60)? "ARKDISP_TVENC_OUT_YPBPR_P480HZ60" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_P576HZ50)? "ARKDISP_TVENC_OUT_YPBPR_P576HZ50" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_P720HZ60)? "ARKDISP_TVENC_OUT_YPBPR_P720HZ60" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_P720HZ50)? "ARKDISP_TVENC_OUT_YPBPR_P720HZ50" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_I1080HZ60)? "ARKDISP_TVENC_OUT_YPBPR_I1080HZ60" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_I1080HZ50)? "ARKDISP_TVENC_OUT_YPBPR_I1080HZ50" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_I1080HZ50_1250)? "ARKDISP_TVENC_OUT_YPBPR_I1080HZ50_1250" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_P1080HZ60)? "ARKDISP_TVENC_OUT_YPBPR_P1080HZ60" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_P1080HZ50)? "ARKDISP_TVENC_OUT_YPBPR_P1080HZ50" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_CVBS_PAL)? "ARKDISP_TVENC_OUT_CVBS_PAL" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_CVBS_NTSC)? "ARKDISP_TVENC_OUT_CVBS_NTSC" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_ITU656_PAL)? "ARKDISP_TVENC_OUT_ITU656_PAL" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_ITU656_NTSC)? "ARKDISP_TVENC_OUT_ITU656_NTSC" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_VGA_640x480HZ60)? "ARKDISP_TVENC_OUT_VGA_640x480HZ60" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_VGA_800x600HZ60)? "ARKDISP_TVENC_OUT_VGA_800x600HZ60" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_VGA_1024x768HZ60)? "ARKDISP_TVENC_OUT_VGA_1024x768HZ60" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_VGA_1280x1024HZ60)? "ARKDISP_TVENC_OUT_VGA_1280x1024HZ60" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_VGA_1900x1200HZ60)? "ARKDISP_TVENC_OUT_VGA_1900x1200HZ60" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_VGA_1280x1024HZ75)? "ARKDISP_TVENC_OUT_VGA_1280x1024HZ75" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_VGA_1280x960HZ85)? "ARKDISP_TVENC_OUT_VGA_1280x960HZ85" :
						(input_arg.out_mode == ARKDISP_TVENC_OUT_VGA_1280x720HZ60)? "ARKDISP_TVENC_OUT_VGA_1280x720HZ60" :
						"unknown",
					input_arg.backcolor_y,
					input_arg.backcolor_cb,
					input_arg.backcolor_cr
						);
			}
			break;
		case 16: /* set tvenc config */
			{
				struct ark_disp_tvenc_cfg_arg arg;
				int enable, out_mode, backcolor_y, backcolor_cb, backcolor_cr;
				int error;

				sscanf(buffer, "%*d %d %d %x %x %x", &enable, &out_mode,
						&backcolor_y, &backcolor_cb, &backcolor_cr);

				arg.enable       = enable;
				arg.out_mode     = out_mode;
				arg.backcolor_y  = backcolor_y;
				arg.backcolor_cb = backcolor_cb;
				arg.backcolor_cr = backcolor_cr;

				spin_lock(&context->tvenc_cfg.lock);
				error = ark_disp_set_tvenc_cfg(&arg);
				spin_unlock(&context->tvenc_cfg.lock);

				if (error == 0)
					printk(KERN_ALERT "tvenc parameters set done\n");
				else
					printk(KERN_ALERT "tvenc parameters set failed!\n");
			}
			break;
		case 17: /* get lcd config */
			{
				struct ark_disp_lcd_cfg_arg input_arg;

				if (ark_disp_get_lcd_cfg(&input_arg) != 0) {
					printk(KERN_ALERT "%s %d: get cfg error\n",
							__FUNCTION__, __LINE__);
					break;
				}

				printk(KERN_ALERT
						"lcd_config.enable       = %d\n"
						"lcd_config.panel_type   = %s\n"
						"lcd_config.backcolor_y  = 0x%02x\n"
						"lcd_config.backcolor_cb = 0x%02x\n"
						"lcd_config.backcolor_cr = 0x%02x\n",
						input_arg.enable,
						(input_arg.panel_type == ARKDISP_LCD_PANEL_PARALLEL_24BIT)? "ARKDISP_LCD_PANEL_PARALLEL_24BIT" :
						(input_arg.panel_type == ARKDISP_LCD_PANEL_PARALLEL_18BIT)? "ARKDISP_LCD_PANEL_PARALLEL_18BIT" :
						(input_arg.panel_type == ARKDISP_LCD_PANEL_PARALLEL_16BIT)? "ARKDISP_LCD_PANEL_PARALLEL_16BIT" :
						(input_arg.panel_type == ARKDISP_LCD_PANEL_SRGB)? "ARKDISP_LCD_PANEL_SRGB" :
						(input_arg.panel_type == ARKDISP_LCD_PANEL_CPU_SCREEN)? "ARKDISP_LCD_PANEL_CPU_SCREEN" :
						"unknown",
						input_arg.backcolor_y,
						input_arg.backcolor_cb,
						input_arg.backcolor_cr
				      );
			}
			break;
		case 18: /* set tvenc config */
			{
				struct ark_disp_lcd_cfg_arg arg;
				int enable, panel_type, backcolor_y, backcolor_cb, backcolor_cr;
				int error;

				sscanf(buffer, "%*d %d %d %x %x %x", &enable, &panel_type,
						&backcolor_y, &backcolor_cb, &backcolor_cr);

				arg.enable       = enable;
				arg.panel_type   = panel_type;
				arg.backcolor_y  = backcolor_y;
				arg.backcolor_cb = backcolor_cb;
				arg.backcolor_cr = backcolor_cr;

				spin_lock(&context->lcd_cfg.lock);
				error = ark_disp_set_lcd_cfg(&arg);
				spin_unlock(&context->lcd_cfg.lock);

				if (error == 0)
					printk(KERN_ALERT "lcd parameters set done\n");
				else
					printk(KERN_ALERT "lcd parameters set failed!\n");
			}
			break;

		case 21: /* set itu656 direct data in enable */
			{
				int enable;

				sscanf(buffer, "%*d %d", &enable);

				printk(KERN_ALERT "set itu656 data in enable to: %d\n",
						enable);

				if (ark_disp_set_itu656in_en(enable) != 0) {
					printk(KERN_ALERT "itu656in_en set failed (%d)\n",
							context->itu656in_en);
				} else {
					printk(KERN_ALERT "itu656in_en set success (%d)\n",
							context->itu656in_en);
				}
			}
			break;
		case 22: /*open lcd*/
			/*{  
				int  id, enable;	
				struct ark_disp_buf_info *buf;
				buf = &context->disp_obj[0].buf_info;
				
				sscanf(buffer, "%*d %d", &enable);
				if(enable){
					ark_disp_set_osd_data_addr(0, buf->buf_pool_base_phys); //set framebuffer addr for osd1,it default address
					for (id = 0; id < ARK_DISP_OSD_NUM; id++) {
						spin_lock(&context->disp_obj[id].lock);
						ark_disp_set_osd_format(id, 6, 0, 0);
						spin_unlock(&context->disp_obj[id].lock);
						boot_complete = 1;
					}
					ark_disp_enable_interrupts();
				}else{
					ark_disp_disable_interrupts();
				}
			}*/
			break;
		case 23: //show framebuffer
			{
				struct ark_disp_obj_info *disp;
				struct ark_disp_buf_info *buf;
				struct ark_disp_layer_info *layer;
				int id = 0;

				disp = &context->disp_obj[id];
				buf = &disp->buf_info;
				layer = &disp->layer_info;
				ark_disp_set_osd_format(id, ARK_DISP_OSD_PIXFMT_RGB888, 0, 0);
				ark_disp_set_osd_data_addr(id, buf->buf_pool_base_phys);
				ark_disp_set_osd_colorkey(id, 1, 0x10, 0x80, 0x80);
				if (layer->screen == ARKDISP_SCREEN_TVENC) {	
				   ark_disp_set_osd_blend_mode_tvenc(id, 2);
				   ark_disp_set_osd_alpha_blend_en_tvenc(id, 1);				   	
				} else {
				   ark_disp_set_osd_blend_mode_lcd(id, 2);
				   ark_disp_set_osd_alpha_blend_en_lcd(id, 1);
				}
				boot_complete = 1;
			}
			break;
		case 24: //fb0 show/hide
			{
				struct ark_disp_obj_info *disp;
				struct ark_disp_layer_info *layer;
				int enable;

				sscanf(buffer, "%*d %d", &enable);				

				disp = &context->disp_obj[0];
				layer = &disp->layer_info;
				if (layer->screen == ARKDISP_SCREEN_TVENC) {	
				   ark_disp_set_osd_en_tvenc(0, enable);			   	
				} else {
				   ark_disp_set_osd_en_lcd(0, enable);
				}
			}		
			break;
		default:
			printk(KERN_ALERT
					" 0:read reg, 1:write reg\n");
			break;
	}

	return count;
}
#endif //#ifndef ARK_DISP_USE_PROC_DIR_ENTRY


/* This is the first function being called when opening the device
 * driver. It identifies the driver-specific data structure of the device
 * being opened and stores the start pointer of the data structure in the 
 * private_data field of the file structure, i.e. filp->private_data, 
 * for easier access in the future.
 *
 * Arguments:
 *   inode : kernel data structure that represents the device file of the
 *           driver on disk
 *   filp  : a pointer to the file structure that is created by the kernel
 *           to represent the file opened for the device driver
 *
 * Return:
 *   0
 */
static int ark_disp_open(struct inode *inode, struct file *filp)
{
	struct ark_disp_device *dev;

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);
	dev = container_of(filp->private_data,
					     struct ark_disp_device, ark_disp_miscdevice);
	filp->private_data = dev;

	return 0;
}

static int ark_display_fasync(int fd, struct file *filp, int mode)
{
	struct ark_disp_device *disp =
		(struct ark_disp_device *)filp->private_data;
	return fasync_helper(fd,filp,mode,&disp->async_queue);
}

/* This function is invoked when the associated file structure is being
 * released.
 *
 * Arguments:
 *   inode : kernel data structure that represents the device file of the
 *           driver on disk
 *   filp  : a pointer to the file structure that is created by the kernel
 *           to represent the file opened for the device driver
 *
 * Return:
 *   0
 */

static int ark_disp_release(struct inode *inode, struct file *filp)
{
	struct ark_disp_device *dev = NULL;

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	dev = container_of(filp->private_data,
					     struct ark_disp_device, ark_disp_miscdevice);

	if(filp->f_flags & FASYNC)
	{
		/* remove this filp from the asynchronusly notified filp's */
		ark_display_fasync(-1, filp, 0);
	}

	/* NOTE: intend not to do anything here */

	return 0;
}


/* This function is invoked to handle ioctl system calls and implement
 * the requested ioctl command. It validates the ioctl command and performs
 * the requested device control function. If the command number does not
 * match a valid operation, it returns -ENOTTY. Otherwise, it returns 0
 *
 * Arguments:
 *   inode : kernel data structure that represents the device file of the
 *           driver on disk
 *   filp  : a pointer to the file structure that is created by the kernel
 *           to represent the file opened for the device driver
 *
 * Return:
 *   0
 */
static long ark_disp_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct ark_disp_device *disp =
		(struct ark_disp_device *)file->private_data;

	struct ark_disp_context *context = &disp->context;
	struct ark_disp_obj_info *disp_tvout = &context->disp_obj[3];
	int res = 0;

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);
	switch (cmd) {
		case ARKDISP_NO_OPERATION:
			ARKDISP_DBGPRTK("%s %d: no operation test (0x%2X)\n",
					__FUNCTION__, __LINE__, cmd);
			break;
		case ARKDISP_GET_VDE_CFG:
			{
				struct ark_disp_vde_cfg_arg input_arg;

				ARKDISP_DBGPRTK("%s %d: ARKDISP_GET_VDE_CFG (0x%2X)\n",
						__FUNCTION__, __LINE__, cmd);

				if (arg == (unsigned long)NULL) {
					ARKDISP_DBGPRTK("%s %d: null arg error\n",
							__FUNCTION__, __LINE__);
					return -EINVAL;
				}

				if (copy_from_user(&input_arg, (void*)arg, sizeof(input_arg))) {
					ARKDISP_DBGPRTK("%s %d: copy_from_user error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}

				if (input_arg.layer_id >= ARK_DISP_LAYER_NUM) {
					ARKDISP_DBGPRTK("%s %d: invalid layer_id (%d)\n",
							__FUNCTION__, __LINE__, input_arg.layer_id);
					return -EINVAL;
				}

				if (ark_disp_get_vde_cfg(&input_arg) != 0) {
					ARKDISP_DBGPRTK("%s %d: get cfg error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}

				if (copy_to_user((void*)arg, &input_arg, sizeof(input_arg))) {
					ARKDISP_DBGPRTK("%s %d: copy_to_user error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}
			}
			break;
		case ARKDISP_SET_VDE_CFG:
			{
				struct ark_disp_vde_cfg_arg input_arg;

				ARKDISP_DBGPRTK("%s %d: ARKDISP_SET_VDE_CFG (0x%2X)\n",
						__FUNCTION__, __LINE__, cmd);

				if (arg == (unsigned long)NULL) {
					ARKDISP_DBGPRTK("%s %d: null arg error\n",
							__FUNCTION__, __LINE__);
					return -EINVAL;
				}

				if (copy_from_user(&input_arg, (void*)arg, sizeof(input_arg))) {
					ARKDISP_DBGPRTK("%s %d: copy_from_user error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}

				if (input_arg.layer_id >= ARK_DISP_LAYER_NUM) {
					ARKDISP_DBGPRTK("%s %d: invalid layer_id (%d)\n",
							__FUNCTION__, __LINE__, input_arg.layer_id);
					return -EINVAL;
				}
				if (input_arg.hue > 0xFF) {
					ARKDISP_DBGPRTK("%s %d: invalid hue (%d)\n",
							__FUNCTION__, __LINE__, input_arg.hue);
					return -EINVAL;
				}
				if (input_arg.saturation > 0xFF) {
					ARKDISP_DBGPRTK("%s %d: invalid saturation (%d)\n",
							__FUNCTION__, __LINE__, input_arg.saturation);
					return -EINVAL;
				}
				if (input_arg.brightness > 0xFF) {
					ARKDISP_DBGPRTK("%s %d: invalid brightness (%d)\n",
							__FUNCTION__, __LINE__, input_arg.brightness);
					return -EINVAL;
				}
				if (input_arg.contrast > 0xFF) {
					ARKDISP_DBGPRTK("%s %d: invalid contrast (%d)\n",
							__FUNCTION__, __LINE__, input_arg.contrast);
					return -EINVAL;
				}

				if (ark_disp_set_vde_cfg(&input_arg) != 0) {
					ARKDISP_DBGPRTK("%s %d: set cfg error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}
			}
			break;
		case ARKDISP_GET_LAYER_CFG:
			{
				struct ark_disp_layer_cfg_arg input_arg;

				ARKDISP_DBGPRTK("%s %d: ARKDISP_GET_LAYER_CFG (0x%2X)\n",
						__FUNCTION__, __LINE__, cmd);

				if (arg == (unsigned long)NULL) {
					ARKDISP_DBGPRTK("%s %d: null arg error\n",
							__FUNCTION__, __LINE__);
					return -EINVAL;
				}

				if (copy_from_user(&input_arg, (void*)arg, sizeof(input_arg))) {
					ARKDISP_DBGPRTK("%s %d: copy_from_user error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}

				/* validate the inputs */
				if (input_arg.layer_id >= ARK_DISP_LAYER_NUM) {
					ARKDISP_DBGPRTK("%s %d: invalid layer_id (%d)\n",
							__FUNCTION__, __LINE__, input_arg.layer_id);
					return -EINVAL;
				}

				if (ark_disp_get_layer_cfg(&input_arg) != 0) {
					ARKDISP_DBGPRTK("%s %d: get cfg error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}

				if (copy_to_user((void*)arg, &input_arg, sizeof(input_arg))) {
					ARKDISP_DBGPRTK("%s %d: copy_to_user error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}
			}
			break;
		case ARKDISP_SET_LAYER_CFG:
			{
				struct ark_disp_layer_cfg_arg input_arg;

				ARKDISP_DBGPRTK("%s %d: ARKDISP_SET_LAYER_CFG (0x%2X)\n",
						__FUNCTION__, __LINE__, cmd);

				if (arg == (unsigned long)NULL) {
					ARKDISP_DBGPRTK("%s %d: null arg error\n",
							__FUNCTION__, __LINE__);
					return -EINVAL;
				}

				if (copy_from_user(&input_arg, (void*)arg, sizeof(input_arg))) {
					ARKDISP_DBGPRTK("%s %d: copy_from_user error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}

				/* validate the inputs */
				if (input_arg.layer_id >= ARK_DISP_LAYER_NUM) {
					ARKDISP_DBGPRTK("%s %d: invalid layer_id (%d)\n",
							__FUNCTION__, __LINE__, input_arg.layer_id);
					return -EINVAL;
				}
				if (input_arg.enable != 0 && input_arg.enable != 1) {
					ARKDISP_DBGPRTK("%s %d: invalid enable (%d)\n",
							__FUNCTION__, __LINE__, input_arg.enable);
					return -EINVAL;
				}
				if (input_arg.interlaced != 0 && input_arg.interlaced != 1) {
					ARKDISP_DBGPRTK("%s %d: invalid interlaced (%d)\n",
							__FUNCTION__, __LINE__, input_arg.interlaced);
					return -EINVAL;
				}
				if (input_arg.alpha_blend_en != 0 &&
						input_arg.alpha_blend_en != 1) {
					ARKDISP_DBGPRTK("%s %d: invalid back_blend_en (%d)\n",
							__FUNCTION__, __LINE__, input_arg.alpha_blend_en);
					return -EINVAL;
				}
				if (input_arg.type != ARKDISP_LAYER_OSD &&
						input_arg.type != ARKDISP_LAYER_VIDEO) {
					ARKDISP_DBGPRTK("%s %d: invalid type (%d)\n",
							__FUNCTION__, __LINE__, input_arg.type);
					return -EINVAL;
				}
				if (input_arg.screen != ARKDISP_SCREEN_LCD &&
						input_arg.screen != ARKDISP_SCREEN_TVENC) {
					ARKDISP_DBGPRTK("%s %d: invalid screen (%d)\n",
							__FUNCTION__, __LINE__, input_arg.screen);
					return -EINVAL;
				}
				/* NOTE: pixfmt will be validated in ark_disp_set_layer_cfg() */

				if (input_arg.src_xres > ARK_DISP_SRC_XRES_MAX) {
					ARKDISP_DBGPRTK("%s %d: invalid src_xres (%d)\n",
							__FUNCTION__, __LINE__, input_arg.src_xres);
					return -EINVAL;
				}
				if (input_arg.src_yres > ARK_DISP_SRC_YRES_MAX) {
					ARKDISP_DBGPRTK("%s %d: invalid src_yres (%d)\n",
							__FUNCTION__, __LINE__, input_arg.src_yres);
					return -EINVAL;
				}
				if (input_arg.xres > ARK_DISP_OUT_XRES_MAX) {
					ARKDISP_DBGPRTK("%s %d: invalid xres (%d)\n",
							__FUNCTION__, __LINE__, input_arg.xres);
					return -EINVAL;
				}
				if (input_arg.yres > ARK_DISP_OUT_YRES_MAX) {
					ARKDISP_DBGPRTK("%s %d: invalid yres (%d)\n",
							__FUNCTION__, __LINE__, input_arg.yres);
					return -EINVAL;
				}
				if (ark_disp_try_layer_cfg(&input_arg) != 0) {
					ARKDISP_DBGPRTK("%s %d: try cfg error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}
				if (ark_disp_set_layer_cfg(&input_arg) != 0) {
					ARKDISP_DBGPRTK("%s %d: set cfg error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}
			}
			break;


		case ARKDISP_GET_TVENC_CFG:
			{
				struct ark_disp_tvenc_cfg_arg input_arg;

				ARKDISP_DBGPRTK("%s %d: ARKDISP_GET_TVENC_CFG (0x%2X)\n",
						__FUNCTION__, __LINE__, cmd);

				if (arg == (unsigned long)NULL) {
					ARKDISP_DBGPRTK("%s %d: null arg error\n",
							__FUNCTION__, __LINE__);
					return -EINVAL;
				}

				if (copy_from_user(&input_arg, (void*)arg, sizeof(input_arg))) {
					ARKDISP_DBGPRTK("%s %d: copy_from_user error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}

				if (ark_disp_get_tvenc_cfg(&input_arg) != 0) {
					ARKDISP_DBGPRTK("%s %d: get cfg error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}

				if (copy_to_user((void*)arg, &input_arg, sizeof(input_arg))) {
					ARKDISP_DBGPRTK("%s %d: copy_to_user error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}
			}
			break;
		case ARKDISP_SET_TVENC_CFG:
			{
				struct ark_disp_tvenc_cfg_arg input_arg;

				ARKDISP_DBGPRTK("%s %d: ARKDISP_SET_TVENC_CFG (0x%2X)\n",
						__FUNCTION__, __LINE__, cmd);

				if (arg == (unsigned long)NULL) {
					ARKDISP_DBGPRTK("%s %d: null arg error\n",
							__FUNCTION__, __LINE__);
					return -EINVAL;
				}

				if (copy_from_user(&input_arg, (void*)arg, sizeof(input_arg))) {
					ARKDISP_DBGPRTK("%s %d: copy_from_user error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}

				if (input_arg.enable != 0 && input_arg.enable != 1) {
					ARKDISP_DBGPRTK("%s %d: invalid enable (%d)\n",
							__FUNCTION__, __LINE__, input_arg.enable);
					return -EINVAL;
				}
				if (input_arg.enable == 0)
					disp_tvout->show_tv = 0;
				if (ark_disp_set_tvenc_cfg(&input_arg) != 0) {
					ARKDISP_DBGPRTK("%s %d: set cfg error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}
			}
			break;
		case ARKDISP_SET_ITU656IN_ON:
			ARKDISP_DBGPRTK("%s %d: ARKDISP_SET_ITU656IN_ON (0x%2X)\n",
					__FUNCTION__, __LINE__, cmd);

			if (ark_disp_set_itu656in_en(1) != 0) {
				ARKDISP_DBGPRTK("%s %d: itu656in enable error\n",
						__FUNCTION__, __LINE__);
				return -EFAULT;
			}
			break;
		case ARKDISP_SET_ITU656IN_OFF:
			ARKDISP_DBGPRTK("%s %d: ARKDISP_SET_ITU656IN_OFF (0x%2X)\n",
					__FUNCTION__, __LINE__, cmd);
			if (ark_disp_set_itu656in_en(0) != 0) {
				ARKDISP_DBGPRTK("%s %d: itu656in disable error\n",
						__FUNCTION__, __LINE__);
				return -EFAULT;
			}
			break;
		case ARKDISP_SET_PRESCAL_ON:
			ARKDISP_DBGPRTK("%s %d: ARKDISP_SET_PRESCAL_ON (0x%2X)\n",
					__FUNCTION__, __LINE__, cmd);

			context->prescale_en = 1;
			break;
		case ARKDISP_SET_PRESCAL_OFF:
			ARKDISP_DBGPRTK("%s %d: ARKDISP_SET_PRESCAL_OFF (0x%2X)\n",
					__FUNCTION__, __LINE__, cmd);

			context->prescale_en = 0;
			break;
		case ARKDISP_GET_BACKCAR_STATUS:
			ARKDISP_DBGPRTK("%s %d: ARKDISP_SET_PRESCAL_OFF (0x%2X)\n",
					__FUNCTION__, __LINE__, cmd);
			{
				int flag;
				flag = uboot_backcar_status? 0 : 1;
				if (copy_to_user((void*)arg, &flag, sizeof(flag))) {
					ARKDISP_DBGPRTK("%s %d: copy_to_user error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}
			}
			break;
		case ARKDISP_SET_GUI_TVOUT:
			{
				int gui_tvout;

				if (copy_from_user(&gui_tvout, (void*)arg, sizeof(gui_tvout))) {
					ARKDISP_DBGPRTK("%s %d: copy_from_user error\n",
							__FUNCTION__, __LINE__);
					return -EFAULT;
				}		

				ark_disp_set_gui_tvout(gui_tvout);
				context->gui_tvout = gui_tvout;
			}
			break;
		default:
			ARKDISP_DBGPRTK("%s %d: undefined cmd (0x%2X)\n",
					__FUNCTION__, __LINE__, cmd);
			return -EINVAL;
	}

	/* verify the register contents */
#ifdef ARK_DISP_DBG
	if (ark_disp_reg_check(context) == 0)
		ARKDISP_DBGPRTK("%s %d: register check passed\n",
				__FUNCTION__, __LINE__);
	else
		ARKDISP_DBGPRTK("%s %d: register check failed!!\n",
				__FUNCTION__, __LINE__);
#endif

	return res;
}

static struct file_operations ark_disp_fops = {
	.owner          = THIS_MODULE,
	.open           = ark_disp_open,
	.unlocked_ioctl = ark_disp_ioctl,
	.release        = ark_disp_release,
	.fasync         = ark_display_fasync,
};

/* This function is invoked when the device module is loaded into the
 * kernel. It allocates system resources for constructing driver control
 * data structures and initializes thesm accordingly.
 */
static struct ark_disp_device *__disp_probe(struct platform_device *pdev)
{
	struct ark_disp_device *display;
	struct proc_dir_entry *proc_file;
	//struct resource *r;
	int error = 0;

	display = kzalloc(sizeof(struct ark_disp_device), GFP_KERNEL);
	if (display == NULL) {
		printk(KERN_ERR "%s %d: failed to allocate memory\n",
				__FUNCTION__, __LINE__);
		return ERR_PTR(-ENOMEM);
	}

	display->pdev = pdev;
	display->async_queue = NULL;
	display->driver_name = "ark_display_drv";
	display->name = "ark_display";
	display->context.irq = -1;
	display->context.scal_irq = -1;
	display->ark_disp_miscdevice.minor = MISC_DYNAMIC_MINOR;
	display->ark_disp_miscdevice.fops = &ark_disp_fops;
	display->ark_disp_miscdevice.name = "ark_display";

	error = misc_register(&display->ark_disp_miscdevice);
	if (error)
		goto out_misc_err;

#ifdef ARK_DISP_USE_PROC_DIR_ENTRY
	/* create procfs entry */
	proc_file = create_proc_entry( "display", 0644, NULL);
	proc_file->data = (void *)&display->context;
	proc_file->read_proc  = ark_disp_read_proc;
	proc_file->write_proc = ark_disp_write_proc;
#endif // ARK_DISP_PROC_DIR_ENTRY_ENABLE

	/* initialize hardware and data structure */
	error = ark_disp_dev_init(&display->context);
	if (error != 0) {
		printk(KERN_ERR "%s %d: dev init err\n", __FUNCTION__, __LINE__);
		goto err_init;
	}

	display->context.dev = &pdev->dev;

	/* register frame buffer drivers */
	error = ark_disp_fb_init(&display->context);
	if (error != 0) {
		printk(KERN_ERR "%s %d: fb init err\n", __FUNCTION__, __LINE__);
		goto err_init;
	}
#ifdef ARK1680_V4L2
	/* register v4l2 drivers */
	error = ark_disp_v4l2_init(&display->context);
	if (error != 0) {
		printk(KERN_ERR "%s %d: v4l2 dri init err\n", __FUNCTION__, __LINE__);
		goto err_init;
	}
#endif
	if (display->context.irq < 0) {
		display->context.irq = platform_get_irq(pdev, 0);
		if (display->context.irq < 0) {
			printk(KERN_ERR "%s %d: get lcd irq err\n", __FUNCTION__, __LINE__);
			error = -ENXIO;
			goto err_no_lcd_irq;
		}
	}

	ark_disp_io_init();
	ark_disp_set_itu656in_status(!uboot_backcar_status);
	if (!uboot_backcar_status && gpio_get_value(BACKCAR_GPIO)) {
		ark_disp_set_itu656in_en(0);
	} else if (uboot_backcar_status && !gpio_get_value(BACKCAR_GPIO)) {
		ark_disp_set_itu656in_en(1);
	}
	
	error = request_irq(display->context.irq, ark_disp_intr_handler, IRQF_SHARED, "display", display);
	if (error) {
		printk(KERN_ERR "request lcd irq error\n");
		goto err_req_lcd_irq;
	}
	
	if (display->context.scal_irq < 0) {
		display->context.scal_irq = platform_get_irq(pdev, 1);
		if (display->context.scal_irq < 0) {
			printk(KERN_ERR "%s %d: get scal_irq err\n",__FUNCTION__, __LINE__);
			error = -ENXIO;
			goto err_no_scal_irq;
		}
	}
	error = request_irq(display->context.scal_irq, ark_disp_scal_intr_handler, IRQF_SHARED, "display", display);
	if (error) {
		printk(KERN_ERR "%s %d: can't get assigned scal_irq %d, error %d\n",
				__FUNCTION__, __LINE__, display->context.scal_irq, error);
		goto err_req_scal_irq;
	}

	error = request_irq(gpio_to_irq(BACKCAR_GPIO), ark_backcar_intr_handler, 
		IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING, "display", display);
	if (error) {
		printk(KERN_ERR "%s %d: can't get assigned backcar irq %d, error %d\n",
				__FUNCTION__, __LINE__, gpio_to_irq(BACKCAR_GPIO), error);
		goto err_no_backcar_irq;
	}	

	ark_disp_enable_interrupts();
	platform_set_drvdata(pdev, display);

	return display;	

err_req_scal_irq:
err_no_scal_irq:
	free_irq(display->context.irq, display);
	display->context.irq = -1;
err_req_lcd_irq:	
err_no_lcd_irq:
err_no_backcar_irq:
err_init:

#ifdef ARK_DISP_USE_PROC_DIR_ENTRY
	remove_proc_entry("display", NULL);
#endif
	misc_deregister(&display->ark_disp_miscdevice);
out_misc_err:
	kfree(display);
	return ERR_PTR(error);
}

static int __devinit ark_disp_probe(struct platform_device *pdev)
{
	struct ark_disp_device *display;

	ark_disp_platform_device = pdev;

	display = __disp_probe(pdev);
	if (IS_ERR(display))
		return PTR_ERR(display);
	return 0;
}

/* This function is invoked when the device module is removed from the
 * kernel. It releases all resources to the system.
 */
static int __devexit ark_disp_remove(struct platform_device *pdev)
{
	struct ark_disp_device *display;

	display = platform_get_drvdata(pdev);
	if (display == NULL)
		return -ENODEV;

#ifdef ARK1680_V4L2
	/* free v4l2 driver memories */
	ark_disp_v4l2_release(&display->context);
#endif
	/* free the interrupt channel */
	if (display->context.irq >= 0) {
		free_irq(display->context.irq, display);
		display->context.irq = -1;
	}
	if (display->context.scal_irq >= 0) {
		free_irq(display->context.scal_irq, display);
		display->context.scal_irq = -1;
	}
	
#ifdef ARK_DISP_USE_PROC_DIR_ENTRY
	remove_proc_entry("display", NULL);
#endif
	misc_deregister(&display->ark_disp_miscdevice);
	kfree(display);
	return 0;
}

static struct platform_driver ark_disp_driver = {
	.driver     = {
		.name   = "ark-display",
	},
	.probe      = ark_disp_probe,
	.remove     = __devexit_p(ark_disp_remove),
};

static int __init ark_disp_init(void)
{
	int ret = 0;

	ret = platform_driver_register(&ark_disp_driver);
	if (ret) {
		printk(KERN_ERR "%s %d: failed to register ark_display_driver\n",
				__FUNCTION__, __LINE__);
	}

	return ret;
}
//module_init(ark_disp_init);
device_initcall(ark_disp_init);

static void __exit ark_disp_exit(void)
{
	platform_driver_unregister(&ark_disp_driver);
}
module_exit(ark_disp_exit);

EXPORT_SYMBOL(ark_disp_platform_device);
