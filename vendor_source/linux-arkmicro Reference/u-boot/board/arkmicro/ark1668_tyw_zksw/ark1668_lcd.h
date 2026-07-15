#ifndef	ARK_LCD_H
#define	ARK_LCD_H

#include <linux/types.h>

#if !APP_RUN_PROCESS
#include <common.h>
#include <asm/global_data.h>
#include "ark1668_hardware.h"
#include "ark1668_sys.h"
#endif

#define RED_Y	0x50
#define RED_U	0x5a
#define RED_V	0xEF

#define GREEN_Y	0x90
#define GREEN_U	0x36
#define GREEN_V 0x22

#define BLUE_Y	0x28
#define BLUE_U	0xEF
#define BLUE_V  0x6E

#define BLACK_Y	0x10
#define BLACK_U	0x80
#define BLACK_V	0x80

#define HICOLOR_565		0
#define HICOLOR_X555	1
#define HICOLOR_I555	2
#define HICOLOR_4444	3

#define LVDS_W				1366  
#define LVDS_H				768  	
#define LVDS_PIXEL			(LVDS_W * LVDS_H)
#define LVDS_HSW			32
#define LVDS_HBP			80
#define LVDS_HFP			48
#define LVDS_VFP			3
#define LVDS_VSW			5
#define LVDS_VBP			14
#define LVDS_IOE			0
#define LVDS_IHS 			1
#define LVDS_IVS			1

#define LCD_W				800		
#define LCD_H				480		
#define LCD_PIXEL			(LCD_W * LCD_H)

//#define HV_MODE

#ifdef   HV_MODE
#define HSW					0x15
#define HBP					0x54
#define HFP					0x15

#define VFP					0x18
#define VSW					0x50
#define VBP					0x20

#else // DE_MODE
#define HSW					0x52//82
#define HBP					0x2f//64
#define HFP					0x40//64

#define VFP					0x0//36
#define VSW					0x16//16//20
#define VBP					0x16//40
#endif

#define IOE					0
#define IHS 				0
#define IVS					0

#define CONTRAST			0x50
#define BRIGHTNESS			0x80
#define HUE					0
#define SATURATION			0x40

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
//screen type
#define PARALLEL_24			0
#define PARALLEL_18			1
#define PARALLEL_16			2
#define RGB_DUMMY			3
#define CCIR_656			4
#define YUV_PANEL			5
#define CPU_PANEL			6
#define RGB_NORMAL			7
//SCALER CUT BOUNDARY
#define LEFT_BLANK        	0
#define RIGHT_BLANK      	0
#define TOP_BLANK          	0
#define BOTTOM_BLANK   		0
//LCD ROTATE CONTROL
#define ROTATE_0            0
#define ROTATE_90           1 
#define ROTATE_180          2 
#define ROTATE_270          3
#define HORIZON_MIRROR      4
#define VERTICAL_MIRROR     5

#define AUTO_FILTER			1
#define MANUL_FILTER        0

#define ENABLE_H_FILTER		1
#define DISABLE_H_FILTER	0

#define MOVE_STEP			8
#define EFFECT_NUM			2

#define LCD_WAIT_FRAME		20

//#define POST_SCALER		0
#define DEINTERLACE        	0
#define ITU656_WRITEBACK 	0

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
	
	VGA_FORMAT_1024x768HZ60,
	VGA_FORMAT_1280x1024HZ60,
	VGA_FORMAT_1900x1200HZ60,
	VGA_FORMAT_1280x1024HZ75,
	VGA_FORMAT_1280x960HZ85,
	VGA_FORMAT_1280x720HZ60,
	VGA_FORMAT_640x480HZ60,
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

enum ark_disp_tvenc_out_mode {
    ARKDISP_TVENC_OUT_YPBPR_I480HZ60       = 0,
    ARKDISP_TVENC_OUT_YPBPR_I576HZ50       = 1,
    ARKDISP_TVENC_OUT_YPBPR_P480HZ60       = 2,
    ARKDISP_TVENC_OUT_YPBPR_P576HZ50       = 3,
    ARKDISP_TVENC_OUT_YPBPR_P720HZ60       = 4,
    ARKDISP_TVENC_OUT_YPBPR_P720HZ50       = 5,
    ARKDISP_TVENC_OUT_YPBPR_I1080HZ60      = 6,
    ARKDISP_TVENC_OUT_YPBPR_I1080HZ50      = 7,
    ARKDISP_TVENC_OUT_YPBPR_I1080HZ50_1250 = 8,
    ARKDISP_TVENC_OUT_YPBPR_P1080HZ60      = 9,
    ARKDISP_TVENC_OUT_YPBPR_P1080HZ50      = 10,
    ARKDISP_TVENC_OUT_CVBS_PAL             = 11,
    ARKDISP_TVENC_OUT_CVBS_NTSC            = 12,
    ARKDISP_TVENC_OUT_ITU656_PAL           = 13,
    ARKDISP_TVENC_OUT_ITU656_NTSC          = 14,
    ARKDISP_TVENC_OUT_VGA_640x480HZ60      = 15,
    ARKDISP_TVENC_OUT_VGA_800x600HZ60      = 16,
    ARKDISP_TVENC_OUT_VGA_1024x768HZ60     = 17,
    ARKDISP_TVENC_OUT_VGA_1280x1024HZ60    = 18,
    ARKDISP_TVENC_OUT_VGA_1900x1200HZ60    = 19, // bandwidth  limit
    ARKDISP_TVENC_OUT_VGA_1280x1024HZ75    = 20,
    ARKDISP_TVENC_OUT_VGA_1280x960HZ85     = 21, // bandwidth  limit
    ARKDISP_TVENC_OUT_VGA_1280x720HZ60     = 22,
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
	SCREEN_USER_TYPE,
	SCREEN_MAX_NUM,
};
enum clk_source_id {
	SCREEN_CLKSEL_CPUPLL,
	SCREEN_CLKSEL_SYSPLL,
	SCREEN_CLKSEL_DDSCLK,
	SCREEN_CLKSEL_24MCLK = 4,
	SCREEN_CLKSEL_NONE = -1,
};
enum rgb_mode_id {
	RGB_MODE_BGR,
	RGB_MODE_GBR,
	RGB_MODE_RBG,
	RGB_MODE_BRG,
	RGB_MODE_GRB,
	RGB_MODE_RGB,
	RGB_MODE_NUM,
};

enum ui_scaler_type_id {
	UI_SCALER_NONE,//normal mode: no caler ,cannt set posx posy
	UI_POSITION_CVBS,//no scaler, can set posx posy throught arkdata.ini	
	UI_SCALER_VGA, //scaler mode, can set posx posy throught arkdata.ini
	UI_SCALER_CVBS, //scaler mode, can set posx posy throught arkdata.ini
	UI_SCALER_END,
};

enum mirror_type {
	MIRROR_TYPE_NONE,
	MIRROR_TYPE_L2R,//mirror left to right 
	MIRROR_TYPE_U2D,//mirror up to down 
	MIRROR_TYPE_L2R_U2D,//mirror left to right and  up to down 
	MIRROR_TYPE_END,
};

#define   ARK_DISPLAY_ALL_MODE         0

#if ARK_DISPLAY_ALL_MODE
typedef struct _VP_INFO
{
	unsigned int video_contrast;
	unsigned int video_brightness;
	unsigned int video_saturation;
	unsigned int video_hue;

	unsigned int video2_contrast;
	unsigned int video2_brightness;
	unsigned int video2_saturation;
	unsigned int video2_hue;

	unsigned int osd1_contrast;
	unsigned int osd1_brightness;
	unsigned int osd1_saturation;
	unsigned int osd1_hue;

	unsigned int osd2_contrast;
	unsigned int osd2_brightness;
	unsigned int osd2_saturation;
	unsigned int osd2_hue;

	unsigned int osd3_contrast;
	unsigned int osd3_brightness;
	unsigned int osd3_saturation;
	unsigned int osd3_hue;

}vp_info;

typedef struct _tGammaInfo
{
	unsigned int gamma_en;
	unsigned int gamma_regval[48];

} gamma_info;
#endif

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

        unsigned int clk_source;
        unsigned int clk_freq;
        unsigned int clk_div1;
        unsigned int clk_div2;

        unsigned int src_format;
        unsigned int rgb_mode;
        unsigned int bpp;
        unsigned int lvds_cfg;
        unsigned int interlace;

        unsigned int frame_rate;
        unsigned int src_width;
        unsigned int src_height;
        unsigned int pad_unset;
};

#if ARK_DISPLAY_ALL_MODE

typedef struct _itu656in
{	
	unsigned int ModeControl;
	unsigned int VGATE_DELAY;
	unsigned int DEN_V_STOP;
	unsigned int DEN_V_START;

	unsigned int NTSC_TVGDEL;
	unsigned int NTSC_TVSYNC;
	unsigned int NTSC_THGDEL;
	unsigned int NTSC_THSYNC;

	unsigned int NTSC_THLEN;
	unsigned int NTSC_THGATE;

	unsigned int NTSC_TVLEN;
	unsigned int NTSC_TVGATE;

	unsigned int NTSC_VFZ;
	unsigned int NTSC_HFZ;

	unsigned int NTSC_SYNC_UP_CNT;
	unsigned int NTSC_SYNC_DOWN_CNT;
	unsigned int NTSC_DATENA_INV;
	unsigned int NTSC_VSYNC_INV;
	unsigned int NTSC_HSYNC_INV;
	unsigned int NTSC_FIELD_INV;
	unsigned int NTSC_HV_DELAY;

	unsigned int PAL_TVGDEL;
	unsigned int PAL_TVSYNC;
	unsigned int PAL_THGDEL;
	unsigned int PAL_THSYNC;

	unsigned int PAL_THLEN;
	unsigned int PAL_THGATE;

	unsigned int PAL_TVLEN;
	unsigned int PAL_TVGATE;

	unsigned int PAL_VFZ;
	unsigned int PAL_HFZ;

	unsigned int PAL_SYNC_UP_CNT;
	unsigned int PAL_SYNC_DOWN_CNT;
	unsigned int PAL_DATENA_INV;
	unsigned int PAL_VSYNC_INV;
	unsigned int PAL_HSYNC_INV;
	unsigned int PAL_FIELD_INV;
	unsigned int PAL_HV_DELAY;
	
}itu656byp_info;
typedef struct _special_info
{	
	unsigned int dithering;
	unsigned int video_hfz;
	unsigned int video_vfz;
	unsigned int backlight;//backlight 环境变量中获取
	unsigned int carback_mask;	//bit0=1:carback_brightness active,bit0=0:carback_brightness not active; ... ; bit5=1:carback_sharpness active,bit5=0:carback_sharpness not active
	int carback_brightness;
	int carback_contrast;
	int carback_saturation;
	int carback_hue;
	int carback_sharpness;
	unsigned int usb_update_delay;//usb update delay(unit: s)环境变量中获取
	unsigned int disp_xpos;
	unsigned int disp_ypos;
	unsigned int disp_width;
	unsigned int disp_height;
	unsigned int ui_scaler_type;
	unsigned int track_setting;
	unsigned int usr_data7;
	unsigned int usr_data8;
	unsigned int usr_data9;

}special_info;

#define MAX_TOUCHKEY		8
struct touchkey {
	int left;
	int top;
	int right;
	int bottom;
	int value;
};

typedef struct{
	int origin;
	int panelx_min;
	int panelx_max;
	int panely_min;
	int panely_max;
	int touchkey_num;
	struct touchkey key[MAX_TOUCHKEY];
}touchscreen_info;
#endif
typedef  struct _display_updatepara
{
	unsigned char flag_id[16];
	struct screen_info screeninfo;
#if ARK_DISPLAY_ALL_MODE
	unsigned int  flag_vpinfo;
	unsigned int  flag_gamma;
	vp_info		  vpinfo;
	gamma_info    gammainfo;
	itu656byp_info itu656bypinfo;
	special_info   spec_info;
	touchscreen_info touch_info;
#endif
}display_updatepara;


#define SCREEN_MODULE_ID	4


#define DEBUG_DISPLAY  1
#define IS_TVENC_SCREEN(x) ((x)->screen_type > SCREEN_TYPE_TVENC)



void ark_disp_wait_lcd_frame_int(void);
void ark_disp_wait_tvenc_frame_int(void);
int is_interlace_tvenc(struct screen_info *screen);
void ark_set_osd_image(enum DISP_OSD_LAYER_ID  layer_id,int format, int width, int height);
void ark_set_osd_addr(enum DISP_OSD_LAYER_ID  layer_id, unsigned int addr);
void ark_disp_set_osd_layer_position(int id, int x, int y);
void ark_backlight_config_f(int screen_id);
void ark_display_initialize_port(struct screen_info *screen);
void ark_osd_en_layer(int id, unsigned int enable);
void ark_set_osd_frame_mode(int id, int frame);
void ark_backlight_config(int screen_id);


#endif
