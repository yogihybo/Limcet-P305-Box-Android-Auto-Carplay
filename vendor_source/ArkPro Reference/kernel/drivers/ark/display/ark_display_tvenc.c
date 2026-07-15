/*
 * Copyright(c) 2012 Hong Kong Applied Science and Technology
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
 *      ark_display_tvenc.c
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

#include <linux/ark/ringpair.h>
#include <linux/ark/ark_display_io.h>
#include <mach/hardware.h>
#include <mach/va_map.h>
#include "ark_display.h"
#include "ark_display_priv.h"
#include "ark_lcd_regs.h"
#include "ark_sys_regs.h"


#define VGA640x480hz60      0
#define VGA800x600hz60      1
#define VGA1024x768hz60     2
#define VGA1280x1024hz60    3
#define VGA1900x1200hz60    4  // bandwidth  limit
#define VGA1280x1024hz75    5
#define VGA1280x960hz85     6  // bandwidth  limit
#define VGA1280x720hz60     7

#define VGA_CLK_25_175M     0
#define VGA_CLK_40M         1
#define VGA_CLK_65M         2
#define VGA_CLK_74_25M      3
#define VGA_CLK_108M        4
#define VGA_CLK_154M        5
#define VGA_CLK_135M        6
#define VGA_CLK_148_4M      7

typedef struct {
	unsigned int width;
	unsigned int height;
	unsigned int hbp;
	unsigned int hfp;
	unsigned int hsw;
	unsigned int vbp;
	unsigned int vfp;
	unsigned int vsw;
} VGA_FORMAT;

VGA_FORMAT vga_info[8] = 
{
	//    {640, 480, 48, 16, 96, 33, 10, 5},
	{640,  480,  48,  16,  96,  34, 11, 2}, 
	{800,  600,  88,  40,  128, 23, 1,  4}, 
	{1024, 768,  160, 24,  136, 29, 3,  6},
	{1280, 1024, 248, 48,  112, 38, 1,  3},//60hz
	{1920, 1200, 80,  48,  32,  26, 3,  6},
	{1280, 1024, 248, 16,  144, 38, 1,  3},//75hz
	{1280, 960,  224, 64,  160, 47, 1,  3},//85hz
	{1280, 720,  220, 110, 40,  20, 5,  5}
};

#define CVBS_PAL    0
#define CVBS_NTSC   1

typedef struct {
	unsigned int width;
	unsigned int height;
	unsigned int hsw;
	unsigned int vsw;
	unsigned int mode;
} cvbs_format_struct;

cvbs_format_struct cvbs_info[2] =
{		
	//{720, 576, 144, 24, 0}, //PAL
	{720, 576, 142, 24, 0}, //PAL
	// {720, 480, 138, 22, 1}  //NTSC
	{720, 480, 136, 22, 1}  //NTSC
};

/*************************************************************
0: 720x480i  1:720x576i  2:720x480p 3:720x576p4:1280x720p(60hz)  
5: 1280x720p(50hz) 6:1920x1080i(60hz)  7:1920x1080i(50hz,1125)
8:1920x1080i(50hz,1250)  9:1920x1080p(60hz) 10: 1920x1080p(50hz)  
 ****************************************************************/

#define i480hz60        0  
#define i576hz50        1
#define p480hz60        2
#define p576hz50        3
#define p720hz60        4
#define p720hz50        5
#define i1080hz60       6
#define i1080hz50       7
#define i1080hz50_1250  8
#define p1080hz60       9
#define p1080hz50       10

typedef struct
{
	unsigned int tHFP;
	unsigned int tHBP;
	unsigned int tHSW;

	unsigned int tCPL;
	unsigned int tVSW;
	unsigned int tVFP;

	unsigned int tIOE;
	unsigned int tIHS;
	unsigned int tIVS;
	unsigned int tLPS;
	unsigned int tVBP;
} tvenc_info_struct;

typedef struct {
	unsigned int  width;
	unsigned int  height;
	unsigned int  hsw;
	unsigned int  vsw;
	unsigned char scan_mode; //0:progressive 1:interlater   
} ypbpr_format_struct;


ypbpr_format_struct ypbpr_info[11] = 
{
	{720,  480,   138, 22, 1},  //i480      0
	{720,  576,   144, 24, 1}, 	//i576      1
	{720,  480,   138, 45, 0}, 	//p480      2
	{720,  576,   144, 49, 0}, 	//p576      3

	{1280, 720,  370, 30, 0},	//p720hz60  4
	{1280, 720,  700, 30, 0},	//p720hz50  5

	{1920, 1080, 280, 22, 1},	//i1080hz60     6
	{1920, 1080, 720, 22, 1},	//i1080hz50     7
	{1920, 1080, 456, 35, 1},	//i1080hz50_2   8

	{1920, 1080, 280, 45, 0},   // p1080hz60    9
	// (1920+280)*(1080+45)*60 = 148.5M
	{1920, 1080, 720, 45, 0}    // p1080hz50    10
};

#define ITU656OUT_PAL   0
#define ITU656OUT_NTSC  1

typedef struct {
	unsigned int width;
	unsigned int height;
	unsigned int hsw;
	unsigned int vsw;
	unsigned int mode;
} itu656out_format_struct;

itu656out_format_struct itu656out_info[2] =
{
	{720, 576, 142, 24, 0}, //PAL
	{720, 480, 136, 22, 1}  //NTSC
};

//chitestark
static int GetDDSFrequency(void)
{
	printk("%s %d: pseudo func\n", __FUNCTION__, __LINE__);
	return 0;
}

static int SetDDSFrequency(int freq)
{
	unsigned int  dto_inc;
	unsigned int  dds_cofe;
	unsigned int  val;

	val = rSYS_ANALOG_REG1;
	val &= ~(0x1<<2);// scaler_factor = 32
	val &= ~(0x07<<10); 
	val |= (1<<18); // enable	
	rSYS_ANALOG_REG1 = val;
	
	dds_cofe = 1<<22;
	dto_inc = dds_cofe*freq/(24*32);

	val = rSYS_DDS_CLK_CFG;
	val &= ~(0x3fffff);
	val |= dto_inc;
	rSYS_DDS_CLK_CFG = val;  
	
	return 0;
}

void ark_disp_tvenc_reset(void)
{
	rSYS_ANALOG_REG0 &= ~(3<<19);
	rSYS_ANALOG_REG1 &= ~((3<<22) | (1<<17) | (1<<15) | (7<<3) | 1);
	rLCD_YPBPR_CTRL0 &= ~1;
	rLCD_TV_CONTROL = 0;
}


void ark_disp_set_tvenc_backcolor(unsigned char y, unsigned char cb,
		unsigned char cr)
{
	// CLCD_VIDEO_BACK_COLOR_TV REG0x2cc
	rLCD_BACK_COLOR_TV =
		(y<<16) | // 23-16: y of back color
		(cb<<8) | // 15-8: Cb of back color
		cr;       // 7-0: Cr of back color

	/* NOTE: if video layer is smaller than the panel, the area outside the
	 * layer will be filled with the back color
	 */
}

void ark_disp_config_tvenc_interlace_out(int interlace)
{
	if (interlace) rLCD_TV_CONTROL |= (1 << 8);
	else rLCD_TV_CONTROL &= ~(1 << 8);
}

void ark_disp_set_tvenc_en(int mode, int enable)
{
	if (mode == ARKDISP_TVENC_OUT_CVBS_PAL || mode == ARKDISP_TVENC_OUT_CVBS_NTSC) {
		//3 ch all out
		if (enable) rSYS_ANALOG_REG1 |= 7 << 3;
		else rSYS_ANALOG_REG1 &= ~(7 << 3);
	} else if (mode == ARKDISP_TVENC_OUT_CVBS_PAL || mode == ARKDISP_TVENC_OUT_CVBS_NTSC) {
		rSYS_ANALOG_REG1 &= ~(7 << 3);
	} else {
		if (enable) rSYS_ANALOG_REG1 |= 7 << 3;
		else rSYS_ANALOG_REG1 &= ~(7 << 3);	
	}
}

static void ark_disp_config_tvenc_ypbpr_timing(unsigned int mode)
{
	unsigned int tvmode = mode;
	tvenc_info_struct ypbpr_timing;

	ypbpr_timing.tHSW = (ypbpr_info[tvmode].hsw-1);
	ypbpr_timing.tHBP = 0x0;
	ypbpr_timing.tHFP = 0x0;
	ypbpr_timing.tCPL = (ypbpr_info[tvmode].width - 1);
	ypbpr_timing.tVSW = (ypbpr_info[tvmode].vsw - 1);
	ypbpr_timing.tVFP = 0x0;
	ypbpr_timing.tVBP = 0x0;
	if (ypbpr_info[tvmode].scan_mode == 1) {
		// interlace
		ypbpr_timing.tLPS = (ypbpr_info[tvmode].height/2- 1);
		ARKDISP_DBGPRTK("%s %d: interlace\n", __FUNCTION__, __LINE__);
	} else if (ypbpr_info[tvmode].scan_mode == 0) {
		// Progressive
		ypbpr_timing.tLPS = (ypbpr_info[tvmode].height- 1);
		ARKDISP_DBGPRTK("%s %d: progressive\n", __FUNCTION__, __LINE__);
	} else {
		ARKDISP_DBGPRTK("%s %d: invalid scan mode\n", __FUNCTION__, __LINE__);
		ypbpr_timing.tLPS = (ypbpr_info[tvmode].height- 1);
	}		
	ypbpr_timing.tIVS = 0x0;
	ypbpr_timing.tIHS = 0x0;
	ypbpr_timing.tIOE = 0x0;

	ARKDISP_DBGPRTK("%s %d:\n", __FUNCTION__, __LINE__);

	rLCD_TIMING0_TV =
		(ypbpr_timing.tHSW<<20)|
		(ypbpr_timing.tHBP<<10)|
		(ypbpr_timing.tHFP<<0);

	ARKDISP_DBGPRTK("%s %d:\n", __FUNCTION__, __LINE__);

	rLCD_TIMING1_TV =
		(ypbpr_timing.tVFP<<19)|
		(ypbpr_timing.tVSW<<13)|
		(ypbpr_timing.tCPL<<0);

	ARKDISP_DBGPRTK("%s %d:\n", __FUNCTION__, __LINE__);

	rLCD_TIMING2_TV =
		(ypbpr_timing.tIOE<<23)|
		(ypbpr_timing.tIHS<<22)|
		(ypbpr_timing.tIVS<<21)|
		(ypbpr_timing.tLPS<<10)|
		(ypbpr_timing.tVBP<<0);

	ARKDISP_DBGPRTK("%s %d:\n", __FUNCTION__, __LINE__);

	rLCD_TIMING_FRAME_START_CNT_TV =
		ypbpr_timing.tVSW/2;

	ARKDISP_DBGPRTK("%s %d:\n", __FUNCTION__, __LINE__);

	rLCD_TV_HV_DELAY = 39;

	ARKDISP_DBGPRTK("%s %d: end \n", __FUNCTION__, __LINE__);

}

static void ark_disp_enable_ypbpr_dac(void)
{
	rSYS_ANALOG_REG0 |= 3<<19; 
	rSYS_ANALOG_REG1 |= (1<<17) | 1;
}

static void ark_disp_config_ypbpr_mode(unsigned int mode)
{
	unsigned int tvmode = mode;

	// TV_CONTROL REG0x2b0
	rLCD_TV_CONTROL =
		(ypbpr_info[tvmode].scan_mode<<8)|
		// 1=interlace mode, 0=non-interlace mode
		// this field config the method for reading data from memory,
		// interlace or progressive (should cooperate with scaler)
		(1<<0); // 1=enable encoder (screen 2)

	if (tvmode <= 3) {
		// CLCD_YPBPR_CTRL0 REG0x304
		rLCD_YPBPR_CTRL0 =
			(1<<7)| // 1=SDTV, 0=HDTV
			(ypbpr_info[tvmode].scan_mode<<6)| // 0=P, 1=I
			(tvmode<<2)| // output_mode
			(1<<1)|// 1=select fixed output mode defined by output_mode above
			// 0=user can write the value of the mode
			(1<<0);// 1=enable ypbpr output 
	} else {
		rLCD_YPBPR_CTRL0 =
			(0<<7)|
			(ypbpr_info[tvmode].scan_mode<<6)|
			(tvmode<<2)|
			(1<<1)|
			(1<<0);
	}

	rLCD_YPBPR_CTRL0 |= (1 << 0);
}

static void ark_disp_select_tvenc_ypbpr_clk(unsigned int source_clk,
		unsigned int value)
{
	unsigned int int_tv_clk_sel = source_clk;
	unsigned int output_clk = value;
	unsigned int val;

	switch(int_tv_clk_sel)
	{
		case 1://int_tv_clk = syspll_clk
			printk("waiting updata!\n");
			break;
		case 2://int_tv_clk = clk_24m
			printk("waiting updata!\n");
			break;
		case 4://int_tv_clk = dds_clk:
			//SetDDSFrequency(297);

			switch(output_clk)
			{
				case OUT_13_5M:
					SetDDSFrequency(297);
					printk(" DDS: %d\n",GetDDSFrequency());
					printk("dds OUT_13_5M !\n");
					rSYS_DEVICE_CLK_CFG2 &=~(0xf<<20); 
					rSYS_DEVICE_CLK_CFG2 |=(0x4<<20); //int_tv_clk = dds_clk:

					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(22<<25);  //297M/22 = 13.5M     lcd_pixel_clk_sel=1: lcd_pixel_clk_dived
					break;
				case  OUT_27M:
					SetDDSFrequency(297);
					printk(" DDS: %d\n",GetDDSFrequency());
					printk("dds OUT_27M !\n");
					rSYS_DEVICE_CLK_CFG2 &=~(0xf<<20); 
					rSYS_DEVICE_CLK_CFG2 |=(0x4<<20); //int_tv_clk = dds_clk:

					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(11<<25);  // 297M/11 = 27M     lcd_pixel_clk_sel=1: lcd_pixel_clk_dived
					break;	
				case  OUT_74_25M:
					SetDDSFrequency(297);
					printk(" DDS: %d\n",GetDDSFrequency());
					printk("dds OUT_74_25M !\n");
					rSYS_DEVICE_CLK_CFG2 &=~(0xf<<20); 
					rSYS_DEVICE_CLK_CFG2 |=(0x4<<20); //int_tv_clk = dds_clk:

					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(4<<25);  // 297M/4 = 74.25M     lcd_pixel_clk_sel=1: lcd_pixel_clk_dived
					break;
				case  OUT_148_5M:
					SetDDSFrequency(297);
					printk(" DDS: %d\n",GetDDSFrequency());
					printk("dds OUT_148_5M !\n");
					rSYS_DEVICE_CLK_CFG2 &=~(0xf<<20); 
					rSYS_DEVICE_CLK_CFG2 |=(0x4<<20); //int_tv_clk = dds_clk:

					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(2<<25);  // 297M/2 = 148.5M     lcd_pixel_clk_sel=1: lcd_pixel_clk_dived
					break;
				default:
					break;
			}
			break;
		case 8://int_tv_clk = cpupll_clk
			printk("waiting updata!\n");
			break;
		default:
			break;
	}
}

static void ark_disp_config_tvenc_ypbpr_clk(unsigned int mode)
{
	unsigned int tvmode = mode;

	switch(tvmode)
	{
		case  i480hz60: // 13.5M
		case  i576hz50:
			ark_disp_select_tvenc_ypbpr_clk(YPBPR_SRC_CLK_DDS, OUT_13_5M);
			break;
		case  p480hz60: // 27M
		case  p576hz50:
			ark_disp_select_tvenc_ypbpr_clk(YPBPR_SRC_CLK_DDS, OUT_27M);
			break;
		case  p720hz60:
		case  p720hz50:
		case  i1080hz60:
		case  i1080hz50:
		case i1080hz50_1250: // 74.25M
			ark_disp_select_tvenc_ypbpr_clk(YPBPR_SRC_CLK_DDS,OUT_74_25M);
			break;
		case p1080hz60: // 148.5Mhz
		case p1080hz50:
			ark_disp_select_tvenc_ypbpr_clk(YPBPR_SRC_CLK_DDS, OUT_148_5M);
			break;
		default:
			break;
	}
}

void ark_disp_init_tvenc_ypbpr(unsigned int mode)
{
	ark_disp_tvenc_reset();
	ark_disp_config_tvenc_ypbpr_timing(mode);
	ark_disp_config_ypbpr_mode(mode);
	ark_disp_config_tvenc_interlace_out(0);
	ark_disp_config_tvenc_ypbpr_clk(mode);
	ark_disp_enable_ypbpr_dac();
}

static void ark_disp_config_tvenc_cvbs_timing(unsigned int mode)
{
	unsigned int tvmode = mode;
	tvenc_info_struct cvbs_timing;

	cvbs_timing.tHSW = (cvbs_info[tvmode].hsw-1);
	cvbs_timing.tHBP = 0x0;
	cvbs_timing.tHFP = 0x0;
	cvbs_timing.tCPL = (cvbs_info[tvmode].width - 1);
	cvbs_timing.tVSW = (cvbs_info[tvmode].vsw - 1);
	cvbs_timing.tVFP = 0x0;
	cvbs_timing.tVBP = 0x0;
	cvbs_timing.tLPS = (cvbs_info[tvmode].height/2- 1);
	printk("interlace \n");
	cvbs_timing.tIVS = 0x0;
	cvbs_timing.tIHS = 0x0;
	cvbs_timing.tIOE = 0x0;

	rLCD_TIMING0_TV =
		(cvbs_timing.tHSW<<20)|
		(cvbs_timing.tHBP<<10)|
		(cvbs_timing.tHFP<<0);
	rLCD_TIMING1_TV =
		(cvbs_timing.tVFP<<19)|
		(cvbs_timing.tVSW<<13)|
		(cvbs_timing.tCPL<<0);
	rLCD_TIMING2_TV =
		(cvbs_timing.tIOE<<23)|
		(cvbs_timing.tIHS<<22)|
		(cvbs_timing.tIVS<<21)|
		(cvbs_timing.tLPS<<10)|
		(cvbs_timing.tVBP<<0);
	rLCD_TIMING_FRAME_START_CNT_TV = cvbs_timing.tVSW/2;
	rLCD_TV_HV_DELAY = 39;
}

static void ark_disp_enable_tvenc_cvbs_dac(void)
{
	rSYS_ANALOG_REG1 |= (3<<22) | (1<<17) | (1<<15);
}

static void ark_disp_tvenc_cvbsss_init_ntsc(void)
{
	//TV encoder setting
	#define      chroma_freq_palbg              0x2a098acb  //pal
	#define      chroma_freq_palm               0x21e6efa4  //palm
	#define      chroma_freq_palnc              0x21f69446  //palnc
	#define      chroma_freq_ntsc               0x21f07c1f  //ntsc
	#define      chroma_phase                   0x2a
	#define      clrbar_sel                     0
	#define      clrbar_mode                    0
	#define      bypass_yclamp                  0
	#define      yc_delay                       4
	#define      cvbs_enable                    1
	#define      chroma_bw_1                    0 // bw_1,bw_0 : 00: narrow band; 01: wide band; 10: extra wide; 11: ultra wide.
	#define      chroma_bw_0                    1
	#define      comp_yuv                       0
	#define      compchgain                     0
	#define      hsync_width                    0x3f //0x7e*2
	#define      burst_width                    0x44   //pal 0x3e     ntsc 0x44
	#define      back_porch                     0x3b   //pal 0x45     ntsc 0x3b
	#define      cb_burst_amp                   0x20
	#define      cr_burst_amp                   0x00   //pal 0x20     ntsc 0x00
	#define      slave_mode                     0x1
	#define      black_level                    0xf2
	#define      blank_level                    0xf0
	#define      n1                             0x17
	#define      n3                             0x21
	#define      n8                             0x1b
	#define      n9                             0x1b
	#define      n10                            0x24
	#define      num_lines                      525   // pal: 625;   ntsc: 525.
	#define      n0                             0x3e
	#define      n13                            0x0f
	#define      n14                            0x0f
	#define      n15                            0x60
	#define      n5                             0x05
	#define      white_level                    0x320
	#define      cb_gain                        0x89
	#define      n20                            0x04
	#define      cr_gain                        0x89
	#define      n16                            0x1
	#define      n7                             0x2
	#define      tint                           0
	#define      n17                            0x0a
	#define      n19                            0x05
	#define      n18                            0x00
	#define      breeze_way                     0x16
	#define      n21                            0x3ff
	#define      front_porch                    0x10    //pal 0x0c    ntsc 0x10   ??
	#define      n11                            0x7ce
	#define      n12                            0x000
	#define      activeline                     1440
	#define      firstvideoline                 0xe
	#define      uv_order                       0
	#define      pal_mode                       0       //pal 0x1    ntsc 0x0 
	#define      invert_top                     0
	#define      sys625_50                      0
	#define      cphase_rst                     3
	#define      vsync5                         1
	#define      sync_level                     0x48
	#define      n22                            0
	#define      agc_pulse_level                0xa3
	#define      bp_pulse_level                 0xc8
	#define      n4                             0x15
	#define      n6                             0x05
	#define      n2                             0x15
	#define      vbi_blank_level                0x128
	#define      soft_rst                       0
	#define      row63                          0
	#define      row64                          0x07
	#define      wss_clock                      0x2f7
	#define      wss_dataf1                     0
	#define      wss_dataf0                     0
	#define      wss_linef1                     0
	#define      wss_linef0                     0
	#define      wss_level                      0x3ff
	#define      venc_en                        1
	#define      uv_first                       0
	#define      uv_flter_en                    1
	#define      notch_en                       0
	#define      notch_wide                     0
	#define      notch_freq                     0
	#define      row78                          0
	#define      row79                          0
	#define      row80                          0
	
	rLCD_TV_PARAM_REG0 = chroma_freq_ntsc ;//\u017dË\u017d\u0160¶\u0161Òå NÖÆ PÖÆ
	rLCD_TV_PARAM_REG1 = chroma_bw_1<<27 | comp_yuv<<26|compchgain<<24|yc_delay<<17|cvbs_enable<<16|clrbar_sel<<10|clrbar_mode<<9|
	        						  bypass_yclamp<<8 | chroma_phase ;
	rLCD_TV_PARAM_REG2 = cb_burst_amp<<24 | back_porch<<16 | burst_width<<8 | hsync_width;
	rLCD_TV_PARAM_REG3 = black_level<< 16 | slave_mode<<8 | cr_burst_amp ;
	rLCD_TV_PARAM_REG4 = n3<<24  | n1<<16 | blank_level ;
	rLCD_TV_PARAM_REG5 = n10<<24 | n9<<16 | n8 ;
	rLCD_TV_PARAM_REG6 = num_lines ;
	rLCD_TV_PARAM_REG7 = n15<<24 | n14<<16| n13<<8 | n0 ;
	rLCD_TV_PARAM_REG8 = cb_gain<<24 | white_level<<8 | n5 ;
	rLCD_TV_PARAM_REG9 = n7<<24      | n16 <<16 | cr_gain<<8 | n20 ;
	rLCD_TV_PARAM_REG10 = n18<<24     | n19 <<16 | n17<<8     | tint ;
	rLCD_TV_PARAM_REG11 = front_porch<<24     | n21<<8       | breeze_way ;
	rLCD_TV_PARAM_REG12 = n12 <<16     | n11 ;
	rLCD_TV_PARAM_REG13 = activeline ;
	rLCD_TV_PARAM_REG14 = n22<<24     | sync_level <<16 | uv_order<<15|pal_mode<<14|chroma_bw_0<<13|invert_top<<12|sys625_50<<11|
	  					  cphase_rst<<9|vsync5<<8     | firstvideoline ;
	rLCD_TV_PARAM_REG15 = n6<<24      | n4 <<16  | bp_pulse_level<<8     | agc_pulse_level ;
	rLCD_TV_PARAM_REG16 = soft_rst<<24| vbi_blank_level<<8    | n2 ;
	rLCD_TV_PARAM_REG17 = row64 <<16  | wss_clock ;
	rLCD_TV_PARAM_REG18 = wss_dataf1 ;
	rLCD_TV_PARAM_REG19 = wss_dataf0 ;
	rLCD_TV_PARAM_REG20 = wss_level <<16 | wss_linef0<<8     | wss_linef1 ;
	rLCD_TV_PARAM_REG21 = row80<<24 | row79<<16 | row78<<8 | venc_en<<7     | uv_first <<6 | uv_flter_en<<5  |notch_en<<4 |  notch_wide<<3 | notch_freq ;
}

static void ark_disp_tvenc_cvbs_init_pal(void)
{
	//TV encoder setting
	#define      chroma_freq_palbg              0x2a098acb  //pal
	#define      chroma_freq_palm               0x21e6efa4  //palm
	#define      chroma_freq_palnc              0x21f69446  //palnc
	#define      chroma_freq_ntsc               0x21f07c1f  //ntsc
	#define      chroma_phase                   0x2a
	#define      clrbar_sel                     0
	#define      clrbar_mode                    0
	#define      bypass_yclamp                  0
	#define      yc_delay                       4
	#define      cvbs_enable                    1
	#define      chroma_bw_1                    0 // bw_1,bw_0 : 00: narrow band; 01: wide band; 10: extra wide; 11: ultra wide.
	#define      chroma_bw_0                    1
	#define      comp_yuv                       0
	#define      compchgain                     0
	#define      hsync_width                    0x3f //0x7e*2
	#define      burst_width                    0x3e   //pal 0x3e     ntsc 0x44
	#define      back_porch                     0x45   //pal 0x45     ntsc 0x3b
	#define      cb_burst_amp                   0x20
	#define      cr_burst_amp                   0x20   //pal 0x20     ntsc 0x00
	#define      slave_mode                     0x1
	#define      black_level                    0xf2
	#define      blank_level                    0xf0
	#define      n1                             0x17
	#define      n3                             0x21
	#define      n8                             0x1b
	#define      n9                             0x1b
	#define      n10                            0x24
	#define      num_lines                      625   // pal: 625;   ntsc: 525.
	#define      n0                             0x3e
	#define      n13                            0x0f
	#define      n14                            0x0f
	#define      n15                            0x60
	#define      n5                             0x05
	#define      white_level                    0x320
	#define      cb_gain                        0x89
	#define      n20                            0x04
	#define      cr_gain                        0x89
	#define      n16                            0x1
	#define      n7                             0x2
	#define      tint                           0
	#define      n17                            0x0a
	#define      n19                            0x05
	#define      n18                            0x00
	#define      breeze_way                     0x16
	#define      n21                            0x3ff
	#define      front_porch                    0x0c    //pal 0x0c    ntsc 0x10 
	#define      n11                            0x7ce
	#define      n12                            0x000
	#define      activeline                     1440
	#define      firstvideoline                 0x0e    
	#define      uv_order                       0
	#define      pal_mode                       1       //pal 0x1    ntsc 0x0 
	#define      invert_top                     0
	#define      sys625_50                      0
	#define      cphase_rst                     3
	#define      vsync5                         1
	#define      sync_level                     0x48
	#define      n22                            0
	#define      agc_pulse_level                0xa3
	#define      bp_pulse_level                 0xc8
	#define      n4                             0x15
	#define      n6                             0x05
	#define      n2                             0x15
	#define      vbi_blank_level                0x128
	#define      soft_rst                       0
	#define      row63                          0
	#define      row64                          0x07
	#define      wss_clock                      0x2f7
	#define      wss_dataf1                     0
	#define      wss_dataf0                     0
	#define      wss_linef1                     0
	#define      wss_linef0                     0
	#define      wss_level                      0x3ff
	#define      venc_en                        1
	#define      uv_first                       0
	#define      uv_flter_en                    1
	#define      notch_en                       0
	#define      notch_wide                     0
	#define      notch_freq                     0
	#define      row78                          0
	#define      row79                          0
	#define      row80                          0
	
	rLCD_TV_PARAM_REG0 = chroma_freq_palbg;//chroma_freq_ntsc ;//\u017dË\u017d\u0160¶\u0161Òå NÖÆ PÖÆ
	rLCD_TV_PARAM_REG1 = chroma_bw_1<<27 | comp_yuv<<26|compchgain<<24|yc_delay<<17|cvbs_enable<<16|clrbar_sel<<10|clrbar_mode<<9|
	           														 bypass_yclamp<<8 | chroma_phase ;
	rLCD_TV_PARAM_REG2 = cb_burst_amp<<24 | back_porch<<16 | burst_width<<8 | hsync_width;
	rLCD_TV_PARAM_REG3 = black_level<< 16 | slave_mode<<8 | cr_burst_amp ;
	rLCD_TV_PARAM_REG4 = n3<<24  | n1<<16 | blank_level ;
	rLCD_TV_PARAM_REG5 = n10<<24 | n9<<16 | n8 ;
	rLCD_TV_PARAM_REG6 = num_lines ;
	rLCD_TV_PARAM_REG7 = n15<<24 | n14<<16| n13<<8 | n0 ;
	rLCD_TV_PARAM_REG8 = cb_gain<<24 | white_level<<8 | n5 ;
	rLCD_TV_PARAM_REG9 = n7<<24      | n16 <<16 | cr_gain<<8 | n20 ;
	rLCD_TV_PARAM_REG10 = n18<<24     | n19 <<16 | n17<<8     | tint ;
	rLCD_TV_PARAM_REG11 = front_porch<<24     | n21<<8       | breeze_way ;
	rLCD_TV_PARAM_REG12 = n12 <<16     | n11 ;
	rLCD_TV_PARAM_REG13 = activeline ;
	rLCD_TV_PARAM_REG14 = n22<<24     | sync_level <<16 | uv_order<<15|pal_mode<<14|chroma_bw_0<<13|invert_top<<12|sys625_50<<11|
	              										cphase_rst<<9|vsync5<<8     | firstvideoline ;
	rLCD_TV_PARAM_REG15 = n6<<24      | n4 <<16  | bp_pulse_level<<8     | agc_pulse_level ;
	rLCD_TV_PARAM_REG16 = soft_rst<<24| vbi_blank_level<<8    | n2 ;
	rLCD_TV_PARAM_REG17 = row64 <<16  | wss_clock ;
	rLCD_TV_PARAM_REG18 = wss_dataf1 ;
	rLCD_TV_PARAM_REG19 = wss_dataf0 ;
	rLCD_TV_PARAM_REG20 = wss_level <<16 | wss_linef0<<8     | wss_linef1 ;
	rLCD_TV_PARAM_REG21 = row80<<24 | row79<<16 | row78<<8 | venc_en<<7     | uv_first <<6 | uv_flter_en<<5  |notch_en<<4 |  notch_wide<<3 | notch_freq ;
}

static void ark_disp_config_tvenc_cvbs_mode(unsigned int mode) 
{
	if(mode == CVBS_NTSC) {
		rLCD_TV_CONTROL |= (1<<1);	
		ark_disp_tvenc_cvbsss_init_ntsc();
	} else if (mode == CVBS_PAL) {
		rLCD_TV_CONTROL &= ~(1<<1);
		ark_disp_tvenc_cvbs_init_pal();
	}
	rLCD_TV_CONTROL |= (1 << 8) | 1;
}

static void ark_disp_config_tvenc_cvbs_clk()
{
	unsigned int val;
	
	//config clk
	SetDDSFrequency(216);
	
	//set div to get 13.5M clk		
	val = rSYS_LCD_CLK_CFG;
	val &= ~(0x7f<<25);
	rSYS_LCD_CLK_CFG = val;	
	
	//select dds clk src
	val = rSYS_DEVICE_CLK_CFG2;                       
	val &= ~(0x1FF<<20);                              
	val |= (2<<20) | (4<<24) | (1<<28);               
	rSYS_DEVICE_CLK_CFG2 = val;  
}

void ark_disp_init_tvenc_cvbs(unsigned int mode)
{
	ark_disp_tvenc_reset();
	ark_disp_config_tvenc_cvbs_timing(mode);
	ark_disp_config_tvenc_cvbs_mode(mode);
	ark_disp_config_tvenc_interlace_out(0);
	ark_disp_config_tvenc_cvbs_clk();
	ark_disp_enable_tvenc_cvbs_dac();
}

static void ark_disp_select_tvenc_itu656out_pad(void)
{
	rSYS_PAD_CTRL00 &= ~(0xFFFFFF<<8);
	rSYS_PAD_CTRL00 |= (7<<8) | (7<<12) | (7<<16) | (7<<20) | (7<<24) | (7<<28);

	rSYS_PAD_CTRL01 &= ~(0xFF<<8);
	rSYS_PAD_CTRL01 |= (7<<8) | (7<<12);

	rSYS_PAD_CTRL03 &= ~(0xF<<4);
	rSYS_PAD_CTRL03 |= (7<<4);
}

static void ark_disp_config_tvenc_itu656out_timing(unsigned int mode)
{
	unsigned int tvmode = mode;
	tvenc_info_struct itu656out_timing;

	itu656out_timing.tHSW = (itu656out_info[tvmode].hsw-1);
	itu656out_timing.tHBP = 0x0;
	itu656out_timing.tHFP = 0x0;
	itu656out_timing.tCPL = (itu656out_info[tvmode].width - 1);
	itu656out_timing.tVSW = (itu656out_info[tvmode].vsw - 1);
	itu656out_timing.tVFP = 0x0;
	itu656out_timing.tVBP = 0x0;
	itu656out_timing.tLPS = (itu656out_info[tvmode].height/2- 1);
	printk("interlace\n");

	itu656out_timing.tIVS = 0x0;
	itu656out_timing.tIHS = 0x0;
	itu656out_timing.tIOE = 0x0;

	rLCD_TIMING0_TV =
		(itu656out_timing.tHSW<<20)|
		(itu656out_timing.tHBP<<10)|
		(itu656out_timing.tHFP<<0);
	rLCD_TIMING1_TV =
		(itu656out_timing.tVFP<<19)|
		(itu656out_timing.tVSW<<13)|
		(itu656out_timing.tCPL<<0);
	rLCD_TIMING2_TV =
		(itu656out_timing.tIOE<<23)|
		(itu656out_timing.tIHS<<22)|
		(itu656out_timing.tIVS<<21)|
		(itu656out_timing.tLPS<<10)|
		(itu656out_timing.tVBP<<0);
	rLCD_TIMING_FRAME_START_CNT_TV = itu656out_timing.tVSW/2;
}
#if 0
static void ark_disp_enable_tvenc_itu656out_dac(void)
{
	rSYS_ANALOG_REG1 |= 1<<17;// rgb DAC ʹ��
}
#endif

static void ark_disp_config_tvenc_itu656out_mode(unsigned int mode)
{
	unsigned int pn = mode;
	if (pn == ITU656OUT_PAL) {
		//		rLCD_TV_CONTROL = (0<<30) | (1<<8) | (1<<28) | (0<<18)  |  (5<<14)|(30<<9) |(28 <<4)|(0<<3) |(0<<2) |(0<<1) |(1<<0);
	} else if(pn == ITU656OUT_NTSC) {
		//        	rLCD_TV_CONTROL = (0<<30) | (1<<8)  | (1<<28)  |(33<<18)|  (3<<14)| (4<<9) |(28 <<4) |(0<<3) |(0<<2) |(1<<1)|(1<<0);
	} else {
		printk("Config PN error!\n");
	}

	rLCD_TV_CONTROL =
		(0<<10)| // 0=yuv used as ypbpr, 1=rgb, used as vga
		(0<<9)| // 0=use sw set timing value
		// 1=use default timing for n or p mode
		(1<<8)| // 1=interlace mode, 0=non-interlace mode
		// this field config the method for reading data from memory,
		// interlace or progressive (should cooperate with scaler)
		(1<<7)| // 0=tvencoder or cvbs or ypbpr or vga output
		// 1=only itu656 output
		(itu656out_info[pn].mode<<1)| // 1=ntsc, 0=pal
		(1<<0); // 1=enable encoder (screen 2)

	//steps to be implemented in ark_disp_set_tvenc_cfg()
	//    rLCD_VIDEO_CTL |= 1<<7;  //tv1: field mode
}
#if 0
static void ark_disp_config_tvenc_itu656out_clk(unsigned int source_clk)
{
	unsigned int int_tv_clk_sel = source_clk;
	unsigned int val;

	switch(int_tv_clk_sel)
	{
		case 1://int_tv_clk = syspll_clk
			val = rSYS_DEVICE_CLK_CFG2;   //int_tv_clk = syspll_clk
			val &= ~(0x1FF<<20);          //clk_54m = int_tv_clk/(clk_54m_div? clk_54m_div:1)
			val |= (1<<20) | (4<<24) | (1<<28); //clk_54m =216M(syspll)/4 = 54m
			rSYS_DEVICE_CLK_CFG2 = val;   

			// boot  set  syspll is 216M
			val = rSYS_LCD_CLK_CFG;
			val &= ~(1<<31); // clk_13p5m
			val &= ~(0x3f<<25);
			val |= (1<<24);// 1: ~ clk_27m  
			rSYS_LCD_CLK_CFG = val;
			printk("\n Sys pll OUT_27M! \n");	
			break;
		case 2://int_tv_clk = clk_24m
			printk("waiting updata!\n");
			break;
		case 4://int_tv_clk = dds_clk:
			SetDDSFrequency(216);
			printk(" DDS: %d\n",GetDDSFrequency());

			val = rSYS_DEVICE_CLK_CFG2;                      
			val &= ~(0x1FF<<20);    //clk_54m = int_tv_clk/(clk_54m_div? clk_54m_div:1)
			val |= (4<<20) | (4<<24) | (1<<28); //clk_54m =216M(DDS)/4 = 54m
			rSYS_DEVICE_CLK_CFG2 = val;   

			val = rSYS_LCD_CLK_CFG;
			val &= ~(1<<31); // clk_13p5m
			val &= ~(0x3f<<25);
			val |= (1<<24);// 1: ~ clk_27m  
			rSYS_LCD_CLK_CFG = val;
			printk("\n DDS OUT_27M! \n");	
			break;
		case 8://int_tv_clk = cpupll_clk
			printk("waiting updata!\n");
			break;
		default:
			break;
	}	
}
#endif

void ark_disp_init_tvenc_itu656out(unsigned int mode)
{
	unsigned int tvmode = mode;

	ark_disp_select_tvenc_itu656out_pad();
	ark_disp_config_tvenc_itu656out_timing(tvmode);
	//	ark_disp_enable_tvenc_itu656out_dac();
	ark_disp_config_tvenc_itu656out_mode(tvmode);
	//	ark_disp_config_tvenc_itu656out_clk(TU656_SRC_CLK_DDS);
}

void ark_disp_set_tvenc_itu656out(int tvmode)
{
	unsigned int  pic_width;
	unsigned int  pic_height;

	switch(tvmode)
	{
		case ITU656OUT_PAL:
			printk("ITU656OUT_PAL\n");
			pic_width = 720;
			pic_height = 576/2;
			ark_disp_init_tvenc_itu656out(ITU656OUT_PAL);
			break;
		case ITU656OUT_NTSC:
			printk("ITU656OUT_NTSC\n");
			pic_width = 720;
			pic_height = 480/2;
			ark_disp_init_tvenc_itu656out(ITU656OUT_NTSC);
			break;
		default:
			break;
	}
}

static void ark_disp_select_tvenc_vga_pad(void)
{
	rSYS_PAD_CTRL03 &= ~(0xFF<<8);
	rSYS_PAD_CTRL03 |= (5<<8) | (5<<12);
}

static void ark_disp_config_tvenc_vga_timing(unsigned int mode)
{
	unsigned int tvmode = mode;
	tvenc_info_struct vga_timing;

	vga_timing.tHSW = (vga_info[tvmode].hsw-1);
	vga_timing.tHBP = (vga_info[tvmode].hbp-1);
	vga_timing.tHFP = (vga_info[tvmode].hfp-1);
	vga_timing.tCPL = (vga_info[tvmode].width - 1);
	vga_timing.tVSW = (vga_info[tvmode].vsw - 1);
	vga_timing.tVFP = (vga_info[tvmode].vfp- 1);
	vga_timing.tVBP = (vga_info[tvmode].vbp- 1);
	vga_timing.tLPS = (vga_info[tvmode].height- 1);

	vga_timing.tIVS = 0x0;
	vga_timing.tIHS = 0x0;
	vga_timing.tIOE = 0x0;

	rLCD_TIMING0_TV =
		(vga_timing.tHSW<<20)|
		(vga_timing.tHBP<<10)|
		(vga_timing.tHFP<<0);
	rLCD_TIMING1_TV =
		(vga_timing.tVFP<<19)|
		(vga_timing.tVSW<<13)|
		(vga_timing.tCPL<<0);
	rLCD_TIMING2_TV =
		(vga_timing.tIOE<<23)|
		(vga_timing.tIHS<<22)|
		(vga_timing.tIVS<<21)|
		(vga_timing.tLPS<<10)|
		(vga_timing.tVBP<<0);
	rLCD_TIMING_FRAME_START_CNT_TV = vga_timing.tVSW/2;
}

static void ark_disp_enable_tvenc_vga_dac(void)
{
	rSYS_ANALOG_REG0 |= 3<<19;
	rSYS_ANALOG_REG1 |= (1<<22) | (1<<17) | (7<<3) | 1;	
}

static void ark_disp_config_tvenc_vga_mode(void)
{
	rLCD_TV_CONTROL  =
		(1<<31)|
		(1<<0);
	rLCD_YPBPR_CTRL0 = (1<<0);
}

static void  ark_disp_select_tvenc_vga_clk(unsigned int source_clk,
		unsigned int value)
{
	unsigned int int_tv_clk_sel = source_clk;
	unsigned int output_clk = value;

	switch(int_tv_clk_sel)
	{
		case 1://int_tv_clk = syspll_clk	
			printk("waiting updata!\n");
			break;
		case 2://int_tv_clk = clk_24m
			printk("waiting updata!\n");
			break;
		case 4://int_tv_clk = dds_clk:
			rSYS_DEVICE_CLK_CFG2 &=~(0xf<<20); 
			rSYS_DEVICE_CLK_CFG2 |=(0x4<<20); //int_tv_clk = dds_clk:

			switch(output_clk)
			{
				case VGA_CLK_25_175M:
					SetDDSFrequency(200);
					printk(" DDS: %d,VGA_CLK_25_175M\n",GetDDSFrequency());
					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(8<<25);  // 200M/8 = 25M     lcd_pixel_clk_sel=1: lcd_pixel_clk_dived
					break;
				case VGA_CLK_40M:
					SetDDSFrequency(200);
					printk(" DDS: %d,VGA_CLK_40M\n",GetDDSFrequency());
					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(5<<25);  // 200M/5 = 40M    
					break;
				case VGA_CLK_65M:
					SetDDSFrequency(195);
					printk(" DDS: %d,VGA_CLK_65M\n",GetDDSFrequency());
					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(3<<25);  // 195M/3 = 65M     
					break;
				case VGA_CLK_74_25M:
					SetDDSFrequency(297);
					printk(" DDS: %d,VGA_CLK_74_25M\n",GetDDSFrequency());
					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(4<<25);  // 297M/4 = 74.25M     
					break;
				case VGA_CLK_108M:
					SetDDSFrequency(216);
					printk(" DDS: %d,VGA_CLK_108M\n",GetDDSFrequency());
					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(2<<25);  // 216M/2 = 108M    
					break;
				case VGA_CLK_154M:
					SetDDSFrequency(308);
					printk(" DDS: %d,VGA_CLK_154M\n",GetDDSFrequency());
					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(2<<25);  // 308M/2 = 154M    
					break;
				case VGA_CLK_135M:
					SetDDSFrequency(270);
					printk(" DDS: %d,VGA_CLK_135M\n",GetDDSFrequency());
					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(2<<25);  //270M/2 = 135M   
					break;
				case VGA_CLK_148_4M:
					SetDDSFrequency(297);
					printk(" DDS: %d,VGA_CLK_148_4M\n",GetDDSFrequency());
					rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
					rSYS_LCD_CLK_CFG |= (1<<31)|(2<<25);  // 297M/2 = 148.4M   
					break;
				default:
					break;
			}
			break;
		case 8://int_tv_clk = cpupll_clk
			printk("waiting updata!\n");
			break;
		default:
			break;
	}
}

static void ark_disp_config_tvenc_vga_clk(unsigned int mode)
{
	unsigned int tvmode = mode;

	switch(tvmode)
	{
		case VGA640x480hz60://25.175M 
			ark_disp_select_tvenc_vga_clk(VGA_SRC_CLK_DDS,VGA_CLK_25_175M);
			break;
		case VGA800x600hz60://40M 
			ark_disp_select_tvenc_vga_clk(VGA_SRC_CLK_DDS,VGA_CLK_40M);
			break;
		case VGA1024x768hz60://65M 
			ark_disp_select_tvenc_vga_clk(VGA_SRC_CLK_DDS,VGA_CLK_65M);
			break;
		case VGA1280x720hz60://74.250M  
			ark_disp_select_tvenc_vga_clk(VGA_SRC_CLK_DDS,VGA_CLK_74_25M);
			break;
		case VGA1280x1024hz60:// 108M 
			ark_disp_select_tvenc_vga_clk(VGA_SRC_CLK_DDS,VGA_CLK_108M);
			break;
		case VGA1900x1200hz60:// 154Mhz 60hz 
			ark_disp_select_tvenc_vga_clk(VGA_SRC_CLK_DDS,VGA_CLK_154M);
			break;
		case VGA1280x1024hz75://135Mdds  
			ark_disp_select_tvenc_vga_clk(VGA_SRC_CLK_DDS,VGA_CLK_135M);
			break;
		case VGA1280x960hz85://// 148.4M 
			ark_disp_select_tvenc_vga_clk(VGA_SRC_CLK_DDS,VGA_CLK_148_4M);
			break;
		default:
			break;
	}
}

void ark_disp_init_tvenc_vga(unsigned int  mode)
{
	unsigned int  tvmode = mode;

	ark_disp_select_tvenc_vga_pad();
	ark_disp_config_tvenc_vga_timing(tvmode);
	ark_disp_config_tvenc_vga_mode();
	ark_disp_config_tvenc_vga_clk(tvmode);
	ark_disp_enable_tvenc_vga_dac();
}

void ark_disp_set_tvenc_vga(int tvmode)
{
	unsigned int  pic_width;
	unsigned int  pic_height;

	switch(tvmode)
	{
		case VGA640x480hz60:
			printk("VGA640x480hz60\n");
			pic_width = 640;
			pic_height = 480;
			ark_disp_init_tvenc_vga(VGA640x480hz60);
			break;
		case VGA800x600hz60:
			printk("VGA800x600hz60\n");
			pic_width = 800;
			pic_height = 600;
			ark_disp_init_tvenc_vga(VGA800x600hz60);
			break;
		case VGA1024x768hz60:
			printk("VGA1024x768hz60\n");
			pic_width = 1024;
			pic_height = 768;
			ark_disp_init_tvenc_vga(VGA1024x768hz60);
			break;
		case VGA1280x1024hz60:
			printk("VGA1280x1024hz60\n");
			pic_width = 1280;
			pic_height = 1024;
			ark_disp_init_tvenc_vga(VGA1280x1024hz60);
			break;
		default:
			break;
	}
}

int ark_disp_set_tvenc_out_mode(int out_mode)
{
	switch (out_mode)
	{
		case ARKDISP_TVENC_OUT_YPBPR_I480HZ60:
		case ARKDISP_TVENC_OUT_YPBPR_I576HZ50:
		case ARKDISP_TVENC_OUT_YPBPR_P480HZ60:
		case ARKDISP_TVENC_OUT_YPBPR_P576HZ50:
		case ARKDISP_TVENC_OUT_YPBPR_P720HZ60:
		case ARKDISP_TVENC_OUT_YPBPR_P720HZ50:
		case ARKDISP_TVENC_OUT_YPBPR_I1080HZ60:
		case ARKDISP_TVENC_OUT_YPBPR_I1080HZ50:
		case ARKDISP_TVENC_OUT_YPBPR_I1080HZ50_1250:
		case ARKDISP_TVENC_OUT_YPBPR_P1080HZ60:
		case ARKDISP_TVENC_OUT_YPBPR_P1080HZ50:    
			ark_disp_init_tvenc_ypbpr(out_mode - ARKDISP_TVENC_OUT_YPBPR_I480HZ60);
			break;
		case ARKDISP_TVENC_OUT_CVBS_PAL:
		case ARKDISP_TVENC_OUT_CVBS_NTSC:
			ark_disp_init_tvenc_cvbs(out_mode - ARKDISP_TVENC_OUT_CVBS_PAL);
			break;
		case ARKDISP_TVENC_OUT_ITU656_PAL:
		case ARKDISP_TVENC_OUT_ITU656_NTSC:
			ark_disp_init_tvenc_itu656out(out_mode - ARKDISP_TVENC_OUT_ITU656_PAL);
			break;
		case ARKDISP_TVENC_OUT_VGA_640x480HZ60:
		case ARKDISP_TVENC_OUT_VGA_800x600HZ60:
		case ARKDISP_TVENC_OUT_VGA_1024x768HZ60:
		case ARKDISP_TVENC_OUT_VGA_1280x1024HZ60:
		case ARKDISP_TVENC_OUT_VGA_1900x1200HZ60: // bandwidth  limit
		case ARKDISP_TVENC_OUT_VGA_1280x1024HZ75:
		case ARKDISP_TVENC_OUT_VGA_1280x960HZ85: // bandwidth  limit
		case ARKDISP_TVENC_OUT_VGA_1280x720HZ60:
			ark_disp_init_tvenc_vga(out_mode - ARKDISP_TVENC_OUT_VGA_640x480HZ60);
			break;
		default:
			return -1;
	}
	return 0;
}

