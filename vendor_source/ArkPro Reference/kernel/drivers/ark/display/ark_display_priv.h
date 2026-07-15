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
 *      ark_display_priv.h
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

#ifndef __ARK_DISPLAY_PRIV_H__
#define __ARK_DISPLAY_PRIV_H__

/*************************************************************************
 * Definitions and macros
 *************************************************************************/

#define RED_Y   0x50
#define RED_U   0x5a
#define RED_V   0xEF

#define GREEN_Y 0x90
#define GREEN_U 0x36
#define GREEN_V 0x22

#define BLUE_Y  0x28
#define BLUE_U  0xEF
#define BLUE_V  0x6E

#define BLACK_Y 0x10
#define BLACK_U 0x80
#define BLACK_V 0x80

#define ARK_DISP_LCD_WIDTH      800		
#define ARK_DISP_LCD_HEIGHT     480		
#define ARK_DISP_LCD_PIXEL_NUM  (ARK_DISP_LCD_WIDTH*ARK_DISP_LCD_HEIGHT)


#define YPBPR_SRC_CLK_SYSPLL    1
#define YPBPR_SRC_CLK_24M       2
#define YPBPR_SRC_CLK_DDS       4
#define YPBPR_SRC_CLK_CPUPLL    8

#define CVBS_SRC_CLK_SYSPLL     1
#define CVBS_SRC_CLK_24M        2
#define CVBS_SRC_CLK_DDS        4
#define CVBS_SRC_CLK_CPUPLL     8

#define VGA_SRC_CLK_SYSPLL      1
#define VGA_SRC_CLK_24M         2
#define VGA_SRC_CLK_DDS         4
#define VGA_SRC_CLK_CPUPLL      8

#define ITU656_SRC_CLK_SYSPLL   1
#define TU656_SRC_CLK_24M       2
#define TU656_SRC_CLK_DDS       4
#define TU656_SRC_CLK_CPUPLL    8

#define OUT_13_5M               0 
#define OUT_27M                 1				 
#define OUT_74_25M              2				 
#define OUT_148_5M              3

enum DISP_OSD_LAYER_ID
{
	OSD1_LAYER,
	OSD2_LAYER,
	OSD3_LAYER,
};

enum DISP_VIDEO_LAYER_ID
{
	VIDEO_LAYER,
	VIDEO2_LAYER,
};

enum DISP_OSD_FORMAT
{
    DISP_PALETTE = 0,
    DISP_BMP24BIT,
    DISP_YUV422 = 0x2,
    DISP_YUV422_VYUY = 0x2,
    DISP_YUV422_UYVY = 0x12,
	DISP_YUV422_YVYU = 0x22,
	DISP_YUV422_YUYV = 0x32,
    DISP_YUV444 = 3,
    DISP_RGB_I555,
    DISP_RGB_565,
    DISP_RGB_a888,
    DISP_RGB_888 = 0x7,
    DISP_RBG_888 = 0x17,
    DISP_GRB_888 = 0x27,
    DISP_GBR_888 = 0x37,
    DISP_BRG_888 = 0x47,
    DISP_BGR_888 = 0x57,
    DISP_RGB_a1555 = 8,
    DISP_RGB_a1888,
    DISP_RGB_a4888,
    DISP_RGB_666,
    DISP_RGB_a1666
};

enum DISP_VIDEO_FORMAT
{
	DISP_SEQ_YUV422 = 0,
    DISP_SEQ_Y_U_V422 = 0,
	DISP_SEQ_Y_UV422 = 0x10,
    DISP_SEQ_YUV420 = 1,
    DISP_SEQ_Y_U_V420 = 0x1,
    DISP_SEQ_Y_UV420 = 0x11,
};

#define DispGetYUVFormat(format) ((format) & 0xF)
#define DispGetYUVOrder(format) (((format) & 0xF0) >> 4)

enum screen_type_id {
	SCREEN_TYPE_RGB,
	SCREEN_TYPE_LVDS,
	SCREEN_TYPE_ITU601,
	SCREEN_TYPE_TVENC,
	SCREEN_TYPE_CVBS,
	SCREEN_TYPE_VGA,
	SCREEN_TYPE_YPBPR,
	SCREEN_TYPE_ITU656,
	SCREEN_TYPE_NONE = -1,
};

enum rgb_format_id {
	RGB_FORMAT_RGB0888,
	RGB_FORMAT_RGB565,
	RGB_FORMAT_NONE = -1,
};

enum lvds_format_id {
	LVDS_FORMAT_RGB0888,
	LVDS_FORMAT_RGB565,
	LVDS_FORMAT_NONE = -1,
};

enum cvbs_format_id {
	CVBS_FORMAT_PAL,
	CVBS_FORMAT_NTSC,
	CVBS_FORMAT_NONE = -1,
};

enum vga_format_id {
	VGA_FORMAT_800x480,
	VGA_FORMAT_800x600,
	VGA_FORMAT_NONE = -1,
};

enum ypbpr_format_id {
	YPBPR_FORMAT_480I,
	YPBPR_FORMAT_576I,
	YPBPR_FORMAT_480P,
	YPBPR_FORMAT_576P,
	YPBPR_FORMAT_720P60HZ,
	YPBPR_FORMAT_720P50HZ,
	YPBPR_FORMAT_1080I60HZ,
	YPBPR_FORMAT_1080I50HZ,
	YPBPR_FORMAT_1080I50HZ_1250,
	YPBPR_FORMAT_1080P60HZ,
	YPBPR_FORMAT_1080P50HZ,
	YPBPR_FORMAT_NONE = -1,
};

enum screen_id {
	SCREEN_QUN700,
	SCREEN_CVBS_NTSC,
	SCREEN_CVBS_PAL,
	SCREEN_VGA8060,
	SCREEN_TYPE_YPBPR720P,
	SCREEN_C101EAN,
	SCREEN_CLAA101,
	SCREEN_GM8284DD,
	SCREEN_MAX_NUM,
};

struct screen_info
{
	int screen_id;
    enum screen_type_id screen_type;
   	int format;
    unsigned int width;
    unsigned int height;
    unsigned int vbp;
    unsigned int vfp;
    unsigned int vsw;
    unsigned int hbp;
    unsigned int hfp;
    unsigned int hsw;
    unsigned int vclk_active;
    unsigned int hsync_active;
    unsigned int vsync_active;
    unsigned int de_active;
	unsigned int tvenc;
	unsigned int tvout_format;
};

/*************************************************************************
 * Functions
 *************************************************************************/
/* core functions */
struct ark_disp_context *ark_get_disp_context (void);
void ark_disp_set_next_buf_start_addr(int id, unsigned int addr_phys);
int ark_disp_get_vde_cfg(struct ark_disp_vde_cfg_arg *arg);
int ark_disp_set_vde_cfg(const struct ark_disp_vde_cfg_arg *arg);
int ark_disp_get_layer_cfg(struct ark_disp_layer_cfg_arg *arg);
int ark_disp_set_layer_en(int layer_id, int enable);
int ark_disp_set_layer_cfg(const struct ark_disp_layer_cfg_arg *arg);
int ark_disp_try_layer_cfg(struct ark_disp_layer_cfg_arg *arg);
int ark_disp_get_lcd_cfg(struct ark_disp_lcd_cfg_arg *arg);
int ark_disp_set_lcd_cfg(struct ark_disp_lcd_cfg_arg *arg);
int ark_disp_get_tvenc_cfg(struct ark_disp_tvenc_cfg_arg *arg);
int ark_disp_set_tvenc_cfg(struct ark_disp_tvenc_cfg_arg *arg);
int ark_disp_get_in_cfg(struct ark_disp_in_cfg_arg *arg);
int ark_disp_set_in_cfg(struct ark_disp_in_cfg_arg *arg);
int ark_disp_set_itu656in_en(int enable);
int ark_disp_set_itu656in_status(int status);
irqreturn_t ark_disp_intr_handler(int irq, void *dev_id);
irqreturn_t ark_disp_scal_intr_handler(int irq, void *dev_id);
irqreturn_t ark_backcar_intr_handler(int irq, void *dev_id);
void ark_disp_enable_interrupts(void);
void ark_disp_disable_interrupts(void);
int ark_disp_wait_frame_sync_interrupt(struct ark_disp_obj_info *disp);
int ark_disp_dev_init(struct ark_disp_context *context);
int ark_disp_reg_check(struct ark_disp_context *context);
int ark_disp_set_video_stream_on(int layer_id);
int ark_disp_copy_layer_info_to_cfg_arg(int layer_id,
    struct ark_disp_layer_cfg_arg *arg);
int ark_disp_set_layer_priority_lcd(
    int osd1_pri,int osd2_pri,int osd3_pri, int video_pri, int video2_pri);
int ark_disp_set_layer_priority_tvenc(
    int osd1_pri,int osd2_pri,int osd3_pri, int video_pri, int video2_pri);
extern unsigned int ark_lcd_mmio_base;
//extern unsigned int ark_gpio_mmio_base;
int ark_disp_flush_wait_list(struct ark_disp_obj_info *disp);


/* osd layer control functions */
void ark_disp_set_osd_clk_sel(int id, unsigned int sel);
unsigned int ark_disp_get_osd_en_lcd(int id);
void ark_disp_set_osd_en_lcd(int id, unsigned int enable);
unsigned int ark_disp_get_osd_en_tvenc(int id);
void ark_disp_set_osd_en_tvenc(int id, unsigned int enable);
void ark_disp_set_osd_format(int id, unsigned int format,
    unsigned int rgb_order, unsigned int yuyv_order);
void ark_disp_set_osd_source_size(int id, int width, int height);
void ark_disp_set_osd_layer_size(int id, int width, int height);
void ark_disp_set_osd_layer_position(int id, int x, int y);
void ark_disp_set_osd_win_point(int id, int x, int y);
void ark_disp_set_osd_alpha(int id, unsigned char alpha);
void ark_disp_set_osd_alpha_blend_en_lcd(int id, int enable);
void ark_disp_set_osd_alpha_blend_en_tvenc(int id, int enable);
void ark_disp_set_osd_per_pix_alpha_blend_en_lcd(int id, int enable);
void ark_disp_set_osd_per_pix_alpha_blend_en_tvenc(int id, int enable);
void ark_disp_set_osd_blend_mode_lcd(int id, unsigned int mode);
void ark_disp_set_osd_blend_mode_tvenc(int id, unsigned int mode);
unsigned int ark_disp_get_osd_hue(int id);
unsigned int ark_disp_get_osd_saturation(int id);
unsigned int ark_disp_get_osd_brightness(int id);
unsigned int ark_disp_get_osd_contrast(int id);
void ark_disp_set_osd_hue(int id, unsigned int hue);
void ark_disp_set_osd_saturation(int id, unsigned int saturation);
void ark_disp_set_osd_brightness(int id, unsigned int brightness);
void ark_disp_set_osd_contrast(int id, unsigned int contrast);
void ark_disp_set_osd_colorkey(int id, int enable, unsigned int y,
    unsigned int cb, unsigned int cr);
void ark_disp_set_osd_colorkey_thld(int id, unsigned int y_thld,
    unsigned int cb_thld, unsigned int cr_thld);
void ark_disp_set_osd_priority_lcd(int id, int level);
void ark_disp_set_osd_priority_tvenc(int id, int level);
void ark_disp_set_osd_layer_cut(int id, unsigned int left,
    unsigned int right, unsigned int up, unsigned int down);
void ark_disp_set_osd_palette(int palette_color_num);
void ark_disp_set_osd_data_addr(int id, unsigned int addr);
void ark_disp_set_osd_data_addr_cvbs(int id, unsigned int format,
    unsigned int width, unsigned int height, unsigned int addr);
void ark_disp_set_osd_double_buffer_en(int id, int enable);
void ark_disp_set_osd_frame_mode(int id, int frame);
void ark_disp_set_osd_frame_invert(int id, int invert);

/* video layer control functions */
int ark_disp_get_video_en_lcd(int id);
void ark_disp_set_video_en_lcd(int id, int enable);
int ark_disp_get_video_en_tvenc(int id);
void ark_disp_set_video_en_tvenc(int id, int enable);
void ark_disp_set_video_format(int id, unsigned int format,
    unsigned int rgb_order, unsigned int yuyv_order, unsigned int scal_bypass);
void ark_disp_set_video_source_size(int id, int width, int height);
void ark_disp_set_video_layer_size(int id, int width, int height);
void ark_disp_set_video_layer_position(int id, int x, int y);
void ark_disp_set_video_win_size(int id, int width, int height);
void ark_disp_set_video_win_point(int id, int x, int y);
void ark_disp_set_video_alpha(int id, unsigned char alpha);
void ark_disp_set_video_alpha_blend_en_lcd(int id, int enable);
void ark_disp_set_video_alpha_blend_en_tvenc(int id, int enable);
void ark_disp_set_video_per_pix_alpha_blend_en_lcd(int id, int enable);
void ark_disp_set_video_per_pix_alpha_blend_en_tvenc(int id, int enable);
void ark_disp_set_video_blend_mode_lcd(int id, unsigned int mode);
void ark_disp_set_video_blend_mode_tvenc(int id, unsigned int mode);
unsigned int ark_disp_get_video_hue(int id);
unsigned int ark_disp_get_video_saturation(int id);
unsigned int ark_disp_get_video_brightness(int id);
unsigned int ark_disp_get_video_contrast(int id);
void ark_disp_set_video_hue(int id, unsigned int hue);
void ark_disp_set_video_saturation(int id, unsigned int saturation);
void ark_disp_set_video_brightness(int id, unsigned int brightness);
void ark_disp_set_video_contrast(int id, unsigned int contrast);
void ark_disp_set_video_clk_sel(int id, unsigned int sel);
void ark_disp_set_video_colorkey(int id, int enable, unsigned int y,
    unsigned int cb, unsigned int cr);
void ark_disp_set_video_colorkey_thld(int id, unsigned int y_thld,
    unsigned int cb_thld, unsigned int cr_thld);
void ark_disp_set_video_priority_lcd(int id, int level);
void ark_disp_set_video_priority_tvenc(int id, int level);
void ark_disp_set_video_scal(int id, unsigned int win_width,
    unsigned int win_height, unsigned int left_blank,
    unsigned int right_blank, unsigned int top_blank,
    unsigned int bottom_blank, unsigned int dst_width,
    unsigned int dst_height, int interlace_out_en, int interlace_read_en);
void ark_disp_set_video2_writeback_en(int enable, unsigned int dst_width,
    unsigned int dst_height, unsigned int writeback_buf_addr);
void ark_disp_set_video_layer_cut(int id, unsigned int left,
    unsigned int right, unsigned int up, unsigned int down);
void ark_disp_set_video_data_addr(int id, unsigned int format,
    unsigned int width, unsigned int height, unsigned int yaddr,
    unsigned int cbaddr, unsigned int craddr);
void ark_disp_set_video_data_addr_cvbs(int id,  unsigned int format,
    unsigned int width, unsigned int height, unsigned int addr);
void ark_disp_set_video_double_buffer_en(int id, int enable);
void ark_disp_set_video_frame_mode(int id, int frame);
void ark_disp_set_video_frame_invert(int id, int invert);

/* lcd screen control functions */
void ark_disp_config_gamma(void);
void ark_disp_config_D_tcon_800x480(void);
int ark_disp_set_lcd_panel_type(int type);
void ark_disp_set_lcd_en(int enable);
void ark_disp_set_lcd_backcolor(unsigned char y, unsigned char cb,
    unsigned char cr);

/* tvenc screen control functions */
void ark_disp_set_tvenc_backcolor(unsigned char y, unsigned char cb,
    unsigned char cr);
void ark_disp_set_tvenc_en(int mode, int enable);
int ark_disp_set_tvenc_out_mode(int out_mode);

/* frame buffer control functions */
int ark_disp_fb_init(struct ark_disp_context *context);
int ark_disp_sync_other_cfg_to_fb_info(int layer_id,
    struct ark_disp_context *context);

/* v4l2 driver control functions */
int ark_disp_v4l2_init(struct ark_disp_context *context);
int ark_disp_v4l2_release(struct ark_disp_context *context);
int ark_disp_convert_layer_cfg_to_v4l2_info(int layer_id,
    struct ark_disp_context *context);

/* tv encoder control functions */
void ark_disp_config_tvenc_interlace_out(int interlace);
int ark_disp_set_gui_tvout(int enable);

#endif //#ifndef __ARK_DISPLAY_PRIV_H__
