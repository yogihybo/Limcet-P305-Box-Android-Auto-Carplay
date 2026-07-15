#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fb.h>
#include <linux/matroxfb.h>
#include <media/v4l2-dev.h>
#include <linux/videodev2.h>
#include <media/videobuf-core.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf-dma-contig.h>
#include <linux/module.h>
#include <linux/time.h>
#include <linux/ark/ringpair.h>
#include <linux/ark/ark_display_io.h>
#include <linux/miscdevice.h>

#include "ark_display.h"
#include "ark_display_priv.h"
#include "ark_display_fb.h"


extern unsigned int uboot_backcar_status;
//backcar yuyv  data after writeback yuyv data before write back y_uv 420, picture yuv 422
int ark_fb_update_window(struct ark_disp_obj_info *disp, 
		struct arkfb_update_window *arg)
{
	int id = disp->id;

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	if (ARK_DISP_LAYER_IS_OSD(id)) {
		if(arg->win_width != arg->width ||
				arg->win_height != arg->height ||
				arg->out_width != arg->win_width ||
				arg->out_height != arg->win_height)
		{
			printk("%s invalid parameter.\n", __FUNCTION__);
			return -1;
		}

		ark_disp_set_osd_format(id, arg->format, arg->rgb_order,
				arg->yuyv_order);

		ark_disp_set_osd_source_size(id, arg->width, arg->height); 

		ark_disp_set_osd_layer_size(id, arg->out_width, arg->out_height);

		ark_disp_set_osd_layer_position(id, arg->out_x, arg->out_y);
	} else {
		id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id);
		if(id == 0){
			ark_disp_set_video_format(id, arg->format, arg->rgb_order,
					arg->yuyv_order, 0);
		}else{
			ark_disp_set_video2_writeback_en(0, 0, 0, 0);
			ark_disp_set_video_format(id, arg->format, arg->rgb_order,
					arg->yuyv_order, 0);
		}

		ark_disp_set_video_source_size(id, arg->width, arg->height);

		ark_disp_set_video_win_size(id, arg->win_width, arg->win_height);

		ark_disp_set_video_layer_size(id, arg->out_width, arg->out_height);

		ark_disp_set_video_layer_position(id, arg->out_x, arg->out_y);

		//if(arg->win_width != arg->out_width || arg->win_height != arg->out_height)
		ark_disp_set_video_scal(id, arg->win_width, arg->win_height, 0, 0,
			0, 0, arg->out_width, arg->out_height, arg->interlace_out, 0);
	}

	disp->layer_info.src_xres = arg->width;
	disp->layer_info.src_yres = arg->height;
	disp->layer_info.xres = arg->out_width;
	disp->layer_info.yres = arg->out_height;
	disp->layer_info.xpos = arg->out_x;
	disp->layer_info.ypos = arg->out_y;
	disp->layer_info.crop_xres = arg->win_width;
	disp->layer_info.crop_yres = arg->win_height;
	disp->layer_info.crop_xpos = arg->win_x;
	disp->layer_info.crop_ypos = arg->win_y;
	disp->layer_info.format = arg->format;
	disp->layer_info.rgb_order = arg->rgb_order;
	disp->layer_info.yuyv_order = arg->yuyv_order;

	return 0;
}

int ark_video_update_window(struct ark_disp_obj_info *disp, 
		struct arkfb_update_window *arg)
{
	int id = disp->id;
	struct ark_disp_obj_info *disp_writeback = NULL;
	struct ark_disp_context *context = 
		container_of(disp, struct ark_disp_context, disp_obj[id]);
	int out_height;

	disp_writeback = &context->disp_obj[4];

	if (arg->out_width * arg->out_height > 
		context->screen_width * context->screen_height) {
		printk("%s invalid parameter.\n", __FUNCTION__);
		return -1;		
	}

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	if (ARK_DISP_LAYER_IS_OSD(id)) {
		ark_disp_set_osd_format(id, ARK_DISP_VIDEO_PIXFMT_YUYV, arg->rgb_order,
				arg->yuyv_order);

		ark_disp_set_osd_source_size(id, arg->out_width, arg->out_height); 
		ark_disp_set_osd_layer_size(id, arg->out_width, arg->out_height);
		ark_disp_set_osd_layer_position(id, arg->out_x, arg->out_y);
	} else {
		id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id);
		if(id == 0){
			ark_disp_set_video_format(id, ARK_DISP_VIDEO_PIXFMT_YUYV, arg->rgb_order,
					arg->yuyv_order, 1);
			
			ark_disp_set_video_source_size(id, arg->out_width, arg->out_height);
			ark_disp_set_video_win_size(id, arg->out_width, arg->out_height);
			ark_disp_set_video_layer_size(id, arg->out_width, arg->out_height);
			ark_disp_set_video_layer_position(id, arg->out_x, arg->out_y);
		}
	}

	disp->layer_info.src_xres = arg->width;
	disp->layer_info.src_yres = arg->height;
	disp->layer_info.xres = arg->out_width;
	disp->layer_info.yres = arg->out_height;
	disp->layer_info.xpos = arg->out_x;
	disp->layer_info.ypos = arg->out_y;
	disp->layer_info.crop_xres = arg->win_width;
	disp->layer_info.crop_yres = arg->win_height;
	disp->layer_info.crop_xpos = arg->win_x;
	disp->layer_info.crop_ypos = arg->win_y;
	disp->layer_info.format = ARK_DISP_VIDEO_PIXFMT_YUYV ;//arg->format;
	disp->layer_info.rgb_order = arg->rgb_order;
	disp->layer_info.yuyv_order = arg->yuyv_order;

	disp_writeback->layer_info.src_xres = arg->width;
	disp_writeback->layer_info.src_yres = arg->height;
	disp_writeback->layer_info.xres = arg->out_width;
	disp_writeback->layer_info.yres = arg->out_height;
	disp_writeback->layer_info.xpos = arg->out_x;
	disp_writeback->layer_info.ypos = arg->out_y;
	disp_writeback->layer_info.crop_xres = arg->win_width;
	disp_writeback->layer_info.crop_yres = arg->win_height;
	disp_writeback->layer_info.crop_xpos = arg->win_x;
	disp_writeback->layer_info.crop_ypos = arg->win_y;
	disp_writeback->layer_info.format = arg->format;
	disp_writeback->layer_info.rgb_order = arg->rgb_order;
	disp_writeback->layer_info.yuyv_order = arg->yuyv_order;
	
	return 0;
}


int ark_video2_write_back(struct ark_disp_obj_info *disp, unsigned int src_phy_addr_y, 
	                      unsigned int src_phy_addr_cbcr, unsigned int src_phy_addr_cr,
	                      unsigned int dest_phy_addr)
{
	int id = disp->id;
	struct ark_disp_context *context = container_of(disp, struct ark_disp_context, disp_obj[id]);
	struct ark_disp_obj_info *disp_writeback = &context->disp_obj[4];
	
		// third 3 para 0 disable scaler, 1 enable scaler, when use video2 write back, it must be disable by nan 2016-07-20 
		ark_disp_set_video_format(1, disp_writeback->layer_info.format, 
			disp_writeback->layer_info.rgb_order, disp_writeback->layer_info.yuyv_order, 0); 

		ark_disp_set_video_source_size(1, disp_writeback->layer_info.src_xres, disp_writeback->layer_info.src_yres);

		ark_disp_set_video_win_size(1, disp_writeback->layer_info.crop_xres, disp_writeback->layer_info.crop_yres);

		ark_disp_set_video_layer_size(1, disp_writeback->layer_info.xres, disp_writeback->layer_info.yres);

		ark_disp_set_video_layer_position(1, disp_writeback->layer_info.xpos, disp_writeback->layer_info.ypos);

		ark_disp_set_video_data_addr(1, disp_writeback->layer_info.format, disp_writeback->layer_info.src_xres, 
			disp_writeback->layer_info.src_yres, src_phy_addr_y, src_phy_addr_cbcr, src_phy_addr_cr);

		ark_disp_set_video_scal(1, disp_writeback->layer_info.crop_xres, disp_writeback->layer_info.crop_yres, 
				0, 0, 0, 0, disp_writeback->layer_info.xres, disp_writeback->layer_info.yres, 0, 0);

		ark_disp_set_video2_writeback_en(1, disp_writeback->layer_info.xres, disp_writeback->layer_info.yres, dest_phy_addr);

	return 0;
}

int ark_fb_set_color_key(struct ark_disp_obj_info *disp, 
		struct arkfb_color_key *arg)
{
	int id = disp->id;

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	if (ARK_DISP_LAYER_IS_OSD(id)) {
		ark_disp_set_osd_colorkey(id, arg->en, (arg->color_key >> 16) & 0xFF, 
				(arg->color_key >> 8) & 0xFF, arg->color_key & 0xFF);

		ark_disp_set_osd_colorkey_thld(id, (arg->thld >> 16) & 0xFF, 
				(arg->thld >> 8) & 0xFF, arg->thld & 0xFF);
	} else {
		id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id);

		ark_disp_set_video_colorkey(id, arg->en, (arg->color_key >> 16) & 0xFF, 
				(arg->color_key >> 8) & 0xFF, arg->color_key & 0xFF);

		ark_disp_set_video_colorkey_thld(id, (arg->thld >> 16) & 0xFF, 
				(arg->thld >> 8) & 0xFF, arg->thld & 0xFF);
	}

	disp->layer_info.colorkey_y = (arg->color_key >> 16) & 0xFF;
	disp->layer_info.colorkey_cb = (arg->color_key >> 8) & 0xFF;
	disp->layer_info.colorkey_cr = (arg->color_key >> 0) & 0xFF;

	disp->layer_info.colorkey_y_thld = (arg->thld >> 16) & 0xFF;
	disp->layer_info.colorkey_cb_thld = (arg->thld >> 8) & 0xFF;
	disp->layer_info.colorkey_cr_thld = (arg->thld >> 0) & 0xFF;

	disp->layer_info.colorkey_enable = arg->en;

	return 0;
}

void ark_fb_get_color_key(struct ark_disp_obj_info *disp, 
		struct arkfb_color_key *arg)
{
	arg->color_key = \
			 disp->layer_info.colorkey_y << 16 \
			 | disp->layer_info.colorkey_cb << 8 \
			 | disp->layer_info.colorkey_cr;

	arg->thld = \
		    disp->layer_info.colorkey_y_thld << 16 \
		    | disp->layer_info.colorkey_cb_thld << 8 \
		    | disp->layer_info.colorkey_cr_thld;

	arg->en = disp->layer_info.colorkey_enable;

}


int ark_fb_set_blend(struct ark_disp_obj_info *disp, 
		struct arkfb_blend *arg)
{
	int id = disp->id;

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

	if (ARK_DISP_LAYER_IS_OSD(id)) {
		ark_disp_set_osd_alpha_blend_en_lcd(id, arg->en);

		ark_disp_set_osd_per_pix_alpha_blend_en_lcd(id, arg->pixel_alpha_en);

		ark_disp_set_osd_blend_mode_lcd(id, arg->mode);
	} else {
		id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id);

		ark_disp_set_video_alpha_blend_en_lcd(id, arg->en);

		ark_disp_set_video_per_pix_alpha_blend_en_lcd(id, arg->pixel_alpha_en);

		ark_disp_set_video_blend_mode_lcd(id, arg->mode);
	}

	disp->layer_info.blend_mode = arg->mode;
	disp->layer_info.alpha_blend_en = arg->en;
	disp->layer_info.per_pix_alpha_blend_en = arg->pixel_alpha_en;

	return 0;
}

void ark_fb_get_blend(struct ark_disp_obj_info *disp, 
		struct arkfb_blend *arg)
{
	arg->mode = disp->layer_info.blend_mode;
	arg->en = disp->layer_info.alpha_blend_en;
	arg->pixel_alpha_en = disp->layer_info.per_pix_alpha_blend_en;
}

int ark_fb_set_video_window_addr(struct ark_disp_obj_info *disp, 
		struct arkfb_window_addr *arg)
{
	unsigned long flags;
	int id = disp->id;
	struct list_head *list = NULL;
	struct ark_disp_slot_buf_ctrl *sbc = NULL;
	struct ark_disp_context *context = container_of(disp, struct ark_disp_context, disp_obj[id]);
	struct ark_disp_obj_info *disp_writeback = &context->disp_obj[4];
	if(!context){
		printk("error: %s null\n", __func__);
		return -EFAULT;
	}
	//if(disp->buf_info.indep_disp_list_entry_count > ARK_VIDEO_MAX_FRAME){
	//	disp->need_wait = 1;
	//	wait_event_interruptible_timeout(disp->waitq, disp->need_wait == 0, msecs_to_jiffies(1000));
	//}
	
	//if(ark_die_debug)
	//printk("5");

	if(list_empty(&context->free_list)){
		set_bit(SBUF_STATUS, &disp->need_wait);
		if(!wait_event_interruptible_timeout(disp->waitq, !test_bit(SBUF_STATUS, &disp->need_wait), msecs_to_jiffies(500))){
			printk("display context framebuffer free list: timeout\n");
			return -EFAULT;
		}
	}
		spin_lock_irqsave(&context->lock, flags);
		list = context->free_list.next;
		sbc = list_entry(list, struct ark_disp_slot_buf_ctrl, list);
		list_del(list);
		spin_unlock_irqrestore(&context->lock, flags);
#if 0
		printk("c:%d,sp:0x%x, ap:0x%x, aw:%d, ah:%d, aow:%d, aoh%d, aww:%d, awh:%d\n", 
			disp->buf_info.indep_disp_list_entry_count, \
			sbc->buf_base_phys, arg->phy_addr, \
			disp->layer_info.src_xres, disp->layer_info.src_yres, \
			disp->layer_info.xres, disp->layer_info.yres,\
			disp->layer_info.crop_xres,disp->layer_info.crop_yres);
#endif	
		//printk("6");
		if (test_bit(WRITE_BACK, &disp_writeback->need_wait)) {
			if (!wait_event_interruptible_timeout(disp_writeback->waitq, !test_bit(WRITE_BACK, &disp_writeback->need_wait), msecs_to_jiffies(200))){
				printk("ark_video2_write_back: timeout1\n");
				spin_lock_irqsave(&context->lock, flags);
				if(disp_writeback->write_list)
					list_add_tail(disp_writeback->write_list, &context->free_list);
				spin_unlock_irqrestore(&context->lock, flags);				
				//return -EFAULT;
			}			
		}
		disp_writeback->write_list = list;
		set_bit(WRITE_BACK, &disp->need_wait);
		set_bit(WRITE_BACK, &disp_writeback->need_wait);
		//printk("s 0x%x, 0x%x\n", arg->phy_addr, sbc->buf_base_phys);
		ark_video2_write_back(disp_writeback, arg->phy_addr, arg->phy_cbaddr,
		                      arg->phy_craddr, sbc->buf_base_phys);		
		if (arg->wait_vsync) {
			if (test_bit(WRITE_BACK, &disp_writeback->need_wait)) {
				if (!wait_event_interruptible_timeout(disp_writeback->waitq, !test_bit(WRITE_BACK, &disp_writeback->need_wait), msecs_to_jiffies(200))){
					printk("ark_video2_write_back: timeout2\n");
					spin_lock_irqsave(&context->lock, flags);
					if(disp_writeback->write_list)
						list_add_tail(disp_writeback->write_list, &context->free_list);
					disp_writeback->write_list = NULL;
					spin_unlock_irqrestore(&context->lock, flags);					
					//return -EFAULT;
				}			
			}
		}
#if 0	
	if (ARK_DISP_LAYER_IS_OSD(id)) {
		ark_disp_set_osd_data_addr(id, arg->phy_addr);
	} else {
		id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id);		
		ark_disp_set_video_data_addr(id, disp->layer_info.format, 
				disp->layer_info.src_xres, disp->layer_info.src_yres, arg->phy_addr, 
				arg->phy_cbaddr, arg->phy_craddr);
	}
#endif	
	return 0;
}

int ark_fb_set_window_addr(struct ark_disp_obj_info *disp,
		struct arkfb_window_addr *arg)
{
	unsigned long flags;
	unsigned int posin, posout;
	struct ark_disp_buf_info *buf_info = &disp->buf_info;
	
	if(buf_info->no_writeback_posin != buf_info->no_writeback_posout)
	//printk("in:%d, out:%d\n", buf_info->no_writeback_posin, buf_info->no_writeback_posout);
	
	if((buf_info->no_writeback_posin - buf_info->no_writeback_posout) == ARK_VIDEO_MAX_FRAME){
		set_bit(LAYER_BYPASS, &disp->need_wait);
		if(!wait_event_interruptible_timeout(disp->waitq, !test_bit(LAYER_BYPASS, &disp->need_wait), msecs_to_jiffies(500))){
			printk("set window fb addr: timeout\n");
			return -EFAULT;
		}
	}
	
	spin_lock_irqsave(&disp->lock, flags);
	posin = buf_info->no_writeback_posin & (ARK_VIDEO_MAX_FRAME -1);
	posout = buf_info->no_writeback_posout & (ARK_VIDEO_MAX_FRAME - 1);
	memcpy(&buf_info->no_writeback_phys_addr[posin], arg, sizeof(struct arkfb_window_addr));
	buf_info->no_writeback_posin++;
	spin_unlock_irqrestore(&disp->lock, flags);
#if 0	
	if (ARK_DISP_LAYER_IS_OSD(id)) {
		disp->buf_info.no_writeback_phys_addr
		ark_disp_set_osd_data_addr(
				ARK_DISP_GET_OSD_ID_FROM_LAYER_ID(id),
				arg->phy_addr);
	} else {
		ark_disp_set_video_data_addr(
				ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id),
				disp->layer_info.format,//context->disp_obj[id].layer_info.pixfmt, // format
				disp->layer_info.xres,//context->disp_obj[id].layer_info.src_xres, // width
				disp->layer_info.yres,//context->disp_obj[id].layer_info.src_yres, // height
				arg->phy_addr, 0, 0);
	}
#endif
	return 0;
}

int ark_fb_show_window(struct ark_disp_obj_info *disp)
{
	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);
	int id = disp->id;
	struct ark_disp_layer_info *layer = &disp->layer_info;

	if (ARK_DISP_LAYER_IS_OSD(id)) {
		if (layer->screen == ARKDISP_SCREEN_TVENC)
			ark_disp_set_osd_en_tvenc(id, 1);
		else 
			ark_disp_set_osd_en_lcd(id, 1);
	} else {
		id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id);
		if (layer->screen == ARKDISP_SCREEN_TVENC)
			ark_disp_set_video_en_tvenc(id, 1);
		else
			ark_disp_set_video_en_lcd(id, 1);
	}

	return 0;
}

int ark_fb_hide_window(struct ark_disp_obj_info *disp)
{

	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);
	int id = disp->id;
	struct ark_disp_layer_info *layer = &disp->layer_info;

	if (ARK_DISP_LAYER_IS_OSD(id)) {
		if (layer->screen == ARKDISP_SCREEN_TVENC)
			ark_disp_set_osd_en_tvenc(id, 0);
		else 
			ark_disp_set_osd_en_lcd(id, 0);
	} else {
		id = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(id);
		if (layer->screen == ARKDISP_SCREEN_TVENC)
			ark_disp_set_video_en_tvenc(id, 0);
		else
			ark_disp_set_video_en_lcd(id, 0);
	}

	//printk("id = %ld\n", id);

	return 0;
}
