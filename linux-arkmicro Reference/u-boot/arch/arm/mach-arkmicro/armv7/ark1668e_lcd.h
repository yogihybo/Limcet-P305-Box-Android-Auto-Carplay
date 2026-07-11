#ifndef	ARK_LCD_H
#define	ARK_LCD_H

#include <linux/types.h>
#include <common.h>
#include "ark1668e_hardware.h"
#include "ark1668e_sys.h"

//yuv format
#define Y_U_V               0
#define Y_UV                1
//window format 
#define SEQ_Y_UV422			20		//Y_UV422 format for video layer
#define SEQ_Y_UV420			21		//Y_UV420 format for video layer
#define SEQ_YUV422			0		//Y_U_V422 format for video layer
#define PALETTE				0		//palette for osd layer
#define SEQ_YUV420			1		//Y_U_V420 format for video layer
#define YUV422				2
#define YUV444				3
#define RGB_I555			4
#define RGB_565				5
#define RGB_a888			6
#define RGB_888				7
#define RGB_a1555			8
#define RGB_a1888			9
#define RGB_a4888			10
#define RGB_666				11
#define RGB_a1666			12

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

enum mirror_type {
	MIRROR_TYPE_NONE,
	MIRROR_TYPE_L2R,//mirror left to right 
	MIRROR_TYPE_U2D,//mirror up to down 
	MIRROR_TYPE_L2R_U2D,//mirror left to right and  up to down 
	MIRROR_TYPE_END,
};

void ark_disp_wait_lcd_frame_int(void);
void ark_disp_wait_tvenc_frame_int(void);
void ark_set_osd_image(enum DISP_OSD_LAYER_ID  layer_id,int format, int width, int height);
void ark_set_osd_addr(enum DISP_OSD_LAYER_ID  layer_id, unsigned int addr);
void ark_disp_set_osd_layer_position(int id, int x, int y);
void ark_osd_en_layer(int id, unsigned int enable);

#endif
