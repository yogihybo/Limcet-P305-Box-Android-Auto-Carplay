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
 *      ark_display_video.c
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
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>

#include <linux/videodev2.h>
#include <media/videobuf-core.h>
#include <media/v4l2-common.h>
#include <mach/va_map.h>
#include <linux/ark/ringpair.h>
#include <linux/ark/ark_display_io.h>
#include <mach/hardware.h>

#ifdef ARK_DISP_USE_HW_EMULATION
#include <linux/ecsoc/ec_dcu_io.h>
#endif

#include "ark_display.h"
#include "ark_display_priv.h"
#include "ark_lcd_regs.h"
#include "ark_sys_regs.h"

void ark_disp_set_video_format_2(int id, unsigned int format)
{
	/* definition of video layer src format */
	/*
0: yuv422, plane format
1: yuv420, plane format
2: yuyv, packet format
3: yuv, packet format
4: rgbi555
5: rgb565
6: rgb_a888
7: rgb_888
8: rgba1555
9: rgb_a1888
10: rgb_a4888
11: rgb_666
12: rgb_a1666
	 */
	if (id == 0) {
		/* CLCD_VIDEO_CTL REG0x3c */
		rLCD_VIDEO_CTL &= ~0xF; // clear bit 3-0
		rLCD_VIDEO_CTL |= format; /* 3-0: video layer src format */
	} else if (id == 1) {
		/* CLCD_VIDEO2_CONTROL REG0x320 */
		rLCD_VIDEO2_CTL &= ~0xF; // clear bit 3-0
		rLCD_VIDEO2_CTL |= format; /* 3-0: video layer src format */
	}
}

/* Definition of video layer pixel format in register
0: yuv422, plane format
1: yuv420, plane format
2: yuyv, packet format
3: yuv, packet format
4: rgbi555
5: rgb565
6: rgb_a888
7: rgb_888
8: rgba1555
9: rgb_a1888
10: rgb_a4888
11: rgb_666
12: rgb_a1666
 */
void ark_disp_set_video_format(int id, unsigned int format,
		unsigned int rgb_order, unsigned int yuyv_order, unsigned int scal_bypass)
{
	unsigned int yuv_ycbcr_bypass, rgb_ycbcr_bypass;
	unsigned int y_uv_order;
	unsigned int pixfmt;
	//unsigned int scal_bypass = 1; //enable// disable video scaler by pass mode
	switch (format)
	{
		case ARK_DISP_VIDEO_PIXFMT_RGBI555:
		case ARK_DISP_VIDEO_PIXFMT_RGB565:
		case ARK_DISP_VIDEO_PIXFMT_RGBA888:	
		case ARK_DISP_VIDEO_PIXFMT_RGB888:
		case ARK_DISP_VIDEO_PIXFMT_RGBA1555:
		case ARK_DISP_VIDEO_PIXFMT_RGBA1888:
		case ARK_DISP_VIDEO_PIXFMT_RGBA4888:
		case ARK_DISP_VIDEO_PIXFMT_RGB666:
		case ARK_DISP_VIDEO_PIXFMT_RGBA1666:
			yuv_ycbcr_bypass = 1;
			rgb_ycbcr_bypass = 0;	
			y_uv_order = 0;
			break;
		case ARK_DISP_VIDEO_PIXFMT_Y_UV422:
		case ARK_DISP_VIDEO_PIXFMT_Y_UV420:
			yuv_ycbcr_bypass = 1;
			rgb_ycbcr_bypass = 1;	
			y_uv_order = 1;
			break;	
		case ARK_DISP_VIDEO_PIXFMT_YUYV:
			yuv_ycbcr_bypass = 0;
			rgb_ycbcr_bypass = 0;
			y_uv_order = 0;
			break;
		default:
			yuv_ycbcr_bypass = 1;
			rgb_ycbcr_bypass = 1;
			y_uv_order = 0;
			break;
	}
	pixfmt = format & 0x0F;

	if (id == 0) {
		/* CLCD_VIDEO_CTL REG0x3c */
		rLCD_VIDEO_CTL =
			0<<23 | // 0=disable to get more data bytes on each line
			1<<22 | // 1=select video size specified on CLCD_VIDEO_SIZE 
			// (which is video output size, after scale if need be)
			// if h-scale bypass enabled, h-window size = src hres
			// if v-scale bypass enabled, v-window size = src vres
			y_uv_order<<21 | // 1=Y_UV, 0=Y_U_V
			0<<20 | // 0=double buffer mode disabled
			0<<19 | // 0=use address group 0 for display
			yuyv_order<<17 | // 18-17: 0=yuyv in case YUV422
			rgb_order<<14 | // 16-14: 000=RGB data order or YUV order for YUV444
			0<<11 | // 0=do not keep UV data for next line in case 
			// seq_y_u_v422, seq_y_uv422, seq_y_u_v420, seq_y_uv420
			0<<10 | // 0=use 2-step FIR low-pass filter to do data transfer
			// from yuv422 to yuv444 in case src format is 
			// YUV422, Y_U_V422, Y_UV422, Y_U_V420, Y_UV420
			scal_bypass<<9 |  // 1=enable video scalar by pass mode 
			1<<8 |  // 1=whole scaler is disable when scaler is in bypass mode
			0<<7 |  // 0=field mode for interlace data capture of video 
			0<<6 |  // 0=not invert field 
			yuv_ycbcr_bypass<<5 | // 1=no YUV to YCbCr, 0=YUV to YCbCr 
			rgb_ycbcr_bypass<<4 | // 1=no RGB to YCbCr, 0=RGB to YCbCr
			pixfmt<<0;   // 3-0: video layer source format
		//   0000=yuv422, plane format
		//   0001=yuv420, plane format
		//   0010=yuyv, packet format
		//   0011=yuv, packet format
		//   0100=rgbi555
		//   0101=rgb565
		//   0110=rgb_a888
		//   0111=rgb_888
		//   1000=rgba1555
		//   1001=rgb_a1888
		//   1010=rgb_a4888
		//   1011=rgb_666
		//   1100=rgb_a1666

		if (pixfmt == ARK_DISP_VIDEO_PIXFMT_YUV420)
			rLCD_VIDEO_CTL |= 1<<11;
		// 1=keep UV data for next line in case 
	} else if (id == 1) {
		/* CLCD_VIDEO2_CONTROL REG0x320 */
		rLCD_VIDEO2_CTL =
            //ARK_DISP_LCD_AXI_CMD_ID_WR<<28 | // 31-28: axi write id of video2
			0<<26 | // 0=u/v write first in doing data write back
			// i.e. (uyvyuyvy... or vyuyvyuy...)
			0<<25 | // 0=v first in doing data write back 
			// i.e. vyuyvyuy... or yvyuyvyu..
			//NOTE: as a result, the write back data here is vyuyvyuy...

			0<<24 | // wb_start, set this bit from 0->1 will start write back
			// this bit valid only when write_back_enable_video2=1
			0<<23 | // 0=disable to get more data bytes for each line read
			1<<22 | // 1= select video size on REG0x330 rLCD_VIDEO2_SIZE
			// OR video layer size determined by h-scale, v-scale
			y_uv_order<<21 | // 0=Y_U_V, 1=Y_UV
			0<<20 | // 0=double buffer mode disable
			// if double buffer is enabled, addr group 1 will be
			// displayed firstly
			0<<19 | // 0=use addr group 0 when double buffer mode disable
			yuyv_order<<17 | // 18-17: 00=yuyv when format is YUV422
			rgb_order<<14 | // 16-14: 000=RGB when format is YUV444 or RGB
			0<<13 | // 0=not save UV data for displaying next line
			0<<12 | // 0=use 2-step FIR low-pass filter for processing data
			// xfer from yuv422 to yuv444 when the data format
			// is yuv422, y_u_v422, y_uv422, y_u_v420, y_uv420
			0<<11 | // 0=use axi_clk
			// NOTE: set to 1 (select itu656 clock) when itu656 
			// input is directly via video2 layer
			0<<10 | // 0=?v????? progressive read in doing write back
			scal_bypass<<9 |  // 1=enable scale by pass mode
			1<<8 |  // 1=disable whole scale in video layer when scaler is 
			// in bypass mode
			0<<7 |  // 0=field mode select for interlace data capture of 
			// video
			0<<6 |  // 0=field not invert for frame mode and interlace data
			// capture of video layer
			yuv_ycbcr_bypass<<5 | // 1=no yuv2ycbcr
			rgb_ycbcr_bypass<<4 | // 1=no rgb2ycbcr, 0=do rgb2ycbcr
			pixfmt<<0;   // 3-0: video layer src format
		//   0000=yuv422, plane format
		//   0001=yuv420, plane format
		//   0010=yuyv, packet format
		//   0011=yuv, packet format
		//   0100=rgbi555
		//   0101=rgb565
		//   0110=rgb_a888
		//   0111=rgb_888
		//   1000=rgba1555
		//   1001=rgb_a1888
		//   1010=rgb_a4888
		//   1011=rgb_666
		//   1100=rgb_a1666
		if (pixfmt == ARK_DISP_VIDEO_PIXFMT_YUV420)         
			rLCD_VIDEO2_CTL |= 1<<13;
		// 1=save UV data for displaying next line
	}
}

void ark_disp_set_video_scal(
		int id,
		unsigned int win_width, unsigned int win_height,
		unsigned int left_blank, unsigned int right_blank,
		unsigned int top_blank, unsigned int bottom_blank,
		unsigned int dst_width, unsigned int dst_height,
		int interlace_out_en,
		int interlace_read_en // 1=interlace, 0=progressive
		)
{
	unsigned int vblank = top_blank + bottom_blank;
	unsigned int hblank = left_blank + right_blank;
	int dat_iel_sel = 0, scal_iel_mod_sel = 0;

	if (id == 0) {
		/* CLCD_VIDEO_SCALE_CTL REG0xb0 */
		rLCD_VIDEO_SCALE_CTL =
			0<<11 | // 0=addr update per field
			0<<9 | // 10-9: 00=???j??????A??scaler?]?ݰt?m?j????X?H?s???^
			// read data line in an interlace manner then scale it
			0<<8 | // 0=not line chroma
			1<<7 | // 1=YUV
			0<<6 | // 0=disable horizontal filter (use for down scale)
			1<<5 | // 1=auto set coef of h-filter when down scale

			// NOTE: bits below are used for scaler to do deinterlacing
			0<<4 | // 0=normal scale de-interlace mode
			0<<3 | // 0=current field is field=0
			0<<2 | // 0=field=0 is odd, field=1 is even
			0<<1 | // 0=de-interlace disable
			0<<0;  // 0=use 2 line buffers

		if (dst_width + hblank < win_width)
			rLCD_VIDEO_SCALE_CTL |= 1<<6;
		// enable filter for doing horizontal downscale

		/* When need cut after scaler,the dest size of the scaler must equal
		 * the size you wanted plus the cut number you wanted
		 */

		/* Right_bottom_cut_num REG0x0bc */
		rLCD_VIDEO_RIGHT_BOTTOM_CUT_NUM =
			right_blank<<8 | // 15-8: right cut number in line scaler
			bottom_blank;    // 7-0: bottom cut number in vertical scaler

		/* CLCD_VIDEO_SCAL_CTL0 REG0xb4 */
		rLCD_VIDEO_SCAL_CTL0 =
			left_blank<<18 | // 27-18: left cut number in line scaler
			(win_width * 1024 / (dst_width + hblank));
		// 17-0: ??⤽???O (?????*1024?^/?ت???סA ??????????C
		//       ??F?O??????????q?A?ت??j?p??y?j?_?N?@??
		// hfz = (win_width * 1024) / (dst_width + hblank)

		/* CLCD_VIDEO_SCAL_CTL1 REG0xb8 */
		rLCD_VIDEO_SCAL_CTL1 =
			top_blank<<18 | // 25-18: up cut line number in vertical scaler
			(win_height * 1024 / (dst_height + vblank));
		// 17-0: ??⤽?? ?]??????*1024?^/?ت????סA??????????C
		//       ??F?O??????????q?A?ت??j?p??y?j?_?N?@??
		// vfz = (win_height * 1024) / (dst_height + vblank)
		if (interlace_out_en) {
			/* TV_CONTROL REG0x2b0 */
			rLCD_TV_CONTROL &= ~(1<<8); // 0=non-interlace output mode,
			// reading data from memory in a
			// progressive manner

			/* NOTE: when v scaler cof is 0x400, v scaler bypass, 
			 * now we should change the cof to force v scaler, otherwise
			 * there was sawtooth on picture
			 */
			if ((rLCD_VIDEO_SCAL_CTL1 & 0x3FFFF) == 0x400)
				rLCD_VIDEO_SCAL_CTL1 = rLCD_VIDEO_SCAL_CTL1 - 1;

			/* CLCD_VIDEO_SCALE_CTL REG0xb0 */
			rLCD_VIDEO_SCALE_CTL &= ~(7<<9); // clear bit 11-9
			rLCD_VIDEO_SCALE_CTL |=
				1<<11 | // 1=update addr per frame
				1<<9; // 10-9: 10=read data every line then scale it
		} else {
			/* TV_CONTROL REG0x2b0 */
			//rLCD_TV_CONTROL |= 1<<8; // 1=interlace output mode, reading 
			// data from memory in an interlace
			// manner
		}
	} else if (id == 1) {
		/* CLCD_VIDEO2_SCALE_CTL REG0x354 */
		rLCD_VIDEO2_SCALE_CTL =
			0<<11 | // 0=addr update per field
			0<<9 |  // 10-9: 00=???j??????A??scaler?]?ݰt?m?j????X?H?s???^
			// read data line in an interlace manner then scale it
			0<<8 |  // 0=not line chroma
			1<<7 |  // 1=YCbCr
			0<<6 |  // 0=disable horizontal filter (use for down scale)
			1<<5 |  // 1=auto set coef of h-filter when down scale

			// NOTE: bits below are used for scaler to do deinterlacing
			0<<4 |  // 0=normal scale de-interlace mode
			0<<3 |  // 0=current field is field=0
			0<<2 |  // 0=field=0 is odd, field=1 is even
			0<<1 |  // 0=de-interlace disable
			0<<0;   // 0=use 2 line buffers

		if ((dst_width + hblank) < win_width)
			rLCD_VIDEO2_SCALE_CTL |= 1<<6;
		// enable filter for doing horizontal down scale

		/* when need cut after scaler,the dest size of the scaler must equal
		 * the size you wanted plus the cut number you wanted
		 */

		/* Right_bottom_cut_num REG0x360 */
		rLCD_VIDEO2_RIGHT_BOTTOM_CUT_NUM =
			right_blank<<8 | // 15-8: right cut number in line scaler
			bottom_blank;    // 7-0: bottom cut number in vertical scaler

		/* CLCD_VIDEO2_SCAL_CTL0 REG0x358 */
		rLCD_VIDEO2_SCAL_CTL0 =
			left_blank<<18 | // 27-18: left cut number in line scaler
			(win_width * 1024 / (dst_width + hblank));
		// hfz = (win_width * 1024) / (dst_width + hblank)

		/* CLCD_VIDEO2_SCAL_CTL1 REG0x35c */
		rLCD_VIDEO2_SCAL_CTL1 =
			top_blank<<18 |  // 25-18: up cut line number in vertical scaler
			(win_height * 1024 / (dst_height + vblank));
		// vfz = (win_heigh * 1024) / (dst_height + vblank)

		if (interlace_out_en) {
			/* TV_CONTROL REG0x2b0 */
			rLCD_TV_CONTROL &= ~(1<<8); // 0=non-interlace output mode, 
			// reading data from memory in a
			// progressive manner

			// when v scaler cof is 0x400, v scaler bypass, now we should
			// change the cof to force v scaler, otherwise there was sawtooth
			// on picture
			if ((rLCD_VIDEO2_SCAL_CTL1 & 0x3FFFF) == 0x400)
				rLCD_VIDEO2_SCAL_CTL1 = rLCD_VIDEO2_SCAL_CTL1 - 1;

			rLCD_VIDEO2_SCALE_CTL &= ~(7<<9); // clear bit 11-9
			rLCD_VIDEO2_SCALE_CTL |=
				1<<11 | // 1=update addr per frame (upon every odd field or 
				// even field)
				1<<9;   // 10-9: 10=read data every line (progressive) then
			// scale it
		} else {
			/* TV_CONTROL REG0x2b0 */
			//rLCD_TV_CONTROL |= 1<<8; // 1=interlace output mode, 
			// reading data from memory in an
			// interlace manner
		}
	}
}

void ark_disp_set_video2_writeback_en(
		int enable, unsigned int dst_width, unsigned int dst_height,
		unsigned int writeback_buf_addr)
{
	unsigned int WinHeight = 0;
	if (enable) {
		/* CLCD_CONTROL REG0x04 */
		rLCD_CONTROL |= 1<<10; // 1=enable write back for video2 layer

		/* CLCD_VIDEO2_CONTROL REG0x320 */
		rLCD_VIDEO2_CTL |=
			1<<26 | // 1=y data write first in doing data write back
			1<<25;  // 1=u write first in doing data write back
		// as a result, the write back data is in form of yuyvyuyv...

		rLCD_WRITE_BACK_ADDR_VIDEO2 = writeback_buf_addr;
		// when scaler write back, need fetch more data, otherwise,the data
		// write back would be less than wanted and no writeback finish 
		// interrupt.

		/* CLCD_WB_DATA_PER_FRAME_VIDEO2 REG0x388 */
		rLCD_WB_DATA_PER_FRAME_VIDEO2 = dst_width * dst_height;

		WinHeight = rLCD_VIDEO2_WIN_SIZE;
		rLCD_VIDEO2_WIN_SIZE = ((((WinHeight >> 12) & 0xfff) + 100)<<12) | (WinHeight & 0xfff);

		// CLCD_VIDEO2_CONTROL REG0x320
		rLCD_VIDEO2_CTL |= (1<<24); // bit24 = wb_start
		rLCD_VIDEO2_CTL &= ~(1<<24);
		// NOTE: the above steps are to set wb_start from 0 -> 1 -> 0
		// the transition of 0->1 will start a data write back
	} else {
		/* CLCD_CONTROL REG0x04 */
		rLCD_CONTROL &= ~(1<<10); // 0=disable write back for video2 layer
		rLCD_VIDEO2_CTL |= (1<<24); // bit24 = wb_start
		rLCD_VIDEO2_CTL &= ~(1<<24);		
	}
}

void ark_disp_set_video_source_size(int id, int width, int height)
{
	if (id == 0) {
		/* CLCD_VIDEO_SOURCE_SIZE REG0x16c */
		rLCD_VIDEO_SOURCE_SIZE =
			height << 12 | // 23-12: height of source video image in memory
			width;         // 11-0: width of source video image in memory
	} else if (id == 1) {
		/* CLCD_VIDEO2_SOURCE_SIZE REG0x324 */
		rLCD_VIDEO2_SOURCE_SIZE =
			height << 12 | // height of video2 source image in memory
			width;         // width of video2 source image in memory
	}
}

void ark_disp_set_video_layer_size(int id, int width, int height)
{
	if (id == 0) {
		/* CLCD_VIDEO_SIZE REG0x044 */
		rLCD_VIDEO_SIZE =
			height << 12 | // 23-12: height of video layer
			width;         // 11-0: width of video layer
	} else if (id == 1) {
		/* CLCD_VIDEO2_SIZE REG0x330 */
		rLCD_VIDEO2_SIZE =
			height << 12 | // 23-12: height of video2 layer output on display
			width;         // 11-0: width of video2 layer output on display
	}
}

void ark_disp_set_video_layer_position(int id, int x, int y)
{
	unsigned int x_sign, y_sign;
	int x_start, y_start;

	if (x < 0) {
		x_sign = 1;
		x_start = 0 - x;
	} else {
		x_sign = 0;
		x_start  = x;
	}
	if(y < 0) {
		y_sign = 1;
		y_start = 0 - y;
	} else  {
		y_sign = 0;
		y_start = y;
	}

	if (id == 0) {
		/* CLCD_VIDEO_POSITION REG0x04c */
		rLCD_VIDEO_POSITION =
			y_sign << 25 |  // sign of y
			y_start << 13 | // 24-13: y of video layer
			x_sign << 12 |  // sign of x
			x_start << 0;   // 11-0: x of video layer
	} else if (id == 1) {
		/* CLCD_VIDEO2_POSITION REG0x334 */
		rLCD_VIDEO2_POSITION =
			y_sign << 25 |  // sign of y
			y_start << 13 | // 24-13: y position of video2 layer
			x_sign << 12 |  // sign of x
			x_start << 0;   // 11-0: x position of video2 layer
	}
}

void ark_disp_set_video_win_size(int id, int width, int height)
{
	if (id == 0) {
		/* CLCD_VIDEO_WINDOW_SIZE REG0x040 */
		rLCD_VIDEO_WIN_SIZE =
			height << 12 |  // 23-12: height of source video window
			width;          // 11-0: width of source video window
	} else if (id == 1) {
		/* CLCD_VIDEO2_WINDOW_SIZE REG0x32c */
		rLCD_VIDEO2_WIN_SIZE =
			height << 12 | // 23-12: height of video2 image window
			width;         // 11-0: width of video2 image window
	}
}

void ark_disp_set_video_win_point(int id, int x, int y)
{
	if (id == 0) {
		/* CLCD_VIDEO_WINDOW_POINT REG0x15C */
		rLCD_VIDEO_WIN_POINT =
			y << 12 | // 23-12: y value of start point of video window
			x;        // 11-0: x value of start point of video window
	} else if (id == 1) {
		/* CLCD_VIDEO2_WINDOW_POINT REG0x328 */
		rLCD_VIDEO2_WIN_POINT =
			y << 12 | // 23-12: y value of start point of video2 window
			x;        // 11-0: x value of start point of video2 window
	}
	/* NOTE: the above set the start point of video window on the source
	 * video image in memory
	 */
}

void ark_disp_set_video_alpha(int id, unsigned char alpha)
{
	if (id == 0) {
		/* CLCD_VIDEO_VIDEO2_BLD_COEF REG0x048 */
		rLCD_VIDEO_VIDEO2_BLD_COEF &= ~0xFF;
		rLCD_VIDEO_VIDEO2_BLD_COEF |= alpha;    // 7-0: video2 blending coef
	} else if (id == 1) {
		/* CLCD_VIDEO_VIDEO2_BLD_COEF REG0x048 */
		rLCD_VIDEO_VIDEO2_BLD_COEF &= ~(0xFF<<8);
		rLCD_VIDEO_VIDEO2_BLD_COEF |= alpha<<8; // 15-8: video2 blending coef
	}
}

void ark_disp_set_video_alpha_blend_en_lcd(int id, int enable)
{
	unsigned int pos;

	if (id == 0) {
		pos = 11; /* bit position of video blend with back color enable */
	} else if (id == 1) {
		pos = 9; /* bit position of video2 blend with back color enable  */
	} else {
		/* invalid input */
		return;
	}

	/* CLCD_BLD_MODE_LCD_REG1 REG0x064 */
	if (enable) {
		rLCD_BLD_MODE_LCD_REG1 |= 1 << pos;
		/* enable blending of video layer with back color */
	} else {
		rLCD_BLD_MODE_LCD_REG1 &= ~(1 << pos);
		/* enable blending of video layer with back color */
	}
}

void ark_disp_set_video_alpha_blend_en_tvenc(int id, int enable)
{
	unsigned int pos;

	if (id == 0) {
		pos = 11; /* bit position of video blend with back color enable */
	} else if (id == 1) {
		pos = 9; /* bit position of video2 blend with back color enable  */
	} else {
		/* invalid input */
		return;
	}

	/* CLCD_BLD_MODE_TV_REG1 REG0x06c */
	if (enable) {
		rLCD_BLD_MODE_TV_REG1 |= 1 << pos;
		/* enable blending of video layer with back color */
	} else {
		rLCD_BLD_MODE_TV_REG1 &= ~(1 << pos);
		/* enable blending of video layer with back color */
	}
}

void ark_disp_set_video_per_pix_alpha_blend_en_lcd(int id, int enable)
{
	unsigned int pos;

	if (id == 0) {
		pos = 10; /* video */
	} else if (id == 1) {
		pos = 8; /* video2  */
	} else {
		/* invalid input */
		return;
	}

	/* CLCD_BLD_MODE_LCD_REG1 REG0x064 */
	if (enable) {
		rLCD_BLD_MODE_LCD_REG1 |= 1 << pos;
		/* enable using of pixel alpha (alpha value is from pixel data */
	} else {
		rLCD_BLD_MODE_LCD_REG1 &= ~(1 << pos);
		/* enable using of layer alpha (alpha value is from register */
	}
}

void ark_disp_set_video_per_pix_alpha_blend_en_tvenc(int id, int enable)
{
	unsigned int pos;

	if (id == 0) {
		pos = 10; /* video */
	} else if (id == 1) {
		pos = 8; /* video2  */
	} else {
		/* invalid input */
		return;
	}

	/* CLCD_BLD_MODE_TV_REG1 REG0x06c */
	if (enable) {
		rLCD_BLD_MODE_TV_REG1 |= 1 << pos;
		/* enable using of pixel alpha (alpha value is from pixel data */
	} else {
		rLCD_BLD_MODE_TV_REG1 &= ~(1 << pos);
		/* enable using of layer alpha (alpha value is from register */
	}
}

void ark_disp_set_video_blend_mode_lcd(int id, unsigned int mode)
{
	if (id == 0) {
		/* CLCD_BLD_MODE_LCD_REG0 REG0x060 */
		rLCD_BLD_MODE_LCD_REG0 &= ~(0xF << 4); // clear bit 7-4
		rLCD_BLD_MODE_LCD_REG0 |= mode << 4; // 7-4: set video blending mode
	} else if (id == 1) {
		/* CLCD_BLD_MODE_LCD_REG1 REG0x064 */
		rLCD_BLD_MODE_LCD_REG1 &= ~(0xF << 4); // clear bit 7-4
		rLCD_BLD_MODE_LCD_REG1 |= mode << 4; // 7-4: set video2 blending mode
	}
}

void ark_disp_set_video_blend_mode_tvenc(int id, unsigned int mode)
{
	if (id == 0) {
		/* CLCD_BLD_MODE_TV_REG0 REG0x068 */
		rLCD_BLD_MODE_TV_REG0 &= ~(0xF << 4); // clear bit 7-4
		rLCD_BLD_MODE_TV_REG0 |= mode << 4;	// 7-4: set video blending mode
	} else if (id == 1) {
		/* CLCD_BLD_MODE_TV_REG1 REG0x06c */
		rLCD_BLD_MODE_TV_REG1 &= ~(0xF << 4); // clear bit 7-4
		rLCD_BLD_MODE_TV_REG1 |= mode << 4;	// 7-4: set video2 blending mode
	}
}

void ark_disp_set_video_colorkey(
		int id, int enable, unsigned int y, unsigned int cb, unsigned int cr)
{
	if (id == 0) {
		/* CLCD_COLOR_KEY_MASK_VALUE_VIDEO REG0xe8 */
		rLCD_COLOR_KEY_MASK_VALUE_VIDEO =
			enable << 24 | // 1=enable video layer color key mask
			y << 16 | // 23-16: video layer color key mask value of y
			cb << 8 | // 15-8: video layer color key mask value of cb
			cr << 0;  // 7-0: video layer color key mask value of cr
	} else if (id == 1) {
		/* CLCD_COLOR_KEY_MASK_VALUE_VIDEO2 REG0x378 */
		rLCD_COLOR_KEY_MASK_VALUE_VIDEO2 =
			enable << 24 | // 1=enable video layer color key mask
			y << 16 | // 23-16: video layer color key mask value of y
			cb << 8 | // 15-8: video layer color key mask value of cb
			cr << 0;  // 7-0: video layer color key mask value of cr
	}
}

void ark_disp_get_video_colorkey(int id, unsigned int *colorkey)
{
	if (id == 0) {
		/* CLCD_COLOR_KEY_MASK_VALUE_VIDEO REG0xe8 */
		*colorkey = rLCD_COLOR_KEY_MASK_VALUE_VIDEO;
	} else if (id == 1) {
		/* CLCD_COLOR_KEY_MASK_VALUE_VIDEO2 REG0x378 */
		*colorkey = rLCD_COLOR_KEY_MASK_VALUE_VIDEO2;
	}
}


void ark_disp_set_video_colorkey_thld(
		int id, unsigned int y_thld, unsigned int cb_thld, unsigned int cr_thld)
{
	if (id == 0) {
		/* CLCD_COLOR_KEY_MASK_THLD_VIDEO REG0xd8 */
		rLCD_COLOR_KEY_MASK_THLD_VIDEO =
			y_thld << 16 | // 23-16: osd1 layer color key mask threshold of y
			cb_thld << 8 | // 15-8: osd1 layer color key mask threshold of cb
			cr_thld << 0;  // 7-80 osd1 layer color key mask threshold of cr
	} else if (id == 1) {
		/* CLCD_COLOR_KEY_MASK_THLD_VIDEO2 REG0x374 */
		rLCD_COLOR_KEY_MASK_THLD_VIDEO2 =
			y_thld << 16 | // 23-16: osd1 layer color key mask threshold of y
			cb_thld << 8 | // 15-8: osd1 layer color key mask threshold of cb
			cr_thld << 0;  // 7-80 osd1 layer color key mask threshold of cr
	}
}

void ark_disp_get_video_colorkey_thld(int id, unsigned int *colorkey_thld)
{
	if (id == 0) {
		/* CLCD_COLOR_KEY_MASK_THLD_VIDEO REG0xd8 */
		*colorkey_thld = rLCD_COLOR_KEY_MASK_THLD_VIDEO;
	} else if (id == 1) {
		/* CLCD_COLOR_KEY_MASK_THLD_VIDEO2 REG0x374 */
		*colorkey_thld =rLCD_COLOR_KEY_MASK_THLD_VIDEO2;
	}
}

void ark_disp_set_video_priority_lcd(int id, int level)
{    
	if (id == 0) {
		/* CLCD_BLD_MODE_LCD_REG0 REG0x060 */
		rLCD_BLD_MODE_LCD_REG0 &= ~(0xF << 0);  // clear bit 2-0
		rLCD_BLD_MODE_LCD_REG0 |= (level << 0); // 2-0: blending priority
	} else if (id == 1) {
		/* CLCD_BLD_MODE_LCD_REG1 REG0x064 */
		rLCD_BLD_MODE_LCD_REG1 &= ~(0xF << 0);  // clear bit 2-0
		rLCD_BLD_MODE_LCD_REG1 |= (level << 0); // 2-0: blending priority
	}
}

void ark_disp_set_video_priority_tvenc(int id, int level)
{
	if (id == 0) {
		/* CLCD_BLD_MODE_TV_REG0 REG0x068 */
		rLCD_BLD_MODE_TV_REG0 &= ~(0xF << 0);  // clear bit 2-0
		rLCD_BLD_MODE_TV_REG0 |= (level << 0); // 2-0: blending priority
	} else if (id == 1) {
		/* CLCD_BLD_MODE_TV_REG1 REG0x06c */
		rLCD_BLD_MODE_TV_REG1 &= ~(0xF << 0);  // clear bit 2-0
		rLCD_BLD_MODE_TV_REG1 |= (level << 0); // 2-0: blending priority
	}
}

void ark_disp_set_video_clk_sel(int id, unsigned int sel)
{
	//chitest TBD ...
	unsigned int pos;

	switch (id)
	{
		case 0: // video
			if (sel)
				rSYS_LCD_CLK_CFG |= (1 << 14);
			else
				rSYS_LCD_CLK_CFG &= ~(1 << 14);
			break;
		case 1: // video2
			if (sel)
				rSYS_DEVICE_CLK_CFG0 |= (1 << 28);
			else
				rSYS_DEVICE_CLK_CFG0 &= ~(1 << 28);
			break;
		default:
			break;
	}
}

int ark_disp_get_video_en_lcd(int id)
{
	unsigned int pos;

	/* CLCD_CONTROL REG0x04 */
	switch (id)
	{
		case 0: // video
			pos = 5;
			break;
		case 1: // video2
			pos = 6;
			break;
		default: // video (by default)
			pos = 5;
			break;
	}
	return ((rLCD_CONTROL >> pos) & 0x1);
}

void ark_disp_set_video_en_lcd(int id, int enable)
{
	unsigned int pos;

	/* CLCD_CONTROL REG0x04 */
	switch (id)
	{
		case 0: // video
			pos = 5;
			break;
		case 1: // video2
			pos = 6;
			break;
		default: // video (by default)
			pos = 5;
			break;
	}
	if (enable)
		rLCD_CONTROL |= (1 << pos);  /* enable video layer on LCD screen */
	else
		rLCD_CONTROL &= ~(1 << pos); /* disable video layer on LCD screen */
}

int ark_disp_get_video_en_tvenc(int id)
{
	unsigned int pos;

	/* TV_CONTROL REG0x2b0 */
	switch (id)
	{
		case 0: // video
			pos = 2;
			break;
		case 1: // video2
			pos = 3;
			break;
		default: // video (by default)
			pos = 2;
			break;
	}
	return ((rLCD_TV_CONTROL >> pos) & 0x1);
}

void ark_disp_set_video_en_tvenc(int id, int enable)
{
    unsigned int pos;

    /* CLCD_CONTROL REG0x04 */
    switch (id)
    {
    case 0: // video
    	if (enable) {
			rSYS_LCD_CLK_CFG |=(1<<14);
			rLCD_TV_CONTROL |= 1<<2;	
			rLCD_CONTROL &= ~(1 << 5); 
		} else {
			rSYS_LCD_CLK_CFG &=~(1<<14);
			rLCD_TV_CONTROL &=~(1<<2);		
		}
        break;
    case 1: // video2
    	if (enable) {
			rSYS_DEVICE_CLK_CFG0 |=(1<<28);
			rLCD_TV_CONTROL |= 1<<3;
			rLCD_CONTROL &= ~(1 << 6);
		} else {
			rSYS_DEVICE_CLK_CFG0&=~(1<<28);
			rLCD_CONTROL&=~(1<<3);		
		}
        break;
    default:
        break;
    }
}

unsigned int ark_disp_get_video_hue(int id)
{
	unsigned int val;

	switch (id)
	{
		case 0:
			/* VIDEO_VP_REG_1 REG0x1d8 */
			val = rLCD_VIDEO_VP_REG_1;
			break;
		case 1:
			/* VIDEO2_VP_REG_1 REG0x134 */
			val = rLCD_VIDEO2_VP_REG_1;
			break;
		default:
			/* VIDEO_VP_REG_1 REG0x1d8 (by default) */
			val = rLCD_VIDEO_VP_REG_1;
			break;
	}

	val = (val >> 24) & 0xFF; /* hue: bit31-24 */
	return val;
}

unsigned int ark_disp_get_video_saturation(int id)
{
	unsigned int val;

	switch (id)
	{
		case 0:
			/* VIDEO_VP_REG_1 REG0x1d8 */
			val = rLCD_VIDEO_VP_REG_1;
			break;
		case 1:
			/* VIDEO2_VP_REG_1 REG0x134 */
			val = rLCD_VIDEO2_VP_REG_1;
			break;
		default:
			/* VIDEO_VP_REG_1 REG0x1d8 (by default) */
			val = rLCD_VIDEO_VP_REG_1;
			break;
	}

	val = (val >> 16) & 0xFF; /* saturation: bit23-16 */
	return val;
}

unsigned int ark_disp_get_video_brightness(int id)
{
	unsigned int val;

	switch (id)
	{
		case 0:
			/* VIDEO_VP_REG_1 REG0x1d8 */
			val = rLCD_VIDEO_VP_REG_1;
			break;
		case 1:
			/* VIDEO2_VP_REG_1 REG0x134 */
			val = rLCD_VIDEO2_VP_REG_1;
			break;
		default:
			/* VIDEO_VP_REG_1 REG0x1d8 (by default) */
			val = rLCD_VIDEO_VP_REG_1;
			break;
	}

	val = (val >> 8) & 0xFF; /* brightness: bit15-8 */
	return val;
}

unsigned int ark_disp_get_video_contrast(int id)
{
	unsigned int val;

	switch (id)
	{
		case 0:
			/* VIDEO_VP_REG_1 REG0x1d8 */
			val = rLCD_VIDEO_VP_REG_1;
			break;
		case 1:
			/* VIDEO2_VP_REG_1 REG0x134 */
			val = rLCD_VIDEO2_VP_REG_1;
			break;
		default:
			/* VIDEO_VP_REG_1 REG0x1d8 (by default) */
			val = rLCD_VIDEO_VP_REG_1;
			break;
	}

	val = (val >> 0) & 0xFF; /* contrast: bit15-8 */
	return val;
}

void ark_disp_set_video_hue(int id, unsigned int hue)
{
	switch (id)
	{
		case 0:
			/* VIDEO_VP_REG_1 REG0x1d8 */
			rLCD_VIDEO_VP_REG_1 &= ~(0xFF << 24); /* Hue: bit31-24 */
			rLCD_VIDEO_VP_REG_1 |= hue << 24;
			break;
		case 1:
			/* VIDEO2_VP_REG_1 REG0x134 */
			rLCD_VIDEO2_VP_REG_1 &= ~(0xFF << 24); /* Hue: bit31-24 */
			rLCD_VIDEO2_VP_REG_1 |= hue << 24;
			break;
		default:
			/* VIDEO_VP_REG_1 REG0x1d8 (by default) */
			rLCD_VIDEO_VP_REG_1 &= ~(0xFF << 24); /* Hue: bit31-24 */
			rLCD_VIDEO_VP_REG_1 |= hue << 24;
			break;
	}
}

void ark_disp_set_video_saturation(int id, unsigned int saturation)
{
	switch (id)
	{
		case 0:
			/* VIDEO_VP_REG_1 REG0x1d8 */
			rLCD_VIDEO_VP_REG_1 &= ~(0xFF << 16); /* Saturation: bit23-16 */
			rLCD_VIDEO_VP_REG_1 |= saturation << 16;
			break;
		case 1:
			/* VIDEO2_VP_REG_1 REG0x134 */
			rLCD_VIDEO2_VP_REG_1 &= ~(0xFF << 16); /* Saturation: bit23-16 */
			rLCD_VIDEO2_VP_REG_1 |= saturation << 16;
			break;
		default:
			/* VIDEO_VP_REG_1 REG0x1d8 (by default) */
			rLCD_VIDEO_VP_REG_1 &= ~(0xFF << 16); /* Saturation: bit23-16 */
			rLCD_VIDEO_VP_REG_1 |= saturation << 16;
			break;
	}
}

void ark_disp_set_video_brightness(int id, unsigned int brightness)
{
	switch (id)
	{
		case 0:
			/* VIDEO_VP_REG_1 REG0x1d8 */
			rLCD_VIDEO_VP_REG_1 &= ~(0xFF << 8); /* Brightness: bit15-8 */
			rLCD_VIDEO_VP_REG_1 |= brightness << 8;
			break;
		case 1:
			/* VIDEO2_VP_REG_1 REG0x134 */
			rLCD_VIDEO2_VP_REG_1 &= ~(0xFF << 8); /* Brightness: bit15-8 */
			rLCD_VIDEO2_VP_REG_1 |= brightness << 8;
			break;
		default:
			/* VIDEO_VP_REG_1 REG0x1d8 */
			rLCD_VIDEO_VP_REG_1 &= ~(0xFF << 8); /* Brightness: bit15-8 */
			rLCD_VIDEO_VP_REG_1 |= brightness << 8;
			break;    
	}
}

void ark_disp_set_video_contrast(int id, unsigned int contrast)
{
	switch (id)
	{
		case 0:
			/* VIDEO_VP_REG_1 REG0x1d8 */
			rLCD_VIDEO_VP_REG_1 &= ~(0xFF << 0); /* Contrast: bit7-0 */
			rLCD_VIDEO_VP_REG_1 |= contrast << 0;
			break;
		case 1:
			/* VIDEO2_VP_REG_1 REG0x134 */
			rLCD_VIDEO2_VP_REG_1 &= ~(0xFF << 0); /* Contrast: bit7-0 */
			rLCD_VIDEO2_VP_REG_1 |= contrast << 0;
			break;
		default:
			/* VIDEO_VP_REG_1 REG0x1d8 */
			rLCD_VIDEO_VP_REG_1 &= ~(0xFF << 0); /* Contrast: bit7-0 */
			rLCD_VIDEO_VP_REG_1 |= contrast << 0;
			break;    
	}
}

void ark_disp_set_video_layer_cut(int id,
		unsigned int left, unsigned int right,
		unsigned int up, unsigned int down)
{
	if (id == 0) {
		/* CLCD_BLD_CUT_LEFT_RIGHT_VIDEO REG0xfc */
		rLCD_BLD_CUT_LEFT_RIGHT_VIDEO =
			right<<12 |  // 23-12: blending window cut num of right side
			(left & 0xfff)<<0; // 11-0: blending window cut num of left side

		/* CLCD_BLD_CUT_UP_DOWN_VIDEO REG0x100 */
		rLCD_BLD_CUT_UP_DOWN_VIDEO =
			down<<12 |  // 23-12: blending window cut number of down side
			(up & 0xfff)<<0; // 11-0: blending window cut number of up side
	} else if (id == 1) {
		/* CLCD_BLD_CUT_LEFT_RIGHT_VIDEO2 REG0x37c */
		rLCD_BLD_CUT_LEFT_RIGHT_VIDEO2 =
			right<<12 |
			(left & 0xfff)<<0;

		/* CLCD_BLD_CUT_UP_DOWN_VIDEO2 REG0x380 */
		rLCD_BLD_CUT_UP_DOWN_VIDEO2 =
			down<<12 |
			(up & 0xfff)<<0;
	}
}

void ark_disp_set_video_data_addr(int id, unsigned int format,
		unsigned int width, unsigned int height, unsigned int yaddr,
		unsigned int cbaddr, unsigned int craddr)
{
#ifdef ARK_DISP_USE_HW_EMULATION
	ARKDISP_DBGPRTK("%s %d: buf_addr=0x%08x\n",
			__FUNCTION__, __LINE__, buf_addr);

	ec_dcu_write_reg(0x20, yaddr & 0x0fffffff);
#endif
	if (id == 0) {
		/* CLCD_VIDEO_ADDR1 REG0x054 */
		rLCD_VIDEO_ADDR1 = yaddr;
		// start addr of y or rgb video data in group 0

		if(cbaddr != 0) {
			rLCD_VIDEO_ADDR2 = cbaddr;
		} else {
			/* CLCD_VIDEO_ADDR2 yaddr */
			rLCD_VIDEO_ADDR2 = yaddr + width * height;
		}
		// start addr of u data in group 0
		if(craddr != 0) {
			rLCD_VIDEO_ADDR3 = craddr;
		} else {
			if (format == ARK_DISP_VIDEO_PIXFMT_YUV420) {
				/* CLCD_VIDEO_ADDR3 REG0x05c */
				rLCD_VIDEO_ADDR3 = rLCD_VIDEO_ADDR2 + width * height / 4;
				// start addr of v data in group 0
			} else  {
				/* CLCD_VIDEO_ADDR3 REG0x05c */
				rLCD_VIDEO_ADDR3 = rLCD_VIDEO_ADDR2 + width * height / 2;
				// start addr of v data in group 0
			}
		}
	} else if (id == 1) {
		/* CLCD_VIDEO2_ADDR1 REG0x338 */
		rLCD_VIDEO2_ADDR1 = yaddr;
		// start addr of y or rgb data in group 0
		if(cbaddr != 0) {
			rLCD_VIDEO2_ADDR2 = cbaddr;
		} else {
			// CLCD_VIDEO2_ADDR2 REG0x33c
			rLCD_VIDEO2_ADDR2 = yaddr + width * height;
			// start addr of u data in group 0			
		}

		if(craddr != 0) {
			rLCD_VIDEO2_ADDR3 = craddr;
		} else {
			if (format == ARK_DISP_VIDEO_PIXFMT_YUV420) {
				/* CLCD_VIDEO2_ADDR3 REG0x340 */
				rLCD_VIDEO2_ADDR3 = rLCD_VIDEO2_ADDR2 + width * height / 4;
				// start addr of v data in group 0
			} else {
				rLCD_VIDEO2_ADDR3 = rLCD_VIDEO2_ADDR2 + width * height / 2;
				// start addr of v data in group 0
			}
		}					
	}
}

void ark_disp_set_video_data_addr_cvbs(int id, unsigned int format,
		unsigned int width, unsigned int height, unsigned int addr)
{
	if (id == 0) {
		// CLCD_VIDEO_ADDR1 REG0x054
		rLCD_VIDEO_ADDR1 = addr;
		// start addr of Y or RGB video data in group0

		switch (format)
		{
			case ARK_DISP_VIDEO_PIXFMT_YUV422:
				// CLCD_VIDEO_ADDR2 REG0x058
				rLCD_VIDEO_ADDR2 = rLCD_VIDEO_ADDR1 + width * height;
				// start addr of u video data in group0    
				// CLCD_VIDEO_ADDR3 REG0x05c
				rLCD_VIDEO_ADDR3 = rLCD_VIDEO_ADDR2 + width * height / 2;
				// start addr of v video data in group0
				// CLCD_VIDEO_ADDR1_GROUP1 REG0x2e4
				rLCD_VIDEO_ADDR1_GROUP1 = rLCD_VIDEO_ADDR1 + width;
				// start addr of y data in group1 for double buffer mode
				// CLCD_VIDEO_ADDR2_GROUP1 REG0x2e8
				rLCD_VIDEO_ADDR2_GROUP1 = rLCD_VIDEO_ADDR2 + width / 2;
				// start addr of u data in group1 for double buffer mode
				// CLCD_VIDEO_ADDR3_GROUP1 REG0x2ec
				rLCD_VIDEO_ADDR3_GROUP1 = rLCD_VIDEO_ADDR3 + width / 2;
				// start addr of v data in group1 for double buffer mode
				break;
			case ARK_DISP_VIDEO_PIXFMT_YUV420:
				// CLCD_VIDEO_ADDR2 REG0x058
				rLCD_VIDEO_ADDR2 = rLCD_VIDEO_ADDR1 + width * height;
				// start addr of u video data in group0
				// CLCD_VIDEO_ADDR3 REG0x05c
				rLCD_VIDEO_ADDR3 = rLCD_VIDEO_ADDR2 + width * height / 4;
				// start addr of v video data in group0
				// CLCD_VIDEO_ADDR1_GROUP1 REG0x2e4
				rLCD_VIDEO_ADDR1_GROUP1 = rLCD_VIDEO_ADDR1 + width;
				// start addr of Y data in group1 for double buffer mode
				// CLCD_VIDEO_ADDR2_GROUP1 REG0x2e8
				rLCD_VIDEO_ADDR2_GROUP1 = rLCD_VIDEO_ADDR2 + width / 2;
				// start addr of u data in group1 for double buffer mode           
				// CLCD_VIDEO_ADDR3_GROUP1 REG0x2ec
				rLCD_VIDEO_ADDR3_GROUP1 = rLCD_VIDEO_ADDR3 + width / 2;
				// start addr of v data in group1 for double buffer mode
				break;
			case ARK_DISP_VIDEO_PIXFMT_YUYV: 
			case ARK_DISP_VIDEO_PIXFMT_RGBI555:
			case ARK_DISP_VIDEO_PIXFMT_RGB565:
				// bpp=16
				// CLCD_VIDEO_ADDR1_GROUP1 REG0x2e4
				rLCD_VIDEO_ADDR1_GROUP1 = addr + width * 2;
				// start addr of y or rgb data in group1 for double buffer mode
				break;
			case ARK_DISP_VIDEO_PIXFMT_YUV:
			case ARK_DISP_VIDEO_PIXFMT_RGB888:
			case ARK_DISP_VIDEO_PIXFMT_RGBA888:
				// bpp=32
				// CLCD_VIDEO_ADDR1_GROUP1 REG0x2e4
				rLCD_VIDEO_ADDR1_GROUP1 = addr + width * 4;
				// start addr of y or rgb video in group1 for double buffer mode
				break;
			default:
				break;
		}
	} else if (id == 1) {
		// CLCD_VIDEO2_ADDR1 REG0x338
		rLCD_VIDEO2_ADDR1 = addr;
		// start addr of y or rgb video data in group0

		switch (format)
		{
			case ARK_DISP_VIDEO_PIXFMT_YUV422:
				// CLCD_VIDEO2_ADDR2 REG0x33c
				rLCD_VIDEO2_ADDR2 = rLCD_VIDEO2_ADDR1 + width * height;
				// start addr of u video data in group0
				// CLCD_VIDEO2_ADDR3 REG0x340
				rLCD_VIDEO2_ADDR3 = rLCD_VIDEO2_ADDR2 + width * height / 2;
				// start addr of v video data in group0
				// CLCD_VIDEO2_ADDR1_GROUP1 REG0x344
				rLCD_VIDEO2_ADDR1_GROUP1 = rLCD_VIDEO2_ADDR1 + width;
				// start addr of y data in group1 for double buffer mode
				// CLCD_VIDEO2_ADDR2_GROUP1 REG0x348
				rLCD_VIDEO2_ADDR2_GROUP1 = rLCD_VIDEO2_ADDR2 + width / 2;
				// start addr of u data in group1 for double buffer mode
				// CLCD_VIDEO2_ADDR3_GROUP1 REG0x34c
				rLCD_VIDEO2_ADDR3_GROUP1 = rLCD_VIDEO2_ADDR3 + width / 2;
				// start addr of v data in group1 for double buffer mode
				break;
			case ARK_DISP_VIDEO_PIXFMT_YUV420:
				// CLCD_VIDEO2_ADDR2 REG0x33c
				rLCD_VIDEO2_ADDR2 = rLCD_VIDEO2_ADDR1 + width * height;
				// start addr of u video data in group 0
				// CLCD_VIDEO2_ADDR3 REG0x340
				rLCD_VIDEO2_ADDR3 = rLCD_VIDEO2_ADDR2 + width * height / 4;
				// start addr of v video data in group 0
				// CLCD_VIDEO2_ADDR1_GROUP1 REG0x344
				rLCD_VIDEO2_ADDR1_GROUP1 = rLCD_VIDEO2_ADDR1 + width;
				// start addr of y data in group1 for double buffer mode
				// CLCD_VIDEO2_ADDR2_GROUP1 REG0x348
				rLCD_VIDEO2_ADDR2_GROUP1 = rLCD_VIDEO2_ADDR2 + width / 2;
				// start addr of u data in group1 for double buffer mode
				// CLCD_VIDEO2_ADDR3_GROUP1 REG0x34c
				rLCD_VIDEO2_ADDR3_GROUP1 = rLCD_VIDEO2_ADDR3 + width / 2;
				// start addr of v data in group1 for double buffer mode
				break;
			case ARK_DISP_VIDEO_PIXFMT_YUYV: 
			case ARK_DISP_VIDEO_PIXFMT_RGBI555:
			case ARK_DISP_VIDEO_PIXFMT_RGB565:
				// bpp=16
				// CLCD_VIDEO_ADDR1_GROUP1 REG0x344
				rLCD_VIDEO2_ADDR1_GROUP1 = addr + width * 2;
				// start addr of y or rgb data in group1 for double buffer mode
				break;
			case ARK_DISP_VIDEO_PIXFMT_YUV:
			case ARK_DISP_VIDEO_PIXFMT_RGB888:
			case ARK_DISP_VIDEO_PIXFMT_RGBA888:
				// bpp=32
				// CLCD_VIDEO_ADDR1_GROUP1 REG0x344
				rLCD_VIDEO2_ADDR1_GROUP1 = addr + width * 4;
				// start addr of y or rgb data in group1 for double buffer mode
				break;
			default:
				break;
		}
	}
}

void ark_disp_set_video_double_buffer_en(int id, int enable)
{
	if (id == 0) {
		if (enable) {
			// CLCD_VIDEO_CTL Reg0x3c
			rLCD_VIDEO_CTL |= 1<<20; // 1=enable double buffer mode
		} else {
			rLCD_VIDEO_CTL &= ~(1<<20);
		}
	} else if (id == 1) {
		if (enable) {
			// CLCD_VIDEO2_CTL Reg0x320
			rLCD_VIDEO2_CTL |= 1<<20; // 1=enable double buffer mode
		} else {
			rLCD_VIDEO2_CTL &= ~(1<<20);
		}
	}
}

void ark_disp_set_video_frame_mode(int id, int frame)
{
	/* frame mode select for interlace data capture of video layer, normally
	 * used in cvbs, itu656, ypbpr
	 */
	if (id == 0) {
		if (frame) {
			// CLCD_VIDEO_CTL Reg0x3c
			rLCD_VIDEO_CTL |= 1<<7; // 1=frame mode
		} else {
			rLCD_VIDEO_CTL &= ~(1<<7);
		}
	} else if (id == 1) {
		if (frame) {
			// CLCD_VIDEO2_CTL Reg0x320
			rLCD_VIDEO2_CTL |= 1<<7;
		} else {
			rLCD_VIDEO2_CTL &= ~(1<<7);
		}
	}
}

void ark_disp_set_video_frame_invert(int id, int invert)
{
	/* frame mode select for interlace data capture of video layer, normally
	 * used in cvbs, itu656, ypbpr
	 */
	if (id == 0) {
		if (invert) {
			// CLCD_VIDEO_CTL Reg0x3c
			rLCD_VIDEO_CTL |= 1<<6; // 1=invert
		} else {
			rLCD_VIDEO_CTL &= ~(1<<6);
		}
	} else if (id == 1) {
		if (invert) {
			// CLCD_VIDEO2_CTL Reg0x320
			rLCD_VIDEO2_CTL |= 1<<6;
		} else {
			rLCD_VIDEO2_CTL &= ~(1<<6);
		}
	}
}

