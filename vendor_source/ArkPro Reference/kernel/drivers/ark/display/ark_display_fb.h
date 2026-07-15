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
 *      ark_display_fb.h
 *
 * Description:
 *
 * 
 * Author:
 *      Sim
 *
 * Remarks:
 *
 */

#ifndef __ARK_DISPLAY_FB_H__
#define __ARK_DISPLAY_FB_H__

#include <linux/fb.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include "ark_display.h"

struct arkfb_update_window {
	__u32 win_x, win_y;
	__u32 win_width, win_height;
	__u32 width, height;
	__u32 format;
	__u32 rgb_order;
	__u32 yuyv_order;
	__u32 out_x, out_y;
	__u32 out_width, out_height;
	__u32 interlace_out;
	__u32 show_tv;
};

struct arkfb_color_key {
	__u32  en;
	__u32  color_key; //y:23-16bit, cb:15:8bit, cr:7:0bit
	__u32  thld; //y:23-16bit, cb:15:8bit, cr:7:0bit
};

struct arkfb_blend {
	__u32  en;
	__u32  pixel_alpha_en;
	__u32  mode;
};

struct ark_osd_priority{
	__u8  osd1;
	__u8  osd2;
	__u8  osd3;
	__u8  video_pri;
	__u8  video2_pri;
};

struct ark_osd_alpha{
	__u8 id;
	__u8 alpha;
};

struct ark_osd_cfg_arg {
    __u32 layer_id;
    __u32 hue;
    __u32 saturation;
    __u32 brightness;
    __u32 contrast;
};

/* IOCTL commands. */

#define ARK_IOW(num, dtype)		_IOW('O', num, dtype)
#define ARK_IOR(num, dtype)		_IOR('O', num, dtype)
#define ARK_IOWR(num, dtype)	_IOWR('O', num, dtype)
#define ARK_IO(num)				_IO('O', num)

#define ARKFB_WAITFORVSYNC		ARK_IO(38)
#define ARKFB_UPDATE_WINDOW		ARK_IOW(39, struct arkfb_update_window)
#define ARKFB_SET_COLOR_KEY		ARK_IOW(40, struct arkfb_color_key)
#define ARKFB_GET_COLOR_KEY		ARK_IOR(45, struct arkfb_color_key)
#define ARKFB_SET_BLEND	    	ARK_IOW(41, struct arkfb_blend)
#define ARKFB_GET_BLEND	    	ARK_IOR(46, struct arkfb_blend)
#define ARKFB_SET_WINDOW_ADDR	ARK_IOW(42, struct arkfb_window_addr)
#define ARKFB_SHOW_WINDOW	    ARK_IO(43)
#define ARKFB_HIDE_WINDOW	    ARK_IO(44)
#define ARKFB_SET_PRIORITY      ARK_IOW(47, struct ark_osd_priority)
#define ARKFB_ENABLE 				ARK_IO(48)
#define ARKFB_SET_OSD_ALPHA	ARK_IOW(49, struct ark_osd_alpha)
#define ARKFB_GET_OSD_CFG      ARK_IOWR(51, struct ark_osd_cfg_arg)
#define ARKFB_SET_OSD_CFG      ARK_IOW(52, struct ark_osd_cfg_arg)
#define ARKFB_SET_WINDOW_POINT		ARK_IOW(53, struct arkfb_update_window)
#define ARKFB_GET_WINDOW_ADDR	ARK_IOR(54, unsigned long)
#define ARKFB_UPDATE_VIDEO_WINDOW ARK_IOW(55, struct arkfb_update_window)
#define ARKFB_SET_VIDEO_WINDOW_ADDR ARK_IOW(56, struct arkfb_window_addr)

int ark_fb_update_window(struct ark_disp_obj_info *disp, 
                           struct arkfb_update_window *arg);

int ark_video_update_window(struct ark_disp_obj_info *disp, 
                           struct arkfb_update_window *arg);

int ark_fb_set_color_key(struct ark_disp_obj_info *disp, 
                           struct arkfb_color_key *arg);

void ark_fb_get_color_key(struct ark_disp_obj_info *disp, 
                           struct arkfb_color_key *arg);

int ark_fb_set_blend(struct ark_disp_obj_info *disp, 
                           struct arkfb_blend *arg);

void ark_fb_get_blend(struct ark_disp_obj_info *disp, 
                           struct arkfb_blend *arg);

int ark_fb_set_window_addr(struct ark_disp_obj_info *disp, 
                           struct arkfb_window_addr *arg);

int ark_fb_set_video_window_addr(struct ark_disp_obj_info *disp, 
                           struct arkfb_window_addr *arg);

int ark_fb_show_window(struct ark_disp_obj_info *disp);

int ark_fb_hide_window(struct ark_disp_obj_info *disp);

#endif
