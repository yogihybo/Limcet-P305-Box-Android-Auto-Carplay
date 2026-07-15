#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#ifdef __cplusplus
extern "C" {
#endif

/* DVR IOCTL commands */
#define ARK_DVR_IOC_MAGIC  'n'

#define ARK_DVR_START			_IO(ARK_DVR_IOC_MAGIC,  0x10)
#define ARK_DVR_STOP			_IO(ARK_DVR_IOC_MAGIC,  0x20)
#define ARK_DVR_GETFRAME		_IOWR(ARK_DVR_IOC_MAGIC,  0x30, struct frame_info)
#define ARK_DVR_SWITCH_CHANNEL	_IOWR(ARK_DVR_IOC_MAGIC,  0x40, unsigned long)
#define ARK_DVR_CHANNEL_STATUS	_IOWR(ARK_DVR_IOC_MAGIC,  0x50, unsigned long)
#define ARK_DVR_INIT			_IOWR(ARK_DVR_IOC_MAGIC,  0x60, struct itu656in_para)
#define ARK_DVR_TVOUT			_IOWR(ARK_DVR_IOC_MAGIC,  0x70, unsigned long)
#define ARK_DVR_DETECT_SIGNAL	_IOWR(ARK_DVR_IOC_MAGIC,  0x80, unsigned long)

/* DISPLAY IOCTL commands */
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

#define ARKDISP_IOCTL_BASE		0xa0

#define ARKDISP_NO_OPERATION     _IOW(ARKDISP_IOCTL_BASE, 0, unsigned long)
#define ARKDISP_GET_VDE_CFG      _IOWR(ARKDISP_IOCTL_BASE, 1, unsigned long)
#define ARKDISP_SET_VDE_CFG      _IOW(ARKDISP_IOCTL_BASE, 2, unsigned long)
#define ARKDISP_GET_LAYER_CFG    _IOWR(ARKDISP_IOCTL_BASE, 3, unsigned long)
#define ARKDISP_SET_LAYER_CFG    _IOW(ARKDISP_IOCTL_BASE, 4, unsigned long)
#define ARKDISP_GET_INPUT_CFG    _IOWR(ARKDISP_IOCTL_BASE, 5, unsigned long)
#define ARKDISP_SET_INPUT_CFG    _IOWR(ARKDISP_IOCTL_BASE, 6, unsigned long)
#define ARKDISP_GET_LCD_CFG      _IOWR(ARKDISP_IOCTL_BASE, 7, unsigned long)
#define ARKDISP_SET_LCD_CFG      _IOW(ARKDISP_IOCTL_BASE, 8, unsigned long)
#define ARKDISP_GET_TVENC_CFG    _IOWR(ARKDISP_IOCTL_BASE, 9, unsigned long)
#define ARKDISP_SET_TVENC_CFG    _IOW(ARKDISP_IOCTL_BASE, 10, unsigned long)
#define ARKDISP_SET_LAYER_ON     _IOW(ARKDISP_IOCTL_BASE, 11, unsigned long)
#define ARKDISP_SET_LAYER_OFF    _IOW(ARKDISP_IOCTL_BASE, 12, unsigned long)
#define ARKDISP_SET_ITU656IN_ON  _IOW(ARKDISP_IOCTL_BASE, 13, unsigned long)
#define ARKDISP_SET_ITU656IN_OFF _IOW(ARKDISP_IOCTL_BASE, 14, unsigned long)
#define ARKDISP_SET_PRESCAL_ON   _IOW(ARKDISP_IOCTL_BASE, 15, unsigned long)
#define ARKDISP_SET_PRESCAL_OFF  _IOW(ARKDISP_IOCTL_BASE, 16, unsigned long)
#define ARKDISP_GET_FB_CFG       _IOW(ARKDISP_IOCTL_BASE, 17, unsigned long)
#define ARKDISP_SET_FB_CFG       _IOW(ARKDISP_IOCTL_BASE, 18, unsigned long)
#define ARKDISP_GET_V4L2_CFG     _IOW(ARKDISP_IOCTL_BASE, 19, unsigned long)
#define ARKDISP_SET_V4L2_CFG     _IOW(ARKDISP_IOCTL_BASE, 20, unsigned long)
#define ARKDISP_RESET            _IOW(ARKDISP_IOCTL_BASE, 21, unsigned long)
#define ARKDISP_GET_STATUS       _IOW(ARKDISP_IOCTL_BASE, 22, unsigned long)
#define ARKDISP_GET_TCON_CTRL    _IOW(ARKDISP_IOCTL_BASE, 23, unsigned long)
#define ARKDISP_SET_TCON_CTRL    _IOW(ARKDISP_IOCTL_BASE, 24, unsigned long)
#define ARKDISP_GET_BACKCAR_STATUS _IOR(ARKDISP_IOCTL_BASE, 25, unsigned long)
#define ARKDISP_SET_GUI_TVOUT	 _IOW(ARKDISP_IOCTL_BASE, 26, unsigned long)


/* DISPLAY format */
#define ARK_DISP_VIDEO_PIXFMT_YUV422   0
#define ARK_DISP_VIDEO_PIXFMT_YUV420   1
#define ARK_DISP_VIDEO_PIXFMT_YUYV     2
#define ARK_DISP_VIDEO_PIXFMT_YUV      3
#define ARK_DISP_VIDEO_PIXFMT_RGBI555  4
#define ARK_DISP_VIDEO_PIXFMT_RGB565   5
#define ARK_DISP_VIDEO_PIXFMT_RGBA888  6
#define ARK_DISP_VIDEO_PIXFMT_RGB888   7
#define ARK_DISP_VIDEO_PIXFMT_RGBA1555 8
#define ARK_DISP_VIDEO_PIXFMT_RGBA1888 9
#define ARK_DISP_VIDEO_PIXFMT_RGBA4888 10
#define ARK_DISP_VIDEO_PIXFMT_RGB666   11
#define ARK_DISP_VIDEO_PIXFMT_RGBA1666 12
#define ARK_DISP_VIDEO_PIXFMT_Y_UV422  (ARK_DISP_VIDEO_PIXFMT_YUV422 | 0x10)
#define ARK_DISP_VIDEO_PIXFMT_Y_UV420  (ARK_DISP_VIDEO_PIXFMT_YUV420 | 0x10)

#define ARK_DISP_OSD_NUM         3
#define ARK_DISP_VID_NUM         2

#define ARK_DISP_LAYER_NUM       (ARK_DISP_OSD_NUM + ARK_DISP_VID_NUM)

struct itu656in_para{
	int	xpos;
	int	ypos;
	int	width;
	int	height;
	int source;
	int tvout;
	int	hori_mirror;
	int	vert_mirror;
	int	left_blank;
	int right_blank;
	int	top_blank;
	int	bottom_blank;
};

enum dvr_source {
	DVR_SOURCE_CAMERA,
	DVR_SOURCE_AUX,
};

struct arkfb_update_window {
	unsigned int win_x, win_y;
	unsigned int win_width, win_height;
	unsigned int width, height;
	unsigned int format;
	unsigned int rgb_order;
	unsigned int yuyv_order;
	unsigned int out_x, out_y;
	unsigned int out_width, out_height;
	unsigned int interlace_out;
	unsigned int show_tv;
};


struct arkfb_window_addr {
	unsigned int  phy_addr;
	unsigned int  phy_cbaddr;
	unsigned int  phy_craddr;
	unsigned int  wait_vsync;
};

struct ark_disp_tvenc_cfg_arg {
    unsigned int enable;
    unsigned int out_mode; /* enum ark_disp_tvenc_out_mode */
    unsigned int backcolor_y;
    unsigned int backcolor_cb;
    unsigned int backcolor_cr;
};

struct ark_disp_vde_cfg_arg {
    unsigned int layer_id;
    unsigned int hue;
    unsigned int saturation;
    unsigned int brightness;
    unsigned int contrast;
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

enum DISP_LAYER_ID
{
	OSD1_LAYER,
	OSD2_LAYER,
	OSD3_LAYER,
	VIDEO_LAYER,
	VIDEO2_LAYER,
};

#ifdef __cplusplus
}
#endif

#endif
