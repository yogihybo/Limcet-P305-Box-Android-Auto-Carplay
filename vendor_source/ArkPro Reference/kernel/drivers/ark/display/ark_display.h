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
 *      ark_display.h
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

#ifndef __ARK_DISPLAY_H__
#define __ARK_DISPLAY_H__

#include <linux/gpio.h>

/* uncomment the line below to use default device major number */
//#define ARK_DISP_USE_DEV_MAJOR_DEFAULT

/* uncomment the line below to enable proc entry */
#define ARK_DISP_USE_PROC_DIR_ENTRY

/* uncomment the line below to use platform resource info to do init */
#define ARK_DISP_USE_PLATFORM_RES

/* uncomment the line below to reserves video buffers in advance */
//#define ARK_DISP_ALLOC_VIDEO_BUFFERS_IN_ADVANCE

/* uncomment the line below to use proprietary framebuffer io function */
//#define ARK_DISP_USE_PROPRIETARY_FBIO

/* uncomment the line below to use common video buffer pool */
//#define ARK_DISP_USE_COMMON_VIDEO_BUFFER_POOL

/* uncomment the line below to enable small frame buffer mode */
//#define ARK_DISP_USE_SMALL_FRAME_BUFFER

/* chitest: use ec803 video memory pool */
#define ARK_DISP_HW_VMEM_BASE               (0x00000000 + 0x380000 * 18)
#define ARK_DISP_HW_VMEM_IO_BASE            (0xB0000000 + 0x380000 * 18)
#define ARK_DISP_VIDEO_BUFFER_SIZE_DEFAULT  0x380000 //11bLSB should be 0


/*************************************************************************
 * Driver information definitions
 *************************************************************************/
#define ARK_DISP_MAJOR_RELEASE              0
#define ARK_DISP_MINOR_RELEASE              0
#define ARK_DISP_BUILD                      1
#define ARK_DISP_VERSION_CODE ((ARK_DISP_MAJOR_RELEASE << 16) | \
    (ARK_DISP_MINOR_RELEASE << 8) | ARK_DISP_BUILD)

//#define ARK1680_V4L2

//#define ARK_DISP_DBG
/*************************************************************************
 * Debug print functions
 *************************************************************************/
#ifdef ARK_DISP_DBG
#define ARKDISP_DBGPRTK(...) printk(KERN_ALERT __VA_ARGS__)
#else
#define ARKDISP_DBGPRTK(...)
#endif


/*************************************************************************
 * register definitions
 *************************************************************************/
#define ARK_DISP_REG_BASE    LCD_BASE
#define ARK_DISP_REG_SIZE    0x800

#define ARK_SYS_REG_BASE     SYS_BASE
#define ARK_SYS_REG_SIZE     0x200

#define ARK_GPIO_REG_BASE    GPIO_BASE
#define ARK_GPIO_REG_SIZE    0x60

/*************************************************************************
 * API definitions
 *************************************************************************/
/* Application calls this ioctl for setting the C-plane buffer offset
 * relative to the start of Y-plane buffer address
 */
#define VIDIOC_S_COFST	_IOWR('V', 200, int)

#define ARK_VIDEO_MAX_FRAME   4

/*************************************************************************
 * driver control structures
 *************************************************************************/

//chitest TBD
#define ARK_DISP_LCD_AXI_CMD_ID_WR      6
#define ARK_DISP_LCD_AXI_CMD_ID_RD      5
#define ARK_VIDEO_MAX_FRAME 4
/* identity of framebuffer allocated for osd1 */
#define ARK_DISP_OSD1_FB_ID     0

enum {
	SBUF_STATUS = 1,
	WRITE_BACK,
	LAYER_BYPASS,
};

struct arkfb_window_addr {
	__u32  phy_addr;
	__u32  phy_cbaddr;
	__u32  phy_craddr;
	__u32  wait_vsync;
};

struct ark_disp_slot_buf_ctrl {
    slot_buf_t slot_buf;
    struct videobuf_buffer *frm;
    unsigned int in_xres; 
    unsigned int in_yres;
    //unsigned int zoom_xres; 
   // unsigned int zoom_yres;
    //unsigned int zoom_xstart;
    //unsigned int zoom_ystart;
    unsigned int out_xres;
    unsigned int out_yres;
    unsigned int buf_base_virt;
    unsigned int buf_base_phys;
    unsigned int buf_size;
    bool big_endian;
    bool in_interlace;
    bool out_interlace;
    bool ctrl;
    struct list_head list;
};

struct ark_disp_vid_buf_ctrl {
    unsigned int buf_addr_virt;
    unsigned int buf_addr_phys;
    unsigned int buf_size;
    unsigned int in_xres; 
    unsigned int in_yres;
    unsigned int line_length;
    bool big_endian;
    bool in_interlace;
    struct list_head list;
};

/* buffer specific parameters */
struct ark_disp_buf_info {
    unsigned int min_buf_num;
    unsigned int buf_num;
    unsigned int min_buf_size;
    unsigned int buf_size;
    unsigned int buf_pool_size;
    unsigned int buf_pool_base_virt;
    unsigned int buf_pool_base_phys;
    struct arkfb_window_addr  no_writeback_phys_addr[ARK_VIDEO_MAX_FRAME]; //these addr is passed by app
    unsigned int no_writeback_posin;
    unsigned int no_writeback_posout;
	
    struct list_head disp_list; // intr handler displays buffers queued in
                                // disp_list first, if no entry found, it
                                // display the frame buffer addressed by
                                // cur_buf_addr
    unsigned int disp_list_entry_count;

    struct list_head wait_list; // buffers sent to deinterlacer are queued
                                // in wait_list, it will be removed from
                                // the list after it has been processed by
                                // deinterlacer
    unsigned int wait_list_entry_count;

    struct list_head done_list; // buffers that are displayed or processed by
                                // deinterlacer will be added to done list,
                                // they will be returned to their source
    unsigned int done_list_entry_count;

    struct list_head indep_disp_list; //display independ video frame

    unsigned int indep_disp_list_entry_count;

    unsigned int prescal_buf_addr_virt[2];  // buffer stores prescaler output
    unsigned int prescal_buf_addr_phys[2];
    int prescal_disp_buf_ind;      // index of buffer displayed on screen

    /* NOTE: include/linux/videodev2.h:#define VIDEO_MAX_FRAME       32 */

    unsigned int cur_buf_addr_virt;  /* address of current frame buffer */
    unsigned int cur_buf_addr_phys;  /* address of current frame buffer */
    unsigned int cbcr_offset;        /* C-Plane offset from start of y-plane */
};


/* NOTE: the default values below are obtained from hardware datasheet for
 * both video and osd layers
 */
#define ARK_DISP_VDE_HUE_DEFAULT            0x00
#define ARK_DISP_VDE_SATURATION_DEFAULT     0x40
#define ARK_DISP_VDE_BRIGHTNESS_DEFAULT     0x70 //0x80
#define ARK_DISP_VDE_CONTRAST_DEFAULT       0x60 //0x50

#define ARK_DISP_OSD_PIXFMT_PALETTE    0
#define ARK_DISP_OSD_PIXFMT_BMP24BIT   1
#define ARK_DISP_OSD_PIXFMT_YUYV       2
#define ARK_DISP_OSD_PIXFMT_YUV        3
#define ARK_DISP_OSD_PIXFMT_RGBI555    4
#define ARK_DISP_OSD_PIXFMT_RGB565     5
#define ARK_DISP_OSD_PIXFMT_RGBA888    6
#define ARK_DISP_OSD_PIXFMT_RGB888     7
#define ARK_DISP_OSD_PIXFMT_RGBA1555   8
#define ARK_DISP_OSD_PIXFMT_RGBA1888   9
#define ARK_DISP_OSD_PIXFMT_RGBA4888   10
#define ARK_DISP_OSD_PIXFMT_RGB666     11
#define ARK_DISP_OSD_PIXFMT_RGBA1666   12

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

#define ARK_DISP_BLDMODE_WHOLE_BLEND                        0
#define ARK_DISP_BLDMODE_WHOLE_OVERWRT                      1
#define ARK_DISP_BLDMODE_KEYCOL_TRANS_ABS_OTHER_OVERWRT     2
#define ARK_DISP_BLDMODE_KEYCOL_TRANS_ABS_OTHER_TRANS       3
#define ARK_DISP_BLDMODE_KEYCOL_OVERWRT_OTHER_TRANS         4
#define ARK_DISP_BLDMODE_KEYCOL_OVERWRT_OTHER_TRANS_ABS     5
#define ARK_DISP_BLDMODE_KEYCOL_TRANS_OTHER_OVERWRT         6
#define ARK_DISP_BLDMODE_KEYCOL_TRANS_OTHER_TRANS_ABS       7
#define ARK_DISP_BLDMODE_WHOLE_BLEND_ALPHA                  8
#define ARK_DISP_BLDMODE_WHOLE_OVERWRT_ALPHA                9
#define ARK_DISP_BLDMODE_KEYCOL_TRANS_OTHER_OVERWRT_ALPHA   10
#define ARK_DISP_BLDMODE_KEYCOL_TRANS_OTHER_BLEND           11
#define ARK_DISP_BLDMODE_KEYCOL_OVERWRT_ALPHA_OTHER_BLEND   12
#define ARK_DISP_BLDMODE_KEYCOL_OVERWRT_ALPHA_OTHER_TRANS   13
#define ARK_DISP_BLDMODE_KEYCOL_BLEND_OTHER_OVERWRT_ALPHA   14
#define ARK_DISP_BLDMODE_KEYCOL_BLEND_OTHER_TRANS           15

#define ARK_DISP_LEFT_CUT_NUM      0
#define ARK_DISP_RIGHT_CUT_NUM     0
#define ARK_DISP_TOP_CUT_NUM       0
#define ARK_DISP_BOTTOM_CUT_NUM    0

#define BACKCAR_GPIO	GPIO5
#define BACKLIGHT_GPIO	GPIO81

#define LED_GPIO	GPIO72
#define LCDL2RSCANPAD GPIO73

/* display layer specific parameters */
struct ark_disp_layer_info {
    bool enable;   /* enable window for display */
    bool interlaced;
    bool alpha_blend_en;
    bool per_pix_alpha_blend_en;
    enum ark_disp_layer_type  type;
    enum ark_disp_screen_type screen;
    enum ark_disp_pix_format  pixfmt;
    unsigned int src_xres;  /* x resolution of input frame in pixels */
    unsigned int src_yres;  /* y resolution of input frame in lines */
    unsigned int xres;      /* x resolution of output window in pixels */
    unsigned int yres;      /* y resolution of output window in lines */
    unsigned int xpos;      /* x pos of output window's upper left corner */
    unsigned int ypos;      /* y pos of output window's upper left corner */
    unsigned int crop_xres; /* x resolution of crop window in pixels */
    unsigned int crop_yres; /* y resolution of crop window in lines */
    unsigned int crop_xpos; /* x pos of crop window's upper left corner */
    unsigned int crop_ypos; /* y pos of crop window's upper left corner */
    unsigned int line_length;
    unsigned char blend_mode;
    unsigned char alpha;    /* alpha blending value */
    unsigned char hue;
    unsigned char saturation;
    unsigned char brightness;
    unsigned char contrast;
    bool colorkey_enable;
    unsigned char colorkey_y;
    unsigned char colorkey_cb;
    unsigned char colorkey_cr;
    unsigned char colorkey_y_thld;
    unsigned char colorkey_cb_thld;
    unsigned char colorkey_cr_thld;
    unsigned char priority;
    unsigned int left_cut_num;
    unsigned int right_cut_num;
    unsigned int top_cut_num;
    unsigned int bottom_cut_num;
    enum ark_disp_rgb_order_mode rgb_order;
    enum ark_disp_yuyv_order_mode yuyv_order;

    /* used/updated by driver only */
    unsigned int format;
    unsigned int bits_per_pixel;
};

/* framebuffer specific parameters */
struct ark_disp_fb_info {
    unsigned int pseudo_palette[16]; // palette of 16 colors
    struct fb_info fb_info;
};

/* V4l2 specific parameters */
struct ark_disp_v4l2_info {
    struct videobuf_queue buf_queue;   /* buffer queue used in video-buf */

    struct list_head output_queue;     /* queue of filled frames */
    unsigned int output_queue_entry_count;

    struct videobuf_buffer *cur_frm;   /* pointer to current v4l2_buffer in
                                        * disp list
                                        */
//    struct videobuf_buffer *next_frm;  /* pointer to next v4l2_buffer */
    spinlock_t lock;                   /* used in video-buf */

    struct video_device *video_dev;    /* video device for this channel */
    enum v4l2_memory memory;           /* buffer exchange mechanism in use */
    struct v4l2_prio_state prio;       /* state of the priority */
    struct v4l2_pix_format pix_fmt;	   /* used to store pixel format */

    unsigned int usrs;                 /* number of open instances */
    unsigned int io_usrs;              /* number of users performing IO */
    unsigned int field_id;             /* id of the field being displayed */

    unsigned char started;             /* indicate whether streaming started */

    /* lock used to access this structure */
    struct mutex mutex_lock;

    struct list_head vbuf_list; // buffers allocated or prepared in the 
                                // function ark_disp_v4l2_buffer_prepare()
                                // will be queued here, they will be freed or
                                // returned to free_vbuf_list in the function
                                // ark_disp_v4l2_buffer_release()
    unsigned int vbuf_list_entry_count;

    /* statistics */
    unsigned int out_buf_cnt;
    unsigned int done_buf_cnt;

//    /* function pointer to set the addresses */
//    void (*set_sdram_addr) (u32, u32);
//    /* for this plane, set defaults in vpbe registers */
//    void (*set_win_size)(u32,u32, u32, u32, u32);
//    /* setting interlace or non interlace */r
//    void (*set_interlaced)(unsigned int);
//    /* enable/disable window */
//    void (*set_win_enable)(unsigned int);
};

struct ark_disp_obj_info {
    unsigned int id;    /* display object identifier, it is initialized at
                         * ark_disp_dev_init()
                         */
    spinlock_t lock;
    unsigned long need_wait;
    unsigned long writeback;
	struct list_head *write_list;
    wait_queue_head_t waitq;
    struct ark_disp_buf_info   buf_info;
    struct ark_disp_layer_info layer_info;
    struct ark_disp_fb_info    fbd_info;
    struct ark_disp_v4l2_info  vid_info;

    /* statistics */
    unsigned int disp_frame_cnt;
	int show_tv;
};

struct ark_disp_lcd_cfg_info {
    bool enable;
    spinlock_t lock;
    wait_queue_head_t timing_intr_waitq;
    unsigned int timing_intr_count;
    unsigned int panel_type;
    unsigned char backcolor_y;
    unsigned char backcolor_cb;
    unsigned char backcolor_cr;
};

struct ark_disp_tvenc_cfg_info {
    bool enable;
    spinlock_t lock;
    wait_queue_head_t timing_intr_waitq;
    unsigned int timing_intr_count;
    unsigned int  out_mode;
    unsigned char backcolor_y;
    unsigned char backcolor_cb;
    unsigned char backcolor_cr;

    /* used/updated by driver only */
    bool layer_frame_mode;
};

struct ark_disp_in_cfg_info {
    bool connected;
    int  ringpair_id;
    char ringpair_name[64];
};

struct ark_disp_out_cfg_info {
    bool out_en;
    int  ringpair_id;
    char ringpair_name[64];
    int  ringpair_slots;
    struct list_head rtn_wait_list;
    unsigned int rtn_wait_list_entry_count;
    struct ark_disp_obj_info *src_disp_obj;
};

/* video file handle structure */
struct ark_disp_v4l2_fh {
    struct ark_disp_obj_info *disp_obj; /* pointer to channel object for
                                         * the opened device
                                         */
    unsigned char io_allowed;  /* indicate whether this file handle is doing
                                * IO
                                */
    enum v4l2_priority prio;   /* to keep track priority of this instance */
    unsigned char initialized; /* to keep track of whether this file handle
                                * has initialized encoder or not
                                */
};

#define ARK_DISP_VIDEO_BUF_SIZE   (1920*1080*3/2)

struct ark_disp_context {
    int irq;
    int scal_irq;
    spinlock_t lock;
    wait_queue_head_t waitq;
    struct device *dev;

    struct ark_disp_obj_info disp_obj[ARK_DISP_LAYER_NUM];
    struct ark_disp_lcd_cfg_info   lcd_cfg;
    struct ark_disp_tvenc_cfg_info tvenc_cfg;
    struct ark_disp_in_cfg_info    in_cfg;
    struct ark_disp_out_cfg_info   out_cfg;

    bool deinterlace_en; // only for video
    bool prescale_en; // only for osd1
    bool itu656in_en; // only for video2
    int palette_color_num;

    struct list_head free_list;
    unsigned int free_list_entry_count;
    struct ark_disp_slot_buf_ctrl sbuf_ctrl[ARK_VIDEO_MAX_FRAME];

    struct list_head free_vbuf_list;
    unsigned int free_vbuf_list_entry_count;
    struct ark_disp_vid_buf_ctrl vbuf_ctrl[ARK_VIDEO_MAX_FRAME];

    unsigned int tasklet_run_count;

	int screen_width;
	int screen_height;
	int gui_tvout;
};

struct ark_disp_device {
    struct platform_device *pdev;
    struct miscdevice ark_disp_miscdevice;
    const char *driver_name;
    const char *name;
    struct fasync_struct *async_queue; //set back car signal for app
    struct ark_disp_context context;
};

#endif //#ifndef __ARK_DISPLAY_H__
