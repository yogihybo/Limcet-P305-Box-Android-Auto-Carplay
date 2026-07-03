#ifndef __ARK_API_H__
#define __ARK_API_H__

#include "display.h"

#ifdef __cplusplus
extern "C" {
#endif

/*********VIDEO API***********/
int arkapi_open_video_fb(void);

int arkapi_open_video_fb_timeout(int seconds);

void arkapi_close_video_fb(int fd);

int arkapi_gui_tvout(int enable);

int arkapi_init_fb_display(int fb_fd, int src_width, int src_height, 
						   int out_x, int out_y, int out_width, int out_height, 
						   int cut_left, int cut_right, int cut_top, int cut_bottom,
						   int format, int interlace_out, int show_tv);

int arkapi_init_fb_video_display(int fb_fd, int src_width, int src_height, 
							     int out_x, int out_y, int out_width, int out_height, 
							     int cut_left, int cut_right, int cut_top, int cut_bottom,
							     int format, int interlace_out, int show_tv);

int arkapi_set_fb_addr(int fb_fd, unsigned int phy_yaddr, unsigned int phy_cbcraddr, 
                       unsigned int phy_craddr, int wait_sync);

int arkapi_show_fb(int fb_fd, int show);

int arkapi_init_tvenc(int out_mode, int enable);

int arkapi_get_screen_size(int *width, int *height);

/*********DVR API***********/
int arkapi_open_dvr(void);

int arkapi_close_dvr(int dvr_fd);

int arkapi_dvr_config(int dvr_fd, enum dvr_source source, int width, int height, int tvout);

int arkapi_dvr_start(int dvr_fd);

int arkapi_dvr_stop(int dvr_fd);

int arkapi_dvr_tvout(int dvr_fd, int enable);

int arkapi_dvr_detect_signal(int dvr_fd);

int arkapi_get_layer_hue(unsigned int layer_id);

int arkapi_set_layer_hue(unsigned int layer_id, unsigned int hue);

int arkapi_get_layer_saturation(unsigned int layer_id);

int arkapi_set_layer_saturation(unsigned int layer_id, unsigned int saturation);

int arkapi_get_layer_brightness(unsigned int layer_id);

int arkapi_set_layer_brightness(unsigned int layer_id, unsigned int brightness);

int arkapi_get_layer_contrast(unsigned int layer_id);

int arkapi_set_layer_contrast(unsigned int layer_id, unsigned int contrast);

#ifdef __cplusplus
}
#endif

#endif
