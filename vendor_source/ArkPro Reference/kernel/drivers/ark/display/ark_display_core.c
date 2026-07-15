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
 *      ark_display_core.c
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
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/fb.h>
#include <linux/matroxfb.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <media/v4l2-dev.h>
#include <linux/videodev2.h>
#include <media/videobuf-core.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf-dma-contig.h>
#include <linux/miscdevice.h>
#include <linux/ark/ringpair.h>
#include <linux/ark/ark_display_io.h>
#include <linux/ark/ark_prescaler_io.h>
#include <linux/ark/ark_deinterlace_io.h>
#include <linux/ark/ark_itu656_io.h>
#include <mach/hardware.h>
#include <mach/va_map.h>

#include "ark_display.h"
#include "ark_display_priv.h"
#include "ark_display_fb.h"
#include "ark_lcd_regs.h"
#include "ark_sys_regs.h"

#define DISPLAY_NEEDED_INIT 0

struct videobuf_dma_contig_memory {
	u32 magic;
	void *vaddr;
	dma_addr_t dma_handle;
	unsigned long size;
};

int boot_complete = 0;

static struct ark_disp_context *core_disp_context;

//static struct ark_disp_context *arkdisp_tasklet_data = NULL;

static unsigned int frame_buf_addr_next_lcd[ARK_DISP_LAYER_NUM];
static unsigned int frame_buf_addr_next_tvenc[ARK_DISP_LAYER_NUM];
static unsigned int frame_buf_addr_next_tvout;
static unsigned int oddfield_buf_addr_next_tvout;
static unsigned int evenfield_buf_addr_next_tvout;

struct screen_info screens [] = {
	{SCREEN_QUN700, SCREEN_TYPE_RGB, LVDS_FORMAT_RGB0888, 800, 480, 40, 36, 16, 32, 32, 41, 0, 1, 1, 0, 0x8, 12},
	{SCREEN_CVBS_NTSC, SCREEN_TYPE_CVBS, CVBS_FORMAT_NTSC, 720, 480, 0, 0, 21, 0, 0, 135, 0, 0, 0, 0, 0x1F},
	{SCREEN_CVBS_PAL, SCREEN_TYPE_CVBS, CVBS_FORMAT_PAL, 720, 576, 0, 0, 23, 0, 0, 141, 0, 0, 0, 0, 0x1F},
	{SCREEN_VGA8060, SCREEN_TYPE_VGA, VGA_FORMAT_800x600, 800, 600, 23, 1, 3, 79, 39, 127, 0, 0, 0, 0, 0x1F},
	{SCREEN_TYPE_YPBPR720P, SCREEN_TYPE_YPBPR, YPBPR_FORMAT_720P60HZ, 1280, 720, 0, 0, 29, 0, 0, 369, 0, 0, 0, 0, 0x1F},
	{SCREEN_C101EAN, SCREEN_TYPE_LVDS, LVDS_FORMAT_RGB0888, 1280, 720, 14, 14, 10, 60, 60, 46, 0, 1, 1, 0, 0x8, 12},
	{SCREEN_CLAA101, SCREEN_TYPE_LVDS, LVDS_FORMAT_RGB0888, 1024, 600, 10, 10, 16, 100, 100, 120, 0, 1, 1, 0, 0x8, 12},
};

static unsigned int g_screen_id;

static int __init screen_id_setup (char *screen)
{
	g_screen_id = simple_strtoul(screen, NULL, 0);
}
__setup("screen=", screen_id_setup);

inline int is_two_screen_display(struct screen_info *screen)
{
	return screen->tvenc != 0 && screen->tvenc != ARK_DISP_LAYER_MASK;
}

int is_interlace_tvenc(struct screen_info *screen)
{
	if(screen->screen_type == SCREEN_TYPE_VGA)
		return 0;
	else if(screen->screen_type == SCREEN_TYPE_CVBS || 
		    screen->screen_type == SCREEN_TYPE_ITU656)
		return 1;
	else if(screen->screen_type == SCREEN_TYPE_YPBPR)
	{
		if(screen->format == 0 || screen->format == 1 || screen->format == 6 ||
		   screen->format == 7 || screen->format == 8)
			return 1;
		else 
			return 0;
	}

	return 0;
}

static int is_interlace_tvout(struct screen_info *screen)
{
	return TVENC_OUT_INTERLACE_MASK & (1 << screen->tvout_format);
}

int ark_disp_copy_layer_info_to_cfg_arg(int layer_id,
		struct ark_disp_layer_cfg_arg *arg)
{
	struct ark_disp_layer_info *layer;

	if (!core_disp_context) {
		ARKDISP_DBGPRTK("%s %d: null context error\n",
				__FUNCTION__, __LINE__);
		return -EFAULT;
	}

	if (layer_id >= ARK_DISP_LAYER_NUM) {
		ARKDISP_DBGPRTK("%s %d: invalid layer_id\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	layer = &core_disp_context->disp_obj[layer_id].layer_info;

	arg->layer_id               = layer_id;
	arg->enable                 = layer->enable;
	arg->interlaced             = layer->interlaced;
	arg->alpha_blend_en         = layer->alpha_blend_en;
	arg->per_pix_alpha_blend_en = layer->per_pix_alpha_blend_en;

	arg->type   = layer->type;
	arg->screen = layer->screen;
	arg->pixfmt = layer->pixfmt;

	arg->src_xres = layer->src_xres;
	arg->src_yres = layer->src_yres;
	arg->xres = layer->xres;
	arg->yres = layer->yres;
	arg->xpos = layer->xpos;
	arg->ypos = layer->ypos;

	arg->crop_xres = layer->crop_xres;
	arg->crop_yres = layer->crop_yres;
	arg->crop_xpos = layer->crop_xpos;
	arg->crop_ypos = layer->crop_ypos;

	arg->line_length = layer->line_length;
	arg->blend_mode = layer->blend_mode;
	arg->alpha = layer->alpha;

	arg->colorkey_enable = layer->colorkey_enable;
	arg->colorkey_y = layer->colorkey_y;
	arg->colorkey_cb = layer->colorkey_cb;
	arg->colorkey_cr = layer->colorkey_cr;
	arg->colorkey_y_thld = layer->colorkey_y_thld;
	arg->colorkey_cb_thld = layer->colorkey_cb_thld;
	arg->colorkey_cr_thld = layer->colorkey_cr_thld;

	arg->priority = layer->priority;

	arg->left_cut_num = layer->left_cut_num;
	arg->right_cut_num = layer->right_cut_num;
	arg->top_cut_num = layer->top_cut_num;
	arg->bottom_cut_num = layer->bottom_cut_num;

	arg->rgb_order = layer->rgb_order;
	arg->yuyv_order = layer->yuyv_order;

	return 0;
}


void ark_disp_set_next_buf_start_addr(int id, unsigned int addr_phys)
{
	struct ark_disp_obj_info *disp = &core_disp_context->disp_obj[id];
	unsigned int addr = addr_phys;

	ARKDISP_DBGPRTK("%s %d: id=%d, addr_phys=0x%08x\n",
			__FUNCTION__, __LINE__, id, addr);

	//    if (ARK_DISP_LAYER_IS_OSD(id)) {
	//        if (disp->layer_info.xres != disp->layer_info.crop_xres ||
	//            disp->layer_info.yres != disp->layer_info.crop_yres) {
	//            /* set prescaler to scale the addr frame to output buffer */
	//            if (disp->buf_info.prescal_disp_buf_ind)
	//                disp->buf_info.prescal_disp_buf_ind = 0;
	//            else
	//                disp->buf_info.prescal_disp_buf_ind = 1;
	//
	//#if defined(CONFIG_ARK_PRESCAL)
	//            ark_prescal_do_scale(
	//                addr_phys, //src_addr
	//                disp->buf_info.prescal_buf_addr_phys[
	//                    disp->buf_info.prescal_disp_buf_ind], //dst_addr
	//                disp->layer_info.crop_xres, // src width
	//                disp->layer_info.crop_yres, // src height
	//                disp->layer_info.xres, // dst width
	//                disp->layer_info.yres, // dst height
	//                ARKPRESCAL_FORMAT_RGB, // format
	//                ARKPRESCAL_WORKMODE_MIX // work mode
	//                ); // do scaling
	//#endif
	//            ARKDISP_DBGPRTK("%s %d: prescale done (out_addr_phys=0x%08x)\n",
	//                __FUNCTION__, __LINE__,
	//                disp->buf_info.prescal_buf_addr_phys[
	//                    disp->buf_info.prescal_disp_buf_ind]);
	//
	//            /* set phys_addr to prescaler output addr */
	//            addr = disp->buf_info.prescal_buf_addr_phys[
	//                    disp->buf_info.prescal_disp_buf_ind];
	//        }
	//    }

	disp->buf_info.cur_buf_addr_phys = addr;
	if (addr >= disp->buf_info.buf_pool_base_phys &&
			addr < disp->buf_info.buf_pool_base_phys
			+ disp->buf_info.buf_pool_size) {
		/* this is a frame buffer memory */
		disp->buf_info.cur_buf_addr_virt =
			addr - disp->buf_info.buf_pool_base_phys
			+ disp->buf_info.buf_pool_base_virt;
	} else {
		disp->buf_info.cur_buf_addr_virt =
			addr - disp->buf_info.buf_pool_base_phys
			+ disp->buf_info.buf_pool_base_virt;
	}

	ARKDISP_DBGPRTK("%s %d: id=%d, cur_buf_addr_virt=0x%08x\n",
			__FUNCTION__, __LINE__, id, disp->buf_info.cur_buf_addr_virt);

	if (disp->layer_info.screen == ARKDISP_SCREEN_LCD) {
		frame_buf_addr_next_lcd[id] = addr;
		ARKDISP_DBGPRTK("%s %d: frame_buf_addr_next_lcd[%d]=0x%08x\n",
				__FUNCTION__, __LINE__, id, addr);
	} else {
		frame_buf_addr_next_tvenc[id] = addr;
		ARKDISP_DBGPRTK("%s %d: frame_buf_addr_next_tvenc[%d]=0x%08x\n",
				__FUNCTION__, __LINE__, id, addr);
	}
}

void ark_disp_set_tvout_next_oddfield_bufaddr(unsigned int addr)
{
	oddfield_buf_addr_next_tvout = addr;
}
EXPORT_SYMBOL(ark_disp_set_tvout_next_oddfield_bufaddr);

void ark_disp_set_tvout_next_evenfield_bufaddr(unsigned int addr)
{
	evenfield_buf_addr_next_tvout = addr;
}
EXPORT_SYMBOL(ark_disp_set_tvout_next_evenfield_bufaddr);

int ark_disp_get_vde_cfg(struct ark_disp_vde_cfg_arg *arg)
{
	struct ark_disp_layer_info *layer;

	ARKDISP_DBGPRTK("%s %d: layer_id=%d\n",
			__FUNCTION__, __LINE__, arg->layer_id);

	if (!core_disp_context) {
		ARKDISP_DBGPRTK("%s %d: null context error\n",
				__FUNCTION__, __LINE__);
		return -EFAULT;
	}

	if (arg->layer_id >= ARK_DISP_LAYER_NUM) {
		ARKDISP_DBGPRTK("%s %d: invalid layer_id\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	layer = &core_disp_context->disp_obj[arg->layer_id].layer_info;

	arg->hue        = layer->hue;
	arg->saturation = layer->saturation;
	arg->brightness = layer->brightness;
	arg->contrast   = layer->contrast;
	return 0;
}

int ark_disp_set_vde_cfg(const struct ark_disp_vde_cfg_arg *arg)
{
	struct ark_disp_layer_info *layer;
	int id;

	ARKDISP_DBGPRTK("%s %d: layer_id=%d\n",
			__FUNCTION__, __LINE__, arg->layer_id);

	if (!core_disp_context) {
		ARKDISP_DBGPRTK("%s %d: null context error\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	if (arg->layer_id >= ARK_DISP_LAYER_NUM) {
		ARKDISP_DBGPRTK("%s %d: invalid layer_id\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	layer = &core_disp_context->disp_obj[arg->layer_id].layer_info;

	if (layer->hue != arg->hue) {
		layer->hue = arg->hue;

		if (ARK_DISP_LAYER_IS_OSD(arg->layer_id)) {
			id = ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(arg->layer_id);
			ark_disp_set_osd_hue(id, arg->hue);
			ARKDISP_DBGPRTK("%s %d: osd_id=%d hue=0x%02x\n",
					__FUNCTION__, __LINE__, id, arg->hue);
		} else {
			id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(arg->layer_id);
			ark_disp_set_video_hue(id, arg->hue);
			ARKDISP_DBGPRTK("%s %d: vid_id=%d hue=0x%02x\n",
					__FUNCTION__, __LINE__, id, arg->hue);
		}
	}
	if (layer->saturation != arg->saturation) {
		layer->saturation = arg->saturation;

		if (ARK_DISP_LAYER_IS_OSD(arg->layer_id)) {
			id = ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(arg->layer_id);
			ark_disp_set_osd_saturation(id, arg->saturation);
			ARKDISP_DBGPRTK("%s %d: osd_id=%d sat=0x%02x\n",
					__FUNCTION__, __LINE__, id, arg->saturation);
		} else {
			id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(arg->layer_id);
			ark_disp_set_video_saturation(id, arg->saturation);
			ARKDISP_DBGPRTK("%s %d: vid_id=%d sat=0x%02x\n",
					__FUNCTION__, __LINE__, id, arg->saturation);
		}
	}
	if (layer->brightness != arg->brightness) {
		layer->brightness = arg->brightness;

		if (ARK_DISP_LAYER_IS_OSD(arg->layer_id)) {
			id = ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(arg->layer_id);
			ark_disp_set_osd_brightness(id, arg->brightness);
			ARKDISP_DBGPRTK("%s %d: osd_id=%d bri=0x%02x\n",
					__FUNCTION__, __LINE__, id, arg->brightness);
		} else {
			id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(arg->layer_id);
			ark_disp_set_video_brightness(id, arg->brightness);
			ARKDISP_DBGPRTK("%s %d: vid_id=%d bri=0x%02x\n",
					__FUNCTION__, __LINE__, id, arg->brightness);
		}
	}
	if (layer->contrast != arg->contrast) {
		layer->contrast = arg->contrast;

		if (ARK_DISP_LAYER_IS_OSD(arg->layer_id)) {
			id = ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(arg->layer_id);
			ark_disp_set_osd_contrast(id, arg->contrast);
			ARKDISP_DBGPRTK("%s %d: osd_id=%d con=0x%02x\n",
					__FUNCTION__, __LINE__, id, arg->contrast);
		} else {
			id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(arg->layer_id);
			ark_disp_set_video_contrast(id, arg->contrast);
			ARKDISP_DBGPRTK("%s %d: vid_id=%d con=0x%02x\n",
					__FUNCTION__, __LINE__, id, arg->contrast);
		}
	}
	return 0;
}

int ark_disp_get_layer_cfg(struct ark_disp_layer_cfg_arg *arg)
{
	struct ark_disp_layer_info *layer;

	ARKDISP_DBGPRTK("%s %d: layer_id=%d\n",
			__FUNCTION__, __LINE__, arg->layer_id);

	if (!core_disp_context) {
		ARKDISP_DBGPRTK("%s %d: null context error\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	if (arg->layer_id >= ARK_DISP_LAYER_NUM) {
		ARKDISP_DBGPRTK("%s %d: invalid layer_id\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	layer = &core_disp_context->disp_obj[arg->layer_id].layer_info;

	arg->enable           = layer->enable;
	arg->interlaced       = layer->interlaced;
	arg->alpha_blend_en   = layer->alpha_blend_en;
	arg->per_pix_alpha_blend_en = layer->per_pix_alpha_blend_en;    
	arg->type             = layer->type;
	arg->screen           = layer->screen;
	arg->pixfmt           = layer->pixfmt;
	arg->src_xres         = layer->src_xres;
	arg->src_yres         = layer->src_yres;
	arg->xres             = layer->xres;
	arg->yres             = layer->yres;
	arg->xpos             = layer->xpos;
	arg->ypos             = layer->ypos;
	arg->crop_xres        = layer->crop_xres;
	arg->crop_yres        = layer->crop_yres;
	arg->crop_xpos        = layer->crop_xpos;
	arg->crop_ypos        = layer->crop_ypos;
	arg->line_length      = layer->line_length;
	arg->blend_mode       = layer->blend_mode;
	arg->alpha            = layer->alpha;

	arg->colorkey_enable  = layer->colorkey_enable;
	arg->colorkey_y       = layer->colorkey_y;
	arg->colorkey_cb      = layer->colorkey_cb;
	arg->colorkey_cr      = layer->colorkey_cr;
	arg->colorkey_y_thld  = layer->colorkey_y_thld;
	arg->colorkey_cb_thld = layer->colorkey_cb_thld;
	arg->colorkey_cr_thld = layer->colorkey_cr_thld;

	arg->priority        = layer->priority;

	arg->left_cut_num    = layer->left_cut_num;
	arg->right_cut_num   = layer->right_cut_num;
	arg->top_cut_num     = layer->top_cut_num;
	arg->bottom_cut_num  = layer->bottom_cut_num;

	arg->rgb_order       = layer->rgb_order;
	arg->yuyv_order      = layer->yuyv_order;

	return 0;
}

int ark_disp_try_layer_cfg(struct ark_disp_layer_cfg_arg *arg)
{
	int id, pixfmt;
	struct ark_disp_layer_info *layer;

	ARKDISP_DBGPRTK("%s %d: layer_id=%d\n",
			__FUNCTION__, __LINE__, arg->layer_id);

	if (arg->layer_id >= ARK_DISP_LAYER_NUM) {
		ARKDISP_DBGPRTK("%s %d: invalid layer_id\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	if (core_disp_context == NULL) {
		ARKDISP_DBGPRTK("%s %d: null context pointer\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	layer = &core_disp_context->disp_obj[arg->layer_id].layer_info;

	if (arg->type != layer->type) {
		ARKDISP_DBGPRTK("%s %d: type not equal (input=%d rec=%d)\n",
				__FUNCTION__, __LINE__, arg->type, layer->type);
		return -EINVAL;
	}

	if (!arg->src_xres || !arg->src_yres) {
		ARKDISP_DBGPRTK("%s %d: null out res (xres=%d, yres=%d)\n",
				__FUNCTION__, __LINE__, arg->xres, arg->yres);
		return -EINVAL;
	}

	if (!arg->xres || !arg->yres) {
		ARKDISP_DBGPRTK("%s %d: null out res (xres=%d, yres=%d)\n",
				__FUNCTION__, __LINE__, arg->xres, arg->yres);
		return -EINVAL;
	}

	if (arg->left_cut_num + arg->right_cut_num > arg->src_xres) {
		ARKDISP_DBGPRTK("%s %d: invalid left/right cut num "
				"(left_cut_num=%d, right_cut_num=%d, xres=%d)\n",
				__FUNCTION__, __LINE__,
				arg->left_cut_num, arg->right_cut_num, arg->xres
			       );
		return -EINVAL;
	}

	if (arg->top_cut_num + arg->bottom_cut_num > arg->src_yres) {
		ARKDISP_DBGPRTK("%s %d: invalid left/right cut num "
				"(top_cut_num=%d, bottom_cut_num=%d, yres=%d)\n",
				__FUNCTION__, __LINE__,
				arg->top_cut_num, arg->bottom_cut_num, arg->yres
			       );
		return -EINVAL;
	}

	if (ARK_DISP_LAYER_IS_OSD(arg->layer_id)) {
		id = ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(arg->layer_id);
		/* check pixel format */
		switch (arg->pixfmt){
			case ARKDISP_PIXFMT_PALETTE:
				pixfmt = ARK_DISP_OSD_PIXFMT_PALETTE;
				break;
			case ARKDISP_PIXFMT_BMP24BIT:
				pixfmt = ARK_DISP_OSD_PIXFMT_BMP24BIT;
				break;
			case ARKDISP_PIXFMT_YUYV:
				pixfmt = ARK_DISP_OSD_PIXFMT_YUYV;
				break;
			case ARKDISP_PIXFMT_YUV:
				pixfmt = ARK_DISP_OSD_PIXFMT_YUV;
				break;
			case ARKDISP_PIXFMT_RGBI555:
				pixfmt = ARK_DISP_OSD_PIXFMT_RGBI555;
				break;
			case ARKDISP_PIXFMT_RGB565:
				pixfmt = ARK_DISP_OSD_PIXFMT_RGB565;
				break;
			case ARKDISP_PIXFMT_RGBA888:
				pixfmt = ARK_DISP_OSD_PIXFMT_RGBA888;
				break;
			case ARKDISP_PIXFMT_RGB888:
				pixfmt = ARK_DISP_OSD_PIXFMT_RGB888;
				break;
			case ARKDISP_PIXFMT_RGBA1555:
				pixfmt = ARK_DISP_OSD_PIXFMT_RGBA1555;
				break;
			case ARKDISP_PIXFMT_RGBA1888:
				pixfmt = ARK_DISP_OSD_PIXFMT_RGBA1888;
				break;
			case ARKDISP_PIXFMT_RGBA4888:
				pixfmt = ARK_DISP_OSD_PIXFMT_RGBA4888;
				break;
			case ARKDISP_PIXFMT_RGB666:
				pixfmt = ARK_DISP_OSD_PIXFMT_RGB666;
				break;
			case ARKDISP_PIXFMT_RGBA1666:
				pixfmt = ARK_DISP_OSD_PIXFMT_RGBA1666;
				break;
			default:
				ARKDISP_DBGPRTK("%s %d: invalid pixfmt input (%d)\n",
						__FUNCTION__, __LINE__, arg->pixfmt);
				return -EINVAL;
				break;
		}

		/* check and adjust crop parameters */
		if (arg->xres > arg->src_xres)
			arg->xres = arg->src_xres;
		if (arg->yres > arg->src_yres)
			arg->yres = arg->src_yres;
		if (arg->xres + arg->crop_xpos > arg->src_xres)
			arg->crop_xpos = arg->src_xres - arg->xres;
		if (arg->yres + arg->crop_ypos > arg->src_yres)
			arg->crop_ypos = arg->src_yres - arg->yres;
		arg->crop_xres = arg->xres;
		arg->crop_yres = arg->yres;
	} else {
		id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(arg->layer_id);
		/* check pixel format */
		switch (arg->pixfmt){
			case ARKDISP_PIXFMT_YUV422:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_YUV422;
				break;
			case ARKDISP_PIXFMT_YUV420:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_YUV420;
				break;
			case ARKDISP_PIXFMT_YUYV:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_YUYV;
				break;
			case ARKDISP_PIXFMT_YUV:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_YUV;
				break;
			case ARKDISP_PIXFMT_RGBI555:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_RGBI555;
				break;
			case ARKDISP_PIXFMT_RGB565:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_RGB565;
				break;
			case ARKDISP_PIXFMT_RGBA888:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_RGBA888;
				break;
			case ARKDISP_PIXFMT_RGB888:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_RGB888;
				break;
			case ARKDISP_PIXFMT_RGBA1555:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_RGBA1555;
				break;
			case ARKDISP_PIXFMT_RGBA1888:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_RGBA1888;
				break;
			case ARKDISP_PIXFMT_RGBA4888:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_RGBA4888;
				break;
			case ARKDISP_PIXFMT_RGB666:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_RGB666;
				break;
			case ARKDISP_PIXFMT_RGBA1666:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_RGBA1666;
				break;
			case ARKDISP_PIXFMT_Y_UV422:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_Y_UV422;
				break;
			case ARKDISP_PIXFMT_Y_UV420:
				pixfmt = ARK_DISP_VIDEO_PIXFMT_Y_UV420;
				break;
			default:
				ARKDISP_DBGPRTK("%s %d: invalid pixfmt input (%d)\n",
						__FUNCTION__, __LINE__, arg->pixfmt);
				return -EINVAL;
				break;
		}

		if (arg->pixfmt == ARK_DISP_VIDEO_PIXFMT_YUV420 ||
				arg->pixfmt == ARK_DISP_VIDEO_PIXFMT_Y_UV420) {
			if (arg->src_xres & 7) {
				ARKDISP_DBGPRTK("%s %d: src_xres not divisible by 8 (%d)\n",
						__FUNCTION__, __LINE__, arg->src_xres);
				return -EINVAL;
			}
		}

		/* check and adjust crop parameters */
		if (arg->crop_xres > arg->src_xres)
			arg->crop_xres = arg->src_xres;
		if (arg->crop_yres > arg->src_yres)
			arg->crop_yres = arg->src_yres;
		if (arg->crop_xres + arg->crop_xpos > arg->src_xres)
			arg->crop_xpos = arg->src_xres - arg->crop_xres;
		if (arg->crop_yres + arg->crop_ypos > arg->src_yres)
			arg->crop_ypos = arg->src_yres - arg->crop_yres;
	}
	return 0;
}

int ark_disp_set_layer_en(int layer_id, int enable)
{
	struct ark_disp_layer_info *layer;
	int id;
	if (!core_disp_context) {
		ARKDISP_DBGPRTK("%s %d: null context error\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	if (layer_id >= ARK_DISP_LAYER_NUM) {
		ARKDISP_DBGPRTK("%s %d: invalid layer_id\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	layer = &core_disp_context->disp_obj[layer_id].layer_info;

	layer->enable = enable;

	if (ARK_DISP_LAYER_IS_OSD(layer_id)) {
		id = ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(layer_id);

		if (layer->enable) {
			if (layer->screen == ARKDISP_SCREEN_LCD) {
				// not select frame mode for lcd output
				ark_disp_set_osd_frame_mode(id, 0);

				/* set osd_sel = 0, i.e. osd clock equals lcd_clk */
				ark_disp_set_osd_clk_sel(id, 0);

				ark_disp_set_osd_en_lcd(id, 1);
				ark_disp_set_osd_en_tvenc(id, 0);
			} else {
				// select frame mode for cvbs, ypbpr, itu656out
				// (not for vga???)
				ark_disp_set_osd_frame_mode(
						id, core_disp_context->tvenc_cfg.layer_frame_mode);

				/* set osd_sel = 1, i.e. osd clock equals tv_pixel_clk */
				ark_disp_set_osd_clk_sel(id, 1);

				ark_disp_set_osd_en_lcd(id, 0);
				ark_disp_set_osd_en_tvenc(id, 1);
			}
		} else {
			ark_disp_set_osd_en_lcd(id, 0);
			ark_disp_set_osd_en_tvenc(id, 0);
		}
	} else {
		id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(layer_id);

		if (layer->enable) {
			if (layer->screen == ARKDISP_SCREEN_LCD) {
				/* not select frame mode for lcd out */
				//ark_disp_set_video_frame_mode(id, 0);

				/* set vid_sel = 0, i.e. vid clock equals lcd_clk */
				ark_disp_set_video_clk_sel(id, 0);

				/*ark_disp_set_video_scal(id,
						layer->crop_xres, layer->crop_yres,
						layer->left_cut_num, layer->right_cut_num, 
						layer->top_cut_num, layer->bottom_cut_num, 
						layer->xres, layer->yres,
						0, // interlace_out_en=0, enable progressive data out
						0  // data read in progressive way
						);*/

				ark_disp_set_video_en_lcd(id, 1);
				ark_disp_set_video_en_tvenc(id, 0);
			} else {
				/* select frame mode for cvbs, ypbpr, itu656out */
				/*ark_disp_set_video_frame_mode(
						id, core_disp_context->tvenc_cfg.layer_frame_mode);*/

				/* set vid_sel = 1, i.e. vid clock equals tv_pixel_clk */
				ark_disp_set_video_clk_sel(id, 1);

				/*ark_disp_set_video_scal(id,
						layer->crop_xres, layer->crop_yres,
						layer->left_cut_num, layer->right_cut_num, 
						layer->top_cut_num, layer->bottom_cut_num, 
						layer->xres, layer->yres,
						1, // interlace_out_en=1, enable interlace data out
						core_disp_context->tvenc_cfg.layer_frame_mode // read mode
						);*/

				ark_disp_set_video_en_lcd(id, 0);
				ark_disp_set_video_en_tvenc(id, 1);
			}
		} else {
			ark_disp_set_video_en_lcd(id, 0);
			ark_disp_set_video_en_tvenc(id, 0);
		}
	}
	return 0;
}
EXPORT_SYMBOL(ark_disp_set_layer_en);

/* NOTE: it is better to run ark_disp_try_layer_cfg() to adjust the input
 * arg before passing it to this function
 */
int ark_disp_set_layer_cfg(const struct ark_disp_layer_cfg_arg *arg)
{
	struct ark_disp_context *context = core_disp_context;
	struct ark_disp_layer_info *layer;
	struct ark_disp_buf_info *buf;
	int id;
	int format, bits_per_pixel;
	ARKDISP_DBGPRTK("%s %d: layer_id=%d\n",
			__FUNCTION__, __LINE__, arg->layer_id);
	if (!context) {
		ARKDISP_DBGPRTK("%s %d: null context error\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	if (arg->layer_id >= ARK_DISP_LAYER_NUM) {
		ARKDISP_DBGPRTK("%s %d: invalid layer_id\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	layer = &context->disp_obj[arg->layer_id].layer_info;
	buf = &context->disp_obj[arg->layer_id].buf_info;

	/* NOTE: the same layer cannot be enabled on both LCD screen and 
	 * TVEnc screen simultaneously
	 */

	if (arg->type != layer->type) {
		ARKDISP_DBGPRTK("%s %d: type not equal (input=%d rec=%d)\n",
				__FUNCTION__, __LINE__, arg->type, layer->type);
		return -EINVAL;
	}

	if (ARK_DISP_LAYER_IS_OSD(arg->layer_id)) {
		id = ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(arg->layer_id);

		switch (arg->pixfmt){
			case ARKDISP_PIXFMT_PALETTE:
				format = ARK_DISP_OSD_PIXFMT_PALETTE;
				bits_per_pixel = 8;
				break;
			case ARKDISP_PIXFMT_BMP24BIT:
				format = ARK_DISP_OSD_PIXFMT_BMP24BIT;
				bits_per_pixel = 24;
				break;
			case ARKDISP_PIXFMT_YUYV:
				format = ARK_DISP_OSD_PIXFMT_YUYV;
				bits_per_pixel = 16;
				break;
			case ARKDISP_PIXFMT_YUV:
				format = ARK_DISP_OSD_PIXFMT_YUV;
				bits_per_pixel = 24;
				break;
			case ARKDISP_PIXFMT_RGBI555:
				format = ARK_DISP_OSD_PIXFMT_RGBI555;
				bits_per_pixel = 16;
				break;
			case ARKDISP_PIXFMT_RGB565:
				format = ARK_DISP_OSD_PIXFMT_RGB565;
				bits_per_pixel = 16;
				break;
			case ARKDISP_PIXFMT_RGBA888:
				format = ARK_DISP_OSD_PIXFMT_RGBA888;
				bits_per_pixel = 32;
				break;
			case ARKDISP_PIXFMT_RGB888:
				format = ARK_DISP_OSD_PIXFMT_RGB888;
				bits_per_pixel = 24;
				break;
			case ARKDISP_PIXFMT_RGBA1555:
				format = ARK_DISP_OSD_PIXFMT_RGBA1555;
				bits_per_pixel = 16;
				break;
			case ARKDISP_PIXFMT_RGBA1888:
				format = ARK_DISP_OSD_PIXFMT_RGBA1888;
				bits_per_pixel = 25;
				break;
			case ARKDISP_PIXFMT_RGBA4888:
				format = ARK_DISP_OSD_PIXFMT_RGBA4888;
				bits_per_pixel = 28;
				break;
			case ARKDISP_PIXFMT_RGB666:
				format = ARK_DISP_OSD_PIXFMT_RGB666;
				bits_per_pixel = 18;
				break;
			case ARKDISP_PIXFMT_RGBA1666:
				format = ARK_DISP_OSD_PIXFMT_RGBA1666;
				bits_per_pixel = 19;
				break;
			default:
				ARKDISP_DBGPRTK(
						"%s %d: invalid pixfmt input (%d)\n",
						__FUNCTION__, __LINE__, arg->pixfmt);
				return -EINVAL;
				break;
		}

		/* update control structures */
		layer->pixfmt = arg->pixfmt;
		layer->enable = arg->enable;
		layer->screen = arg->screen;
		layer->src_xres = arg->src_xres;
		layer->src_yres = arg->src_yres;
		layer->xres = arg->xres;
		layer->yres = arg->yres;
		layer->xpos = arg->xpos;
		layer->ypos = arg->ypos;
		layer->crop_xres = arg->crop_xres;
		layer->crop_yres = arg->crop_yres;
		layer->crop_xpos = arg->crop_xpos;
		layer->crop_ypos = arg->crop_ypos;
		layer->alpha = arg->alpha;
		layer->alpha_blend_en = arg->alpha_blend_en;
		layer->per_pix_alpha_blend_en = arg->per_pix_alpha_blend_en;
		layer->blend_mode = arg->blend_mode;
		layer->colorkey_enable = arg->colorkey_enable;
		layer->colorkey_y = arg->colorkey_y;
		layer->colorkey_cb = arg->colorkey_cb;
		layer->colorkey_cr = arg->colorkey_cr;
		layer->colorkey_y_thld = arg->colorkey_y_thld;
		layer->colorkey_cb_thld = arg->colorkey_cb_thld;
		layer->colorkey_cr_thld = arg->colorkey_cr_thld;
		layer->priority = arg->priority;
		layer->left_cut_num = arg->left_cut_num;
		layer->right_cut_num = arg->right_cut_num;
		layer->top_cut_num = arg->top_cut_num;
		layer->bottom_cut_num = arg->bottom_cut_num;
		layer->rgb_order  = arg->rgb_order;
		layer->yuyv_order = arg->yuyv_order;
		layer->interlaced = arg->interlaced;

		// hw related info
		layer->format = format;
		layer->bits_per_pixel = bits_per_pixel;
		layer->line_length = layer->src_xres * layer->bits_per_pixel / 8;

		/* set osd control registers */

		/* NOTE: always set format first */

		//        if (layer->xres != layer->src_xres ||
		//            layer->yres != layer->src_yres) {
		//            buf->prescal_disp_buf_ind = 0;
		//
		//#if defined(CONFIG_ARK_PRESCAL)
		//            /* set prescaler to output video to the output buffer */
		//            ark_prescal_do_scale(
		//                buf->cur_buf_addr_phys
		//                + layer->crop_xpos * layer->bits_per_pixel / 8
		//                + layer->crop_ypos * layer->src_xres, //src_addr
		//                buf->prescal_buf_addr_phys[buf->prescal_disp_buf_ind], //dst_addr
		//                layer->crop_xres, // src width
		//                layer->crop_yres, // src height
		//                layer->xres, // dst width
		//                layer->yres, // dst height
		//                ARKPRESCAL_FORMAT_RGB, // format
		//                ARKPRESCAL_WORKMODE_MIX // work mode
		//                ); // do scaling
		//#endif
		//            ARKDISP_DBGPRTK("%s %d: prescale done (out_addr_phys=0x%08x)\n",
		//                __FUNCTION__, __LINE__,
		//                buf->prescal_buf_addr_phys[buf->prescal_disp_buf_ind]);
		//        }

		/* NOTE: always set source resolution equal to output resolution */
		ark_disp_set_osd_source_size(
				id, layer->xres, layer->yres); 
		//            id, layer->src_xres, layer->src_yres); 

		ark_disp_set_osd_layer_size(
				id, layer->xres, layer->yres);

		ark_disp_set_osd_layer_position(
				id, layer->xpos, layer->ypos);

		ark_disp_set_osd_layer_size(
				id, layer->crop_xres, layer->crop_yres);

		ark_disp_set_osd_win_point(
				id, layer->crop_xpos, layer->crop_ypos);

		ark_disp_set_osd_alpha(id, layer->alpha);

		if (layer->alpha_blend_en) {
			if (layer->screen == ARKDISP_SCREEN_LCD) {
				ark_disp_set_osd_alpha_blend_en_lcd(id, 1);
				ark_disp_set_osd_alpha_blend_en_tvenc(id, 0);
			} else {
				ark_disp_set_osd_alpha_blend_en_lcd(id, 0);
				ark_disp_set_osd_alpha_blend_en_tvenc(id, 1);
			}
		} else {
			ark_disp_set_osd_alpha_blend_en_lcd(id, 0);
			ark_disp_set_osd_alpha_blend_en_tvenc(id, 0);
		}

		ark_disp_set_osd_per_pix_alpha_blend_en_lcd(  //init in uboot, for display logo 
				id, layer->per_pix_alpha_blend_en);

		ark_disp_set_osd_per_pix_alpha_blend_en_tvenc(
				id, layer->per_pix_alpha_blend_en);
		ark_disp_set_osd_blend_mode_lcd(id, layer->blend_mode);

		ark_disp_set_osd_blend_mode_tvenc(id, layer->blend_mode);

		ark_disp_set_osd_colorkey(id, layer->colorkey_enable,
				layer->colorkey_y, layer->colorkey_cb, layer->colorkey_cr);

		ark_disp_set_osd_colorkey_thld(id, layer->colorkey_y_thld,
				layer->colorkey_cb_thld, layer->colorkey_cr_thld);

		ark_disp_set_osd_priority_lcd(id, layer->priority);

		ark_disp_set_osd_priority_tvenc(id, layer->priority);

		ark_disp_set_osd_layer_cut(id,
				layer->left_cut_num, layer->right_cut_num,
				layer->top_cut_num, layer->bottom_cut_num
				);
	} else {
		id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(arg->layer_id);

		switch (arg->pixfmt){
			case ARKDISP_PIXFMT_YUV422:
				format = ARK_DISP_VIDEO_PIXFMT_YUV422;
				bits_per_pixel = 16;
				break;
			case ARKDISP_PIXFMT_YUV420:
				format = ARK_DISP_VIDEO_PIXFMT_YUV420;
				bits_per_pixel = 12;
				break;
			case ARKDISP_PIXFMT_YUYV:
				format = ARK_DISP_VIDEO_PIXFMT_YUYV;
				bits_per_pixel = 16;
				break;
			case ARKDISP_PIXFMT_YUV:
				format = ARK_DISP_VIDEO_PIXFMT_YUV;
				bits_per_pixel = 24;
				break;
			case ARKDISP_PIXFMT_RGBI555:
				format = ARK_DISP_VIDEO_PIXFMT_RGBI555;
				bits_per_pixel = 16;
				break;
			case ARKDISP_PIXFMT_RGB565:
				format = ARK_DISP_VIDEO_PIXFMT_RGB565;
				bits_per_pixel = 16;
				break;
			case ARKDISP_PIXFMT_RGBA888:
				format = ARK_DISP_VIDEO_PIXFMT_RGBA888;
				bits_per_pixel = 32;
				break;
			case ARKDISP_PIXFMT_RGB888:
				format = ARK_DISP_VIDEO_PIXFMT_RGB888;
				bits_per_pixel = 24;
				break;
			case ARKDISP_PIXFMT_RGBA1555:
				format = ARK_DISP_VIDEO_PIXFMT_RGBA1555;
				bits_per_pixel = 16;
				break;
			case ARKDISP_PIXFMT_RGBA1888:
				format = ARK_DISP_VIDEO_PIXFMT_RGBA1888;
				bits_per_pixel = 25;
				break;
			case ARKDISP_PIXFMT_RGBA4888:
				format = ARK_DISP_VIDEO_PIXFMT_RGBA4888;
				bits_per_pixel = 28;
				break;
			case ARKDISP_PIXFMT_RGB666:
				format = ARK_DISP_VIDEO_PIXFMT_RGB666;
				bits_per_pixel = 18;
				break;
			case ARKDISP_PIXFMT_RGBA1666:
				format = ARK_DISP_VIDEO_PIXFMT_RGBA1666;
				bits_per_pixel = 19;
				break;
			case ARKDISP_PIXFMT_Y_UV422:
				format = ARK_DISP_VIDEO_PIXFMT_Y_UV422;
				bits_per_pixel = 16;
				break;
			case ARKDISP_PIXFMT_Y_UV420:
				format = ARK_DISP_VIDEO_PIXFMT_Y_UV420;
				bits_per_pixel = 12;
				break;
			default:
				ARKDISP_DBGPRTK(
						"%s %d: invalid pixfmt input (%d)\n",
						__FUNCTION__, __LINE__, arg->pixfmt);
				return -EINVAL;
				break;
		}

		/* update control structures */
		layer->pixfmt = arg->pixfmt;
		layer->enable = arg->enable;
		layer->screen = arg->screen;
		layer->src_xres = arg->src_xres;
		layer->src_yres = arg->src_yres;
		layer->xres = arg->xres;
		layer->yres = arg->yres;
		layer->xpos = arg->xpos;
		layer->ypos = arg->ypos;
		layer->crop_xres = arg->crop_xres;
		layer->crop_yres = arg->crop_yres;
		layer->crop_xpos = arg->crop_xpos;
		layer->crop_ypos = arg->crop_ypos;
		layer->alpha = arg->alpha;
		layer->alpha_blend_en = arg->alpha_blend_en;
		layer->per_pix_alpha_blend_en = arg->per_pix_alpha_blend_en;
		layer->blend_mode = arg->blend_mode;
		layer->colorkey_enable = arg->colorkey_enable;
		layer->colorkey_y = arg->colorkey_y;
		layer->colorkey_cb = arg->colorkey_cb;
		layer->colorkey_cr = arg->colorkey_cr;
		layer->colorkey_y_thld = arg->colorkey_y_thld;
		layer->colorkey_cb_thld = arg->colorkey_cb_thld;
		layer->colorkey_cr_thld = arg->colorkey_cr_thld;
		layer->priority = arg->priority;
		layer->left_cut_num = arg->left_cut_num;
		layer->right_cut_num = arg->right_cut_num;
		layer->top_cut_num = arg->top_cut_num;
		layer->bottom_cut_num = arg->bottom_cut_num;
		layer->rgb_order  = arg->rgb_order;
		layer->yuyv_order = arg->yuyv_order;
		layer->interlaced = arg->interlaced;

		// hw related info
		layer->format = format;
		layer->bits_per_pixel = bits_per_pixel;
		if (layer->bits_per_pixel == 12)
			layer->line_length = layer->src_xres;
		else
			layer->line_length = layer->src_xres * layer->bits_per_pixel / 8;

		/* set video control registers */
		ark_disp_set_video_format(id, format, layer->rgb_order, layer->yuyv_order, 0);

		/* NOTE: always set format first */

		ark_disp_set_video_source_size(
				id, layer->src_xres, layer->src_yres);

		ark_disp_set_video_layer_size(
				id, layer->xres, layer->yres);

		ark_disp_set_video_layer_position(
				id, layer->xpos, layer->ypos);

		ark_disp_set_video_win_size(
				id, layer->crop_xres, layer->crop_yres);

		ark_disp_set_video_win_point(
				id, layer->crop_xpos, layer->crop_ypos);

		ark_disp_set_video_alpha(id, layer->alpha);

		if (layer->alpha_blend_en) {
			if (layer->screen == ARKDISP_SCREEN_LCD) {
				ark_disp_set_video_alpha_blend_en_lcd(id, 1);
				ark_disp_set_video_alpha_blend_en_tvenc(id, 0);
			} else {
				ark_disp_set_video_alpha_blend_en_lcd(id, 0);
				ark_disp_set_video_alpha_blend_en_tvenc(id, 1);
			}
		} else {
			ark_disp_set_video_alpha_blend_en_lcd(id, 0);
			ark_disp_set_video_alpha_blend_en_tvenc(id, 0);
		}

		ark_disp_set_video_per_pix_alpha_blend_en_lcd(
				id, layer->per_pix_alpha_blend_en);
		ark_disp_set_video_per_pix_alpha_blend_en_tvenc(
				id, layer->per_pix_alpha_blend_en);

		ark_disp_set_video_blend_mode_lcd(id, layer->blend_mode);
		ark_disp_set_video_blend_mode_tvenc(id, layer->blend_mode);

		ark_disp_set_video_colorkey(id, layer->colorkey_enable,
				layer->colorkey_y, layer->colorkey_cb, layer->colorkey_cr);

		ark_disp_set_video_colorkey_thld(id, layer->colorkey_y_thld,
				layer->colorkey_cb_thld, layer->colorkey_cr_thld);

		ark_disp_set_video_priority_lcd(id, layer->priority); //move other place

		ark_disp_set_video_priority_tvenc(id, layer->priority);
		ark_disp_set_video_layer_cut(id,
				layer->left_cut_num, layer->right_cut_num,
				layer->top_cut_num, layer->bottom_cut_num
				);
	}

	/* set layer enable in the end for preventing displaying any unexpected
	 * image on the screen
	 */
	ark_disp_set_layer_en(arg->layer_id, arg->enable);

	/* sync up fb info */
	ark_disp_sync_other_cfg_to_fb_info(arg->layer_id, core_disp_context);

	/* sync up v4l2 info */
	//ark_disp_convert_layer_cfg_to_v4l2_info(arg->layer_id, core_disp_context);

	return 0;
}

int ark_disp_get_lcd_cfg(struct ark_disp_lcd_cfg_arg *arg)
{
	struct ark_disp_lcd_cfg_info *lcd;

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	if (!core_disp_context) {
		ARKDISP_DBGPRTK("%s %d: null context error\n",
				__FUNCTION__, __LINE__);
		return -EINVAL;
	}

	lcd = &core_disp_context->lcd_cfg;

	arg->enable       = lcd->enable;
	arg->panel_type   = lcd->panel_type;
	arg->backcolor_y  = lcd->backcolor_y;
	arg->backcolor_cb = lcd->backcolor_cb;
	arg->backcolor_cr = lcd->backcolor_cr;
	return 0;
}

int ark_disp_set_lcd_cfg(struct ark_disp_lcd_cfg_arg *arg)
{
	struct ark_disp_lcd_cfg_info *lcd;
	int layer_id;

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	if (!core_disp_context) {
		ARKDISP_DBGPRTK("%s %d: null context error\n",
				__FUNCTION__, __LINE__);
		return -1;
	}
	lcd = &core_disp_context->lcd_cfg;

	if (ark_disp_set_lcd_panel_type(arg->panel_type)) {
		ARKDISP_DBGPRTK("%s %d: set panel type error\n",
				__FUNCTION__, __LINE__);
		return -1;
	}

	ark_disp_set_lcd_en(arg->enable);
	ark_disp_set_lcd_backcolor(
			arg->backcolor_y, arg->backcolor_cb, arg->backcolor_cr);

	lcd->enable       = arg->enable;
	lcd->panel_type   = arg->panel_type;
	lcd->backcolor_y  = arg->backcolor_y;
	lcd->backcolor_cb = arg->backcolor_cb;
	lcd->backcolor_cr = arg->backcolor_cr;

	/* NOTE: reopen layers for setting correct frame mode on them */
	for (layer_id = 0; layer_id < ARK_DISP_LAYER_NUM; layer_id++) {
		ark_disp_set_layer_en(
				layer_id, core_disp_context->disp_obj[layer_id].layer_info.enable);
	}

	return 0;
}

int ark_disp_get_tvenc_cfg(struct ark_disp_tvenc_cfg_arg *arg)
{
	struct ark_disp_tvenc_cfg_info *tvenc;

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	if (!core_disp_context) {
		ARKDISP_DBGPRTK("%s %d: null context error\n",
				__FUNCTION__, __LINE__);
		return -1;
	}

	tvenc = &core_disp_context->tvenc_cfg;

	arg->enable       = tvenc->enable;
	arg->out_mode     = tvenc->out_mode;
	arg->backcolor_y  = tvenc->backcolor_y;
	arg->backcolor_cb = tvenc->backcolor_cb;
	arg->backcolor_cr = tvenc->backcolor_cr;
	return 0;
}

int ark_disp_set_tvenc_cfg(struct ark_disp_tvenc_cfg_arg *arg)
{
	struct ark_disp_tvenc_cfg_info *tvenc;
	int layer_id;

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	if (!core_disp_context) {
		ARKDISP_DBGPRTK("%s %d: null context error\n",
				__FUNCTION__, __LINE__);
		return -1;
	}

	tvenc = &core_disp_context->tvenc_cfg;

	if (ark_disp_set_tvenc_out_mode(arg->out_mode)) {
		ARKDISP_DBGPRTK("%s %d: set out mode error\n",
				__FUNCTION__, __LINE__);
		return -1;
	}

	switch (arg->out_mode){
		case ARKDISP_TVENC_OUT_YPBPR_P480HZ60:
		case ARKDISP_TVENC_OUT_YPBPR_P576HZ50:
		case ARKDISP_TVENC_OUT_YPBPR_P720HZ60:
		case ARKDISP_TVENC_OUT_YPBPR_P720HZ50:
		case ARKDISP_TVENC_OUT_YPBPR_P1080HZ60:
		case ARKDISP_TVENC_OUT_YPBPR_P1080HZ50:
		case ARKDISP_TVENC_OUT_VGA_640x480HZ60:
		case ARKDISP_TVENC_OUT_VGA_800x600HZ60:
		case ARKDISP_TVENC_OUT_VGA_1024x768HZ60:
		case ARKDISP_TVENC_OUT_VGA_1280x1024HZ60:
		case ARKDISP_TVENC_OUT_VGA_1900x1200HZ60: // bandwidth  limit
		case ARKDISP_TVENC_OUT_VGA_1280x1024HZ75:
		case ARKDISP_TVENC_OUT_VGA_1280x960HZ85: // bandwidth  limit
		case ARKDISP_TVENC_OUT_VGA_1280x720HZ60:
			tvenc->layer_frame_mode = 0; //progressive read
			break;
		default:
			/* select layer frame mode for cvbs, ypbpr, itu656ou6 */
			tvenc->layer_frame_mode = 1; // interlace read
			break;

	}

	ark_disp_set_tvenc_en(arg->out_mode, arg->enable);
	ark_disp_set_tvenc_backcolor(
			arg->backcolor_y, arg->backcolor_cb, arg->backcolor_cr);

	tvenc->enable       = arg->enable;
	tvenc->out_mode     = arg->out_mode;
	tvenc->backcolor_y  = arg->backcolor_y;
	tvenc->backcolor_cb = arg->backcolor_cb;
	tvenc->backcolor_cr = arg->backcolor_cr;

	/* NOTE: reopen layers for setting correct frame mode on them */
	/*for (layer_id = 0; layer_id < ARK_DISP_LAYER_NUM; layer_id++) {
		ark_disp_set_layer_en(
				layer_id, core_disp_context->disp_obj[layer_id].layer_info.enable);
	}*/

	return 0;
}
EXPORT_SYMBOL(ark_disp_set_tvenc_cfg);

int ark_disp_set_layer_priority_lcd(
		int osd1_pri,int osd2_pri,int osd3_pri, int video_pri, int video2_pri)
{
	struct ark_disp_context *context = core_disp_context;

	if (context == NULL) {
		printk("%s %d: null context err\n", __FUNCTION__, __LINE__);
		return -EFAULT;
	}

	if (video_pri + video2_pri + osd1_pri + osd2_pri + osd3_pri != 10) {
		ARKDISP_DBGPRTK(
				"%s %d: invalid input priorities (0:%d 1:%d 2:%d 3:%d 4%d)\n",
				__FUNCTION__, __LINE__,
				osd1_pri, osd2_pri, osd3_pri, video_pri, video2_pri);
		return -EINVAL;
	}

	// note; priority of different layers should be selected from [0:4] with
	// any two have the same value
	// for the layer that is disabled to display, its priority can be 
	// set to 4, 5, 6, or 7

	ark_disp_set_osd_priority_lcd(0, osd1_pri);
	ark_disp_set_osd_priority_lcd(1, osd2_pri);
	ark_disp_set_osd_priority_lcd(2, osd3_pri);
	ark_disp_set_video_priority_lcd(0, video_pri);
	ark_disp_set_video_priority_lcd(1, video2_pri);

	context->disp_obj[0].layer_info.priority = osd1_pri;
	context->disp_obj[1].layer_info.priority = osd2_pri;
	context->disp_obj[2].layer_info.priority = osd3_pri;
	context->disp_obj[3].layer_info.priority = video_pri;
	context->disp_obj[4].layer_info.priority = video2_pri;

	return 0;
}

int ark_disp_set_layer_priority_tvenc(
		int osd1_pri,int osd2_pri,int osd3_pri, int video_pri, int video2_pri)
{
	struct ark_disp_context *context = core_disp_context;

	if (context == NULL) {
		ARKDISP_DBGPRTK("%s %d: null context err\n", __FUNCTION__, __LINE__);
		return -EFAULT;
	}

	if (video_pri + video2_pri + osd1_pri + osd2_pri + osd3_pri != 10) {
		ARKDISP_DBGPRTK(
				"%s %d: invalid input priorities (0:%d 1:%d 2:%d 3:%d 4%d)\n",
				__FUNCTION__, __LINE__,
				osd1_pri, osd2_pri, osd3_pri, video_pri, video2_pri);
		return -EINVAL;
	}

	// note; priority of different layers should be selected from [0:4] with
	// any two have the same value
	// for the layer that is disabled to display, its priority can be 
	// set to 4, 5, 6, or 7

	ark_disp_set_osd_priority_tvenc(0, osd1_pri);
	ark_disp_set_osd_priority_tvenc(1, osd2_pri);
	ark_disp_set_osd_priority_tvenc(2, osd3_pri);
	ark_disp_set_video_priority_tvenc(0, video_pri);
	ark_disp_set_video_priority_tvenc(1, video2_pri);

	context->disp_obj[0].layer_info.priority = osd1_pri;
	context->disp_obj[1].layer_info.priority = osd2_pri;
	context->disp_obj[2].layer_info.priority = osd3_pri;
	context->disp_obj[3].layer_info.priority = video_pri;
	context->disp_obj[4].layer_info.priority = video2_pri;

	return 0;
}

int ark_disp_reg_check(struct ark_disp_context *context)
{
	int layer_id, id;
	unsigned int val32;
	int res = 0;

	/* check vde settings */
	for (layer_id = 0; layer_id < ARK_DISP_LAYER_NUM; layer_id++) {
		if (ARK_DISP_LAYER_IS_OSD(layer_id)) {
			id = ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(layer_id);
			val32 = ark_disp_get_osd_hue(id);
			if (context->disp_obj[layer_id].layer_info.hue != val32) {
				ARKDISP_DBGPRTK(
						"%s %d: layer_id=%d error osd.hue=0x%02x (reg=0x%02x)\n",
						__FUNCTION__, __LINE__, layer_id,
						context->disp_obj[layer_id].layer_info.hue, val32
					       );
				res = -EINVAL;
			}
			val32 = ark_disp_get_osd_saturation(id);
			if (context->disp_obj[layer_id].layer_info.saturation != val32) {
				ARKDISP_DBGPRTK(
						"%s %d: layer_id=%d error osd.sat=0x%02x (reg=0x%02x)\n",
						__FUNCTION__, __LINE__, layer_id,
						context->disp_obj[layer_id].layer_info.saturation, val32
					       );
				res = -EINVAL;
			}
			val32 = ark_disp_get_osd_brightness(id);
			if (context->disp_obj[layer_id].layer_info.brightness != val32) {
				ARKDISP_DBGPRTK(
						"%s %d: layer_id=%d error osd.bri=0x%02x (reg=0x%02x)\n",
						__FUNCTION__, __LINE__, layer_id,
						context->disp_obj[layer_id].layer_info.brightness, val32
					       );
				res = -EINVAL;
			}
			val32 = ark_disp_get_osd_contrast(id);
			if (context->disp_obj[layer_id].layer_info.contrast != val32) {
				ARKDISP_DBGPRTK(
						"%s %d: layer_id=%d error osd.con=0x%02x (reg=0x%02x)\n",
						__FUNCTION__, __LINE__, layer_id,
						context->disp_obj[layer_id].layer_info.contrast, val32
					       );
				res = -EINVAL;
			}
		} else {
			id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(layer_id);
			val32 = ark_disp_get_video_hue(id);
			if (context->disp_obj[layer_id].layer_info.hue != val32) {
				ARKDISP_DBGPRTK(
						"%s %d: layer_id=%d error vid.hue=0x%02x (reg=0x%02x)\n",
						__FUNCTION__, __LINE__, layer_id,
						context->disp_obj[layer_id].layer_info.hue, val32
					       );
				res = -EINVAL;
			}
			val32 = ark_disp_get_video_saturation(id);
			if (context->disp_obj[layer_id].layer_info.saturation != val32) {
				ARKDISP_DBGPRTK(
						"%s %d: layer_id=%d error vid.sat=0x%02x (reg=0x%02x)\n",
						__FUNCTION__, __LINE__, layer_id,
						context->disp_obj[layer_id].layer_info.saturation, val32
					       );
				res = -EINVAL;
			}
			val32 = ark_disp_get_video_brightness(id);
			if (context->disp_obj[layer_id].layer_info.brightness != val32) {
				ARKDISP_DBGPRTK(
						"%s %d: layer_id=%d error vid.bri=0x%02x (reg=0x%02x)\n",
						__FUNCTION__, __LINE__, layer_id,
						context->disp_obj[layer_id].layer_info.brightness, val32
					       );
				res = -EINVAL;
			}
			val32 = ark_disp_get_video_contrast(id);
			if (context->disp_obj[layer_id].layer_info.contrast != val32) {
				ARKDISP_DBGPRTK(
						"%s %d: layer_id=%d error vid.con=0x%02x (reg=0x%02x)\n",
						__FUNCTION__, __LINE__, layer_id,
						context->disp_obj[layer_id].layer_info.contrast, val32
					       );
				res = -EINVAL;
			}
		}
	}

	return res;
}

//DECLARE_TASKLET(arkdisp_tasklet, ark_disp_do_tasklet, 0);

void ark_disp_select_itu656_pad(void)
{
	unsigned int val;
	val = rSYS_PAD_CTRL0B;
	val |= (0x1FF<<16);
	rSYS_PAD_CTRL0B = val;	
	val = rSYS_PAD_CTRL0A;
	val &= ~(0xF<<4);
	val |= 5<<4;
	rSYS_PAD_CTRL0A = val;
}

void ark_disp_config_tvenc_itu656_direct_in(void)
{
	struct screen_info *screen = &screens[g_screen_id];

    // ITU656_BYP_MODE_CONTROL REG0x3d0
    rLCD_ITU656_BYP_MOD_CTL = // control of itu656 bypass mode via video2
        (1<<12) | // 1=???
        (1<<11) | // 1=???
        (1<<10) | // 1=enable to generate ITU656 no signal detect intr when
                  //    ITU656 input is no signal
        (1<<8) | // 1=data enable output select ivgate whose position is fixed
                 //    OR selec ivgate_delay which is movement of ivgate
        (1<<7);  // 1=input data valid signal is determined by h_active only

    /* program NTSC regs of itu656 bypass mode via video2 */
    // ITU656_BYP_MODE_NTSC_REG0 REG0x3d4
    rLCD_ITU656_BYP_MOD_NTSC_REG0 =  2<<24 | 7<<16 | 28<<8 | 22;
    // ITU656_BYP_MODE_NTSC_REG4 REG0x3e4    
    rLCD_ITU656_BYP_MOD_NTSC_REG4 = 0x43001;

    /* program PAL regs of itu656 bypass mode via video2 */
    // ITU656_BYP_MODE_PAL_REG0 REG0x3e8
    rLCD_ITU656_BYP_MOD_PAL_REG0 = 2<<24 | 2<<16 | 2<<8 | 2;		
    // ITU656_BYP_MODE_PAL_REG4 REG0x3f8
    rLCD_ITU656_BYP_MOD_PAL_REG4 = 0x43001;	
    
	if (screen->screen_id == SCREEN_QUN700) {
		// ITU656_BYP_MODE_NTSC_REG1 REG0x3d8
	    rLCD_ITU656_BYP_MOD_NTSC_REG1 = 948<<16 | 800;
	    // ITU656_BYP_MODE_NTSC_REG2 REG0x3dc
	    rLCD_ITU656_BYP_MOD_NTSC_REG2 = 525<<16 | 480;
	    // ITU656_BYP_MODE_NTSC_REG3 REG0x3e0
	    rLCD_ITU656_BYP_MOD_NTSC_REG3 = (242*1024/(480+0))<<16 | (716*1024/(800+0));		
	    // ITU656_BYP_MODE_PAL_REG1 REG0x3ec
	    rLCD_ITU656_BYP_MOD_PAL_REG1 =  1168<<16 | 800;		
	    // ITU656_BYP_MODE_PAL_REG2 REG0x3f0
	    rLCD_ITU656_BYP_MOD_PAL_REG2 = 520<<16 | 480;		
	    // ITU656_BYP_MODE_PAL_REG3 REG0x3f4
	    rLCD_ITU656_BYP_MOD_PAL_REG3 = (290*1024/(480+0))<<16 | (710*1024/(800+0));
	} else if (screen->screen_id == SCREEN_CLAA101) {
		// ITU656_BYP_MODE_NTSC_REG1 REG0x3d8
	    rLCD_ITU656_BYP_MOD_NTSC_REG1 = 1242<<16 | 1024;
	    // ITU656_BYP_MODE_NTSC_REG2 REG0x3dc
	    rLCD_ITU656_BYP_MOD_NTSC_REG2 = 635<<16 | 600;
	    // ITU656_BYP_MODE_NTSC_REG3 REG0x3e0
	    rLCD_ITU656_BYP_MOD_NTSC_REG3 = (242*1024/(600+0))<<16 | (716*1024/(1024+0));		
	    // ITU656_BYP_MODE_PAL_REG1 REG0x3ec
	    rLCD_ITU656_BYP_MOD_PAL_REG1 =  1515<<16 | 1024;		
	    // ITU656_BYP_MODE_PAL_REG2 REG0x3f0
	    rLCD_ITU656_BYP_MOD_PAL_REG2 = 635<<16 | 600;		
	    // ITU656_BYP_MODE_PAL_REG3 REG0x3f4
	    rLCD_ITU656_BYP_MOD_PAL_REG3 = (290*1024/(600+0))<<16 | (710*1024/(1024+0));	
	}

    // ITU656_BYP_MODE_REG0 REG0x3fc
    rLCD_ITU656_BYP_MOD_REG0 = 4<<28 | 3<<24 | 10<<8 | 10; // data enable vstop, vstart
}

void ark_disp_set_lcd_switch_to_itu656_timing(void)
{
	// CLCD_VIDEO2_CONTROL REG0x320
	rLCD_VIDEO2_CTL |= (1<<11); // use itu656 clock for pre-fifo clock
}

void ark_disp_set_lcd_switch_to_normal_timing(void)
{
	// CLCD_VIDEO2_CONTROL REG0x320
	rLCD_VIDEO2_CTL &= ~(1<<11);  // use axi clk for pre-fifo clock
}

void ark_disp_set_tvenc_switch_to_itu656_timing(void)
{
	// TV_CONTROL REG0x2b0
	rLCD_TV_CONTROL |= (1<<11); // 1=itu656 bypass mode, itu656 timing bypass
	// to TV
}

void ark_disp_set_tvenc_switch_to_normal_timing(void)
{
	// TV_CONTROL REG0x2b0 
	rLCD_TV_CONTROL &= ~(1<<11); // 0=normal mode, clcd timing fed to TV
}

int ark_disp_set_itu656in_status(int status)
{
	core_disp_context->itu656in_en = status;
}

int ark_disp_set_itu656in_en(int enable)
{
	struct screen_info *screen = &screens[g_screen_id];
	int error = 0;
	if (enable) {
		if (core_disp_context->itu656in_en) 
			return 0;

		//backlight off
		gpio_set_value(BACKLIGHT_GPIO, 0);

		ark_disp_select_itu656_pad(); 

		// set itu656 bypass mode regs
		ark_disp_config_tvenc_itu656_direct_in();
		// enable to generate itu656 no signal detect intr when no signal at
		// itu656 input

		// set to use itu656in clk instead of axi clk for pre-fifo clk
		ark_disp_set_lcd_switch_to_itu656_timing();

		// reference codes
		// open video2 layer on LCD 
		// open_video2(1);	

		// only enable video2
		//  set the priority of video 2

		//ark_disp_set_layer_priority_lcd(1, 2, 4, 0, 3);
		// CLCD_INT_CTL REG0x17c
		rLCD_INTERRUPT_CTL |= (1<<10) | (1<<9);
		// bit10: 1=enable itu656 no signal detect intr of 
		//        itu656 bypass mode
		// bit9 : 1=enable lcd intr of itu656 bypass mode
		ark_disp_set_video_layer_size(1, screen->width, screen->height);
		ark_disp_set_video_win_size(1, 720,288);

		rLCD_VIDEO2_SCALE_CTL |= 1 << 7; 

		error = ark_itu656_direct_data_to_lcd_start();
		if (!error)
			core_disp_context->itu656in_en = 1;

		ark_disp_set_video_en_lcd(1, 1);

		//backlight on
		mdelay(100);
		gpio_set_value(BACKLIGHT_GPIO, 1);		
	} else {
		if (!core_disp_context->itu656in_en) 
			return 0;

		//backlight off
		gpio_set_value(BACKLIGHT_GPIO, 0);		
		
		ark_disp_set_video_en_lcd(1, 0);		
		//ark_disp_set_layer_priority_lcd(3, 1, 2, 0, 4);
		rLCD_INTERRUPT_CTL &= ~((1<<10) | (1<<9)); // clear itu656 intr bits

		//rLCD_CONTROL &= ~(1 << 6);//del by nan

		// set to use axi clk for pre-fifo clk
		ark_disp_set_lcd_switch_to_normal_timing();

		ark_itu656_stop();
		ark_disp_set_osd_en_lcd(0, 1);

		core_disp_context->itu656in_en = 0;

			//backlight on
		mdelay(100);
		gpio_set_value(BACKLIGHT_GPIO, 1);		
	}

	return error;
}

int ark_disp_set_gui_tvout(int enable)
{
	struct ark_disp_tvenc_cfg_arg tvenc_cfg = {0};	
	struct arkfb_update_window tvout_arg = {0};
	struct ark_disp_obj_info *disp_tvout = &core_disp_context->disp_obj[3];
	struct screen_info *screen = &screens[g_screen_id];
	int tvout_mode = screen->tvout_format;

	tvenc_cfg.enable = enable;
	tvenc_cfg.out_mode = tvout_mode;
	tvenc_cfg.backcolor_y = 0x10;
	tvenc_cfg.backcolor_cb = 0x80;
	tvenc_cfg.backcolor_cr = 0x80;
	ark_disp_set_tvenc_cfg(&tvenc_cfg);	

	if (enable) {
		tvout_arg.win_width = screen->width;
		tvout_arg.win_height  = screen->height;
		tvout_arg.width = screen->width;
		tvout_arg.height = screen->height;
		if (tvout_mode == ARKDISP_TVENC_OUT_CVBS_NTSC) {
			tvout_arg.out_width = 720; 
			tvout_arg.out_height = 480;
		} else {
			;		
		}
		tvout_arg.format = ARK_DISP_OSD_PIXFMT_RGB888;
		tvout_arg.interlace_out = 1;
		disp_tvout->writeback = 0;
		ark_fb_update_window(disp_tvout, &tvout_arg);	
		ark_disp_set_video_data_addr(0, 0, 0, 0, rLCD_OSD1_ADDR, 0, 0);
		ark_disp_set_video_en_tvenc(0, 1);
	} else {
		ark_disp_set_video_en_tvenc(0, 0);
	}
}

int ark_fb_update_window_by_layer_id(int layer_id, 
		struct arkfb_update_window *arg)
{
	struct ark_disp_obj_info *disp = &core_disp_context->disp_obj[layer_id];

	ark_fb_update_window(disp, arg);
}
EXPORT_SYMBOL(ark_fb_update_window_by_layer_id);

//#ifndef ARK_DISP_USE_HW_EMULATION
/* This function is an interrupt service routine
 *
 * Arguments:
 *
 * Return:
 *
 */
extern void ark_itu656_display_int_handler(void);
extern unsigned int uboot_backcar_status;
#define WAIT_TIMES 8
static unsigned int switch_flag = 0, wait_count = 0;

	irqreturn_t
ark_disp_intr_handler(int irq, void *dev_id)
{
	int id;
	unsigned long intr_sts, flags;
	struct list_head *list = NULL;
	struct ark_disp_obj_info *disp = NULL;
	struct ark_disp_slot_buf_ctrl *sbc = NULL;
	struct ark_disp_device *display = (struct ark_disp_device *)dev_id;
	struct ark_disp_context *context =&display->context;
	struct ark_disp_obj_info *disp_tvout = &context->disp_obj[3];
	struct screen_info *screen = &screens[g_screen_id];
	unsigned int frame_mask;

	intr_sts = rLCD_INTERRUPT_STATUS; // get current intr status
	// clear display int and keep scaler write back int
	rLCD_INTERRUPT_STATUS = 1 << 8;

	/*1. when boot_comlete is 1, backcar is handled by app 
	**2. when kernel boot complete, but backcar is on, now  uboot_backcar_status is 0(backcar on), we need close backcar, this time should be
	** the last time kernel handled, but switch_flag must be handle to 0, or else blackliglit is off
	**/
#if 0	
	if(boot_complete == 0 || uboot_backcar_status == 0 || switch_flag !=  0) 			
	{
		int status = gpio_get_value(BACKCAR_GPIO);

		if(uboot_backcar_status != status){			
			uboot_backcar_status = status;
			switch_flag = 1;
		}

		switch(switch_flag){
			case 0:
				break;
			case 1:
				gpio_set_value(GPIO81, 0);
				printk("set blacklight off\n");
				switch_flag = 2;
			case 2:
				wait_count++;
				if(wait_count ==  WAIT_TIMES){
					switch_flag = 3;
					wait_count = 0;
				}
				break;
			case 3:
				if(ark_disp_set_itu656in_en(!uboot_backcar_status) != 0){
					printk(KERN_ALERT "itu656in_en set failed \n");
				}else{
					if(uboot_backcar_status){
						if(!boot_complete){
							ark_disp_set_osd_format(0, ARK_DISP_OSD_PIXFMT_YUYV, 0, 1);
							ark_disp_set_osd_data_addr(0, 0x3000000);//set framebuffer to 0x3000000, because now logo loacate in here
						}
						ark_disp_set_osd_en_lcd(0, 1);
					}else{				  
						ark_disp_set_osd_en_lcd(0, 0);
					}
				}
				switch_flag = 4;
				break;
			case 4:
				wait_count++;
				if(wait_count ==  WAIT_TIMES){
					switch_flag = 5;
					wait_count = 0;
				}
				break;
			case 5:
				gpio_set_value(GPIO81, 1);
				printk("set blacklight on\n");
				switch_flag = 0;
				break;			
			default:				
				break;
		}

		//if( status == 0 || boot_complete == 0)
		//if( boot_complete == 0)				
		return IRQ_HANDLED;
	}
#else
	if (!boot_complete) return IRQ_HANDLED;
#endif

	if (is_two_screen_display(screen)) {
		frame_mask = 0x1;
		if (is_interlace_tvout(screen)) {
			if ((intr_sts & (1 << 7)) && frame_buf_addr_next_tvout && 
				disp_tvout->show_tv) {
				ark_disp_set_video_data_addr(0, ARK_DISP_VIDEO_PIXFMT_YUYV,
					0, 0, frame_buf_addr_next_tvout, 0, 0);	
				frame_buf_addr_next_tvout = 0;
			}
			if (intr_sts & (1 << 3)) {
				if (intr_sts & (1 << 6) && oddfield_buf_addr_next_tvout) {
					ark_disp_set_video_data_addr(0, ARK_DISP_VIDEO_PIXFMT_YUYV,
						0, 0, oddfield_buf_addr_next_tvout, 0, 0);	
					oddfield_buf_addr_next_tvout = 0;					
				} 
				if (intr_sts & (1 << 7) && evenfield_buf_addr_next_tvout) {
					ark_disp_set_video_data_addr(0, ARK_DISP_VIDEO_PIXFMT_YUYV,
						0, 0, evenfield_buf_addr_next_tvout, 0, 0);	
					evenfield_buf_addr_next_tvout = 0;
				}
			}
		} else {
			if ((intr_sts & (1 << 3)) && frame_buf_addr_next_tvout && 
				disp_tvout->show_tv) {
				ark_disp_set_video_data_addr(0, ARK_DISP_VIDEO_PIXFMT_YUYV,
					0, 0, frame_buf_addr_next_tvout, 0, 0);	
				frame_buf_addr_next_tvout = 0; 
			}
		}
	} else frame_mask = 0x89;
	
	/* update statistics */
	if (intr_sts & frame_mask) {
		ark_itu656_display_int_handler();
		
		/* update addr regs of video */
		for (id = 0; id < ARK_DISP_LAYER_NUM; id++) {
			disp = &context->disp_obj[id];
			
			if (frame_buf_addr_next_lcd[id]) {
				if (ARK_DISP_LAYER_IS_OSD(id)) {
					ark_disp_set_osd_data_addr(
							ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(id),
							frame_buf_addr_next_lcd[id]);
				} else {
					ark_disp_set_video_data_addr(
							ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id),
							0, 0, 0,
							frame_buf_addr_next_lcd[id],
							0, 0);
				}	
				frame_buf_addr_next_lcd[id] = 0;
			}
			
			//printk("1");
			if (disp->writeback) {
			if (!list_empty(&disp->buf_info.indep_disp_list) ) {
				list = disp->buf_info.indep_disp_list.next;
				sbc = list_entry(list, struct ark_disp_slot_buf_ctrl, list);
				if (ARK_DISP_LAYER_IS_OSD(id)) {
					ark_disp_set_osd_data_addr(
							ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(id),
							sbc->buf_base_phys);
				} else {
					ark_disp_set_video_data_addr(
							ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id),
							ARK_DISP_VIDEO_PIXFMT_YUYV,//context->disp_obj[id].layer_info.pixfmt, // format
							disp->layer_info.xres,//context->disp_obj[id].layer_info.src_xres, // width
							disp->layer_info.yres,//context->disp_obj[id].layer_info.src_yres, // height
							sbc->buf_base_phys, 0, 0);
				}

				frame_buf_addr_next_tvout = sbc->buf_base_phys;
				
				//printk("2");
				spin_lock_irqsave(&disp->lock, flags);
				list_del(list);
				spin_unlock_irqrestore(&disp->lock, flags);
				spin_lock_irqsave(&context->lock, flags);
				list_add_tail(list, &context->free_list);
				spin_unlock_irqrestore(&context->lock, flags);
				disp->buf_info.indep_disp_list_entry_count--;	
				//printk("3");
				if(test_bit(SBUF_STATUS, &disp->need_wait)){
					clear_bit(SBUF_STATUS, &disp->need_wait);
					wake_up_interruptible(&disp->waitq);
				}
				//printk("4");
			}

			}else{
				struct ark_disp_buf_info *buf_info = &disp->buf_info;
				unsigned int posin;				
				unsigned int posout;
				unsigned int phy_addr, phy_cbaddr, phy_craddr; 
				
				spin_lock_irqsave(&disp->lock, flags);
				posin = buf_info->no_writeback_posin & (ARK_VIDEO_MAX_FRAME -1);
				posout = buf_info->no_writeback_posout & (ARK_VIDEO_MAX_FRAME -1);

				if(buf_info->no_writeback_posout < buf_info->no_writeback_posin){
					phy_addr = buf_info->no_writeback_phys_addr[posout].phy_addr;
					phy_cbaddr = buf_info->no_writeback_phys_addr[posout].phy_cbaddr;
					phy_craddr = buf_info->no_writeback_phys_addr[posout].phy_craddr;
					if (ARK_DISP_LAYER_IS_OSD(id)) {
						ark_disp_set_osd_data_addr(
							ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(id),
							phy_addr);
					} else {
						ark_disp_set_video_data_addr(
							ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id),
							disp->layer_info.format,//context->disp_obj[id].layer_info.pixfmt, // format
							disp->layer_info.xres,//context->disp_obj[id].layer_info.src_xres, // width
							disp->layer_info.yres,//context->disp_obj[id].layer_info.src_yres, // height
							phy_addr, phy_cbaddr, phy_craddr);
					}
					buf_info->no_writeback_posout++;
				}
				spin_unlock_irqrestore(&context->lock, flags);
				
				if(test_bit(LAYER_BYPASS, &disp->need_wait)){
					clear_bit(LAYER_BYPASS, &disp->need_wait);
					wake_up_interruptible(&disp->waitq);
				}
			}
		}

		/* wake up the wait queue */
		//wake_up(&core_disp_context->lcd_cfg.timing_intr_waitq);
		//core_disp_context->lcd_cfg.timing_intr_count++;
	}

#if 0

	if (intr_sts & 0xCC) {
		/* update addr regs of video */
		for (id = 0; id < ARK_DISP_LAYER_NUM; id++) {
			if (frame_buf_addr_next_tvenc[id]) {
				if (ARK_DISP_LAYER_IS_OSD(id)) {
					ark_disp_set_osd_data_addr_cvbs(
							ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(id),
							core_disp_context->disp_obj[id].layer_info.pixfmt, // format
							core_disp_context->disp_obj[id].layer_info.src_xres, // width
							core_disp_context->disp_obj[id].layer_info.src_yres, // height
							frame_buf_addr_next_tvenc[id]);
				} else {
					ark_disp_set_video_data_addr_cvbs(
							ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id),
							core_disp_context->disp_obj[id].layer_info.pixfmt, // format
							core_disp_context->disp_obj[id].layer_info.src_xres, // width
							core_disp_context->disp_obj[id].layer_info.src_yres, // height
							frame_buf_addr_next_tvenc[id]);
				}
				frame_buf_addr_next_tvenc[id] = 0;
			}
		}

		/* wake up the wait queue */
		// wake_up(&core_disp_context->tvenc_cfg.timing_intr_waitq);
		// core_disp_context->tvenc_cfg.timing_intr_count++;
	}
	if (dev_id) {
		/* schedule a tasklet */
		tasklet_schedule(&arkdisp_tasklet); // called from low pri softirq
	} else {
		ARKDISP_DBGPRTK("%s %d: not dev_id provided, abort\n",
				__FUNCTION__, __LINE__);
	}
#endif
	return IRQ_HANDLED;
}

irqreturn_t ark_disp_scal_intr_handler(int irq, void *dev_id)
{
	unsigned int intr_sts;
	struct ark_disp_device *display = (struct ark_disp_device *)dev_id;
	struct ark_disp_context *context =&display->context;
	struct ark_disp_obj_info *disp_writeback = &context->disp_obj[4];
	struct ark_disp_obj_info *disp = NULL;
	unsigned long flags;
	int id;
	
	//ARKDISP_DBGPRTK("%s %d: irq %d\n", __FUNCTION__, __LINE__, irq);
	//printk("%s %d: irq %d\n", __FUNCTION__, __LINE__, irq);
	// CLCD_INT_STA REG0x180
	intr_sts = rLCD_INTERRUPT_STATUS; // get current intr status
	rLCD_INTERRUPT_STATUS &= ~(1 << 8);
	//printk("x");	
	
	//\u4e2d\u65ad\u4ea7\u751f\u65f6\u53ef\u80fd\u8fd8\u6709\u6700\u540e\u4e00\u4e9b\u6570\u636e\u53ea\u53d1\u751f\u8bf7\u6c42\u5e76\u6ca1\u6709\u771f\u6b63\u5199\u5b8c\uff0c
	//\u8fd9\u65f6\u5019\u5982\u679c\u9a6c\u4e0a\u5904\u7406\u4e0b\u4e00\u6b21\u56de\u5199\u4f1a\u9020\u6210\u56de\u5199\u5f02\u5e38
	udelay(200);
	
	if (test_bit(WRITE_BACK, &disp_writeback->need_wait)) {
		for (id = 0; id < ARK_DISP_LAYER_NUM; id++) {
			disp = &context->disp_obj[id];
			if (test_bit(WRITE_BACK, &disp->need_wait)) {
				spin_lock_irqsave(&disp->lock, flags);
				if(disp_writeback->write_list) {
					list_add_tail(disp_writeback->write_list, &disp->buf_info.indep_disp_list);
					disp->buf_info.indep_disp_list_entry_count++;
				}
				disp_writeback->write_list = NULL;
				spin_unlock_irqrestore(&disp->lock, flags);
				clear_bit(WRITE_BACK, &disp->need_wait);
				break;
			}
		}
		clear_bit(WRITE_BACK, &disp_writeback->need_wait);
		wake_up_interruptible(&disp_writeback->waitq);		
	}	

	//printk("s %d\n", scal_int_time);	
#if 0
	if (dev_id) {
		/* schedule a tasklet */
		tasklet_schedule(&arkdisp_tasklet); // called from low pri softirq
	} else {
		ARKDISP_DBGPRTK("%s %d: not dev_id provided, abort\n",
				__FUNCTION__, __LINE__);
	}
#endif
	return IRQ_HANDLED;
}
//#endif

irqreturn_t ark_backcar_intr_handler(int irq, void *dev_id)
{
	if (boot_complete) return IRQ_HANDLED;

	struct ark_disp_device *display = (struct ark_disp_device *)dev_id;
	struct ark_disp_context *context =&display->context;
	struct ark_disp_obj_info *disp_ui = &context->disp_obj[0];	
	struct ark_disp_layer_info *layer = &disp_ui->layer_info;
	int status = gpio_get_value(BACKCAR_GPIO);
	//printk("ark_backcar_intr_handler status=%d.\n", status);
	if (status) {
		ark_disp_set_itu656in_en(0);
		if (layer->screen == ARKDISP_SCREEN_TVENC) {	
		   ark_disp_set_osd_en_tvenc(0, 1);			   	
		} else {
		   ark_disp_set_osd_en_lcd(0, 1);
		}		
	} else {
		if (layer->screen == ARKDISP_SCREEN_TVENC) {	
		   ark_disp_set_osd_en_tvenc(0, 0);			   	
		} else {
		   ark_disp_set_osd_en_lcd(0, 0);
		}		
		ark_disp_set_itu656in_en(1);		
	}
	return IRQ_HANDLED;
}

void ark_disp_wait_lcd_frame_int(void)
{
	// wait until LCD timing point intr happens (which is VSync here)
	while(!(rLCD_INTERRUPT_STATUS & 0x01));
	// the timing point is set at bit22-21 on CLCD_CONTROL reg
}

void ark_disp_wait_tvenc_frame_int(void)
{
	// wait until TV timing point intr happens (which is VSYNC here)
	while(!(rLCD_INTERRUPT_STATUS & 0x08));	
	// NOTE: the timing point is set at bit12-11 on CLCD_CONTROL reg
}

void ark_disp_enable_interrupts(void)
{
	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);
	struct screen_info *screen = &screens[g_screen_id];

	rLCD_INTERRUPT_CTL = 0;

	if (screen->tvenc & ARK_DISP_LAYER_MASK) {
		if (is_interlace_tvenc(screen)) {
			rLCD_INTERRUPT_CTL |= 1 << 7;	//tv frame intr
		}
		else
			rLCD_INTERRUPT_CTL |= 1 << 3;	//tv frame intr
	} 

	if (is_two_screen_display(screen)) {
		rLCD_INTERRUPT_CTL |= 1 << 3;
		if (is_interlace_tvout(screen))
			rLCD_INTERRUPT_CTL |= 1 << 7;
	}
		

	if ((screen->tvenc & ARK_DISP_LAYER_MASK) != ARK_DISP_LAYER_MASK) 
		rLCD_INTERRUPT_CTL |= 1;				//lcd frame intr

	ARKDISP_DBGPRTK("%s %d: chk\n", __FUNCTION__, __LINE__);

	/* NOTE: rLCD_CONTROL has been configured to generate interrupt at
	 * the start of vertical sync of lcd and tv frame in 
	 * ark_disp_set_lcd_panel_type()
	 */

	rLCD_INTERRUPT_CTL |= 1<<8; // enable video2 write back frame finish intr

	ARKDISP_DBGPRTK("%s %d: chk\n", __FUNCTION__, __LINE__);
}

void ark_disp_disable_interrupts(void)
{
	// CLCD_INT_CTL REG0x17c
	rLCD_INTERRUPT_CTL = 0; // disable all interrupts
}

/* This function waits vsync interrupts
 *
 * Arguments:
 *   context : pointer to the driver control context
 *   cnt     : number of sync interrupts to wait
 *
 * Return:
 *   return 0 if success, else return -1
 */
int ark_disp_wait_frame_sync_interrupt(struct ark_disp_obj_info *disp)
{
	unsigned int count;
	int rc;
	
	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	if (disp->layer_info.screen == ARKDISP_SCREEN_LCD) {
		struct ark_disp_lcd_cfg_info *lcd = &core_disp_context->lcd_cfg;

		count = lcd->timing_intr_count;

		rc = wait_event_interruptible_timeout(lcd->timing_intr_waitq, 
				(lcd->timing_intr_count > count ||
				 lcd->timing_intr_count < 2),
				msecs_to_jiffies(1000)); // 1  second at most
	} else {
		struct ark_disp_tvenc_cfg_info *tvenc = &core_disp_context->tvenc_cfg;

		count = tvenc->timing_intr_count;

		rc = wait_event_interruptible_timeout(tvenc->timing_intr_waitq, 
				(tvenc->timing_intr_count > count ||
				 tvenc->timing_intr_count < 2),
				msecs_to_jiffies(1000)); // 1  second at most
	}

	return 0;
}

extern unsigned int uboot_backcar_status;

int ark_disp_dev_init(struct ark_disp_context *context)
{
	int id, i;
	struct screen_info *screen = &screens[g_screen_id];

	context->screen_width = screen->width;
	context->screen_height = screen->height;

#if DISPLAY_NEEDED_INIT
	struct ark_disp_lcd_cfg_arg   lcd_cfg_arg;
	struct ark_disp_tvenc_cfg_arg tvenc_cfg_arg;
	struct ark_disp_vde_cfg_arg   vde_cfg_arg;
	struct ark_disp_layer_cfg_arg layer_cfg_arg;
	int error;
#endif

	if(context == NULL)
		return -EFAULT;
	core_disp_context = context;
	/* first of all, disable all layer output */ //del by nan这个函数里面的core_disp_context->disp_obj[layer_id].layer_info都没初始化，这里只是简单的关屏，现在开机画面和倒车需要，不用到它
	//for (id = 0; id < ARK_DISP_LAYER_NUM; id++)
	//        ark_disp_set_layer_en(id, 0);

	/* initialize lock */
	spin_lock_init(&context->lock);

	/* initialize wait queue */
	init_waitqueue_head(&context->waitq);

	/* initialize state variables */
	for (id = 0; id < ARK_DISP_LAYER_NUM; id++) {
		frame_buf_addr_next_lcd[id]   = 0;
		frame_buf_addr_next_tvenc[id] = 0;
	}
	frame_buf_addr_next_tvout = 0;
	oddfield_buf_addr_next_tvout = 0;
	evenfield_buf_addr_next_tvout = 0;

	/* initialize interrupt numbers and control flags */
	context->irq = -1;
	context->scal_irq = -1;

	/* initialize output frame return wait link list */
	INIT_LIST_HEAD(&context->out_cfg.rtn_wait_list);
	context->out_cfg.rtn_wait_list_entry_count = 0;
	context->out_cfg.src_disp_obj = NULL;

	/* disable bypassing v4l2 data to output ringpair */
	context->out_cfg.out_en = 0;

	/* NOTE: mmio will be set by the calling func after succeeded execution
	 * of this func
	 */

	/* initialize lcd control settings */
	context->lcd_cfg.enable = 1;
	spin_lock_init(&context->lcd_cfg.lock);
	init_waitqueue_head(&context->lcd_cfg.timing_intr_waitq);
	context->lcd_cfg.timing_intr_count = 0;
	context->lcd_cfg.panel_type = ARKDISP_LCD_PANEL_PARALLEL_24BIT;
	context->lcd_cfg.backcolor_y  = 0x10;//BLUE_Y;
	context->lcd_cfg.backcolor_cb = 0x80;//BLUE_U;
	context->lcd_cfg.backcolor_cr = 0x80;//BLUE_V;

	/* initialize tvenc control settings */
	context->tvenc_cfg.enable = 1;
	spin_lock_init(&context->tvenc_cfg.lock);
	init_waitqueue_head(&context->tvenc_cfg.timing_intr_waitq);
	context->tvenc_cfg.timing_intr_count = 0;
	context->tvenc_cfg.out_mode = ARKDISP_TVENC_OUT_YPBPR_P576HZ50;
	context->tvenc_cfg.backcolor_y  = 0x10;//BLUE_Y;
	context->tvenc_cfg.backcolor_cb = 0x80;//BLUE_U;
	context->tvenc_cfg.backcolor_cr = 0x80;//BLUE_V;

	INIT_LIST_HEAD(&context->free_list);
	context->free_list_entry_count = 0;

	/* construct free list (which contains free slot control nodes) */
	for (i = 0; i < ARK_VIDEO_MAX_FRAME; i++) {		
		struct ark_disp_slot_buf_ctrl *sbc = NULL;

		sbc = &context->sbuf_ctrl[i];
		sbc->buf_size = context->screen_width * context->screen_height * 2;
		sbc->buf_base_virt  =  __get_free_pages(GFP_KERNEL, get_order(sbc->buf_size));
		if (!sbc->buf_base_virt) {
			printk("%s sbuf_ctrl virt fail\n", __func__);
			while(i-- >= 0){
				free_pages(context->sbuf_ctrl[i].buf_base_virt, 
					get_order(context->sbuf_ctrl[i].buf_size));
			}
			return -ENOMEM;
		}
		context->sbuf_ctrl[i].buf_base_phys = virt_to_phys((void *)sbc->buf_base_virt );
		INIT_LIST_HEAD(&sbc->list);
		list_add_tail(&sbc->list, &context->free_list);
		context->free_list_entry_count++;
	}

	INIT_LIST_HEAD(&context->free_vbuf_list);
	context->free_vbuf_list_entry_count = 0;

	context->deinterlace_en = 1;
	context->prescale_en = 1;
	context->itu656in_en = 1;

	/* initialize display object */
	for (id = 0; id < ARK_DISP_LAYER_NUM; id++) {
		struct ark_disp_buf_info *buf;
		struct ark_disp_layer_info *layer;

		context->disp_obj[id].id = id;
		context->disp_obj[id].writeback = 0;
		/* initialize lock */
		spin_lock_init(&context->disp_obj[id].lock);

		/* initialize wait queue */
		init_waitqueue_head(&context->disp_obj[id].waitq);
		context->disp_obj[id].need_wait = 0;

		/* initialize buffer ctrl info */
		buf = &context->disp_obj[id].buf_info;
		if (ARK_DISP_LAYER_IS_OSD(id)) {
			buf->min_buf_num   = 2;
			buf->buf_num       = 2;
			buf->min_buf_size  = context->screen_width*context->screen_height*4;
			buf->buf_size      = context->screen_width*context->screen_height*4;
			buf->buf_pool_size = context->screen_width*context->screen_height*4*2;			
		} else {
			buf->min_buf_num   = 2;
			buf->buf_num       = 2;
			buf->min_buf_size  = context->screen_width*context->screen_height*4;
			buf->buf_size      = context->screen_width*context->screen_height*4;
			buf->buf_pool_size = context->screen_width*context->screen_height*4*2;
		}
		
		buf->no_writeback_posin = 0;
		buf->no_writeback_posout = 0;
		for(i = 0; i < ARK_VIDEO_MAX_FRAME; i++){
			buf->no_writeback_phys_addr[i].phy_addr = 0;
			buf->no_writeback_phys_addr[i].phy_cbaddr= 0;
			buf->no_writeback_phys_addr[i].phy_craddr= 0;
		}
		
		buf->buf_pool_base_virt = __get_free_pages(GFP_KERNEL, get_order(buf->buf_pool_size));
		if ( !buf->buf_pool_base_virt  ) {
			printk("%s buf_pool_base_virt fail\n", __func__);
			while(id-- >= 0){
				free_pages(context->disp_obj[id].buf_info.buf_pool_base_virt, 
					get_order(context->disp_obj[id].buf_info.buf_pool_size));
			}			
			return -ENOMEM;
		}
		buf->buf_pool_base_phys = virt_to_phys((void *)buf->buf_pool_base_virt );
			
		buf->cur_buf_addr_virt = buf->buf_pool_base_virt;
		buf->cur_buf_addr_phys = buf->buf_pool_base_phys;
		INIT_LIST_HEAD(&buf->disp_list);
		INIT_LIST_HEAD(&buf->wait_list);
		INIT_LIST_HEAD(&buf->done_list);
		INIT_LIST_HEAD(&buf->indep_disp_list);
		buf->disp_list_entry_count = 0;
		buf->done_list_entry_count = 0;
		buf->indep_disp_list_entry_count = 0;

		/* initialize layer ctrl info */
		layer = &context->disp_obj[id].layer_info;

		if((id == 4) && (uboot_backcar_status == 0)) //判断是否是快速倒车，是快速倒车则不要关闭倒车画面
			layer->enable = 1;   /* enable window for display */
		else
			layer->enable = 0;   /* enable window for display */

		layer->interlaced = 0;
		layer->alpha_blend_en = 1;
		if (ARK_DISP_LAYER_IS_OSD(id)) {
			layer->per_pix_alpha_blend_en = 1;
			layer->type = ARKDISP_LAYER_OSD;
		} else {
			layer->per_pix_alpha_blend_en = 0;
			layer->type = ARKDISP_LAYER_VIDEO;
		}
		if ((screen->tvenc >> id) & 1)
			layer->screen = ARKDISP_SCREEN_TVENC;	
		else
			layer->screen = ARKDISP_SCREEN_LCD;
		printk("tvenc=0x%x, id=%d, layer->screen=%d.\n", screen->tvenc, id, layer->screen);
		layer->pixfmt = ARKDISP_PIXFMT_RGBA888;

		if (ARK_DISP_LAYER_IS_OSD(id)) {
			layer->src_xres = context->screen_width;
			layer->src_yres = context->screen_height;
			layer->xres = context->screen_width;
			layer->yres = context->screen_height;
			layer->xpos = 0;
			layer->ypos = 0;
			layer->crop_xres = context->screen_width; /* x resolution of crop window in pixels */
			layer->crop_yres = context->screen_height; /* y resolution of crop window in lines */
			layer->crop_xpos = 0; /* x pos of crop window's upper left corner */
			layer->crop_ypos = 0; /* y pos of crop window's upper left corner */			
		} else {
			layer->src_xres = context->screen_width;
			layer->src_yres = context->screen_height;
			layer->xres = context->screen_width;
			layer->yres = context->screen_height;
			layer->xpos = 0;
			layer->ypos = 0;
			layer->crop_xres = context->screen_width; /* x resolution of crop window in pixels */
			layer->crop_yres =  context->screen_height; /* y resolution of crop window in lines */
			layer->crop_xpos = 0; /* x pos of crop window's upper left corner */
			layer->crop_ypos = 0; /* y pos of crop window's upper left corner */		
		}
		layer->bits_per_pixel = 32;
		layer->line_length = layer->src_xres * layer->bits_per_pixel / 8;
		layer->blend_mode = ARKDISP_BLDMODE_WHOLE_OVERWRT;
		layer->alpha = 0xff;    /* alpha blending value */
		if (ARK_DISP_LAYER_IS_OSD(id)) {
			int osd_id = ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(id);
			layer->hue = ark_disp_get_osd_hue(osd_id);
			layer->saturation = ark_disp_get_osd_saturation(osd_id);
			layer->brightness = ark_disp_get_osd_brightness(osd_id);
			layer->contrast = ark_disp_get_osd_contrast(osd_id);		
		} else {
			int video_id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id);
			layer->hue = ark_disp_get_video_hue(video_id);
			layer->saturation = ark_disp_get_video_saturation(video_id);
			layer->brightness = ark_disp_get_video_brightness(video_id);
			layer->contrast = ark_disp_get_video_contrast(video_id);
		}

		layer->colorkey_enable = 0;
		layer->colorkey_y = 0;
		layer->colorkey_cb = 0;
		layer->colorkey_cr = 0;
		layer->colorkey_y_thld = 0;
		layer->colorkey_cb_thld = 0;
		layer->colorkey_cr_thld = 0;

		layer->left_cut_num = 0;
		layer->right_cut_num = 0;
		layer->top_cut_num = 0;
		layer->bottom_cut_num = 0;
		//layer->priority = id;
	}

	/* configure hardware */
#if DISPLAY_NEEDED_INIT
	for (id = 0; id < ARK_DISP_LAYER_NUM; id++) {
		vde_cfg_arg.layer_id = id;

		spin_lock(&context->disp_obj[id].lock);
		ark_disp_get_vde_cfg(&vde_cfg_arg);
		error = ark_disp_set_vde_cfg(&vde_cfg_arg);
		spin_unlock(&context->disp_obj[id].lock);
		
		layer_cfg_arg.layer_id = id;

		spin_lock(&context->disp_obj[id].lock);
		ark_disp_get_layer_cfg(&layer_cfg_arg);
		spin_unlock(&context->disp_obj[id].lock);
	}

	spin_lock(&context->lcd_cfg.lock);
	ark_disp_get_lcd_cfg(&lcd_cfg_arg);
	ark_disp_set_lcd_cfg(&lcd_cfg_arg);
	spin_unlock(&context->lcd_cfg.lock);

	spin_lock(&context->tvenc_cfg.lock);
	ark_disp_get_tvenc_cfg(&tvenc_cfg_arg);
	ark_disp_set_tvenc_cfg(&tvenc_cfg_arg);
	spin_unlock(&context->tvenc_cfg.lock);

#endif

	if(uboot_backcar_status){
		//rLCD_CONTROL = 0x02800081; 
		//ark_disp_set_layer_priority_lcd(1, 2, 4, 0, 3); 
	}else{
		//rLCD_CONTROL = 0x02800041;
		//ark_disp_set_layer_priority_lcd(2, 1, 4, 0, 3);
	}

	ark_disp_set_lcd_backcolor(0x10, 0x80, 0x80);
	
	return 0;
}