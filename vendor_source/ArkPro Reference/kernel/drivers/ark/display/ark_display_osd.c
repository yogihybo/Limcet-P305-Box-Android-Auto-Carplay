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
 *      ark_display_osd.c
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

#include "ark_display.h"
#include "ark_display_priv.h"
#include "ark_lcd_regs.h"
#include "ark_sys_regs.h"

__attribute((aligned(4))) const unsigned int PALETTE_2_320240[2] = 
{0x000000,0xffffff};

__attribute((aligned(4))) const unsigned int PALETTE_4_640320[4] = 
{0x0000ff,0xffffff,0xff0000,0x00ff00};

__attribute((aligned(4))) const unsigned int PALETTE_16_720480[16] = 
{
0xff00ff,0x880088,0x0000ff,0x000080,0x00ffff,0x008080,0x00ff00,0x008000,
0xffff00,0x808000,0xff0000,0x800000,0xffffff,0xc0c0c0,0x808080,0x000000
};

__attribute((aligned(4))) const unsigned int PALETTE_256_PLACE_800480[256] =
{
  0x000000,0x800000,0x008000,0x808000,0x000080,0x800080,0x008080,0xc0c0c0,0xc0dcc0,0xa6caf0,0x402000,0x602000,0x802000,0xa02000,0xc02000,0xe02000,
  0x004000,0x204000,0x404000,0x604000,0x804000,0xa04000,0xc04000,0xe04000,0x006000,0x206000,0x406000,0x606000,0x806000,0xa06000,0xc06000,0xe06000,
  0x008000,0x208000,0x408000,0x608000,0x808000,0xa08000,0xc08000,0xe08000,0x00a000,0x20a000,0x40a000,0x60a000,0x80a000,0xa0a000,0xc0a000,0xe0a000,
  0x00c000,0x20c000,0x40c000,0x60c000,0x80c000,0xa0c000,0xc0c000,0xe0c000,0x00e000,0x20e000,0x40e000,0x60e000,0x80e000,0xa0e000,0xc0e000,0xe0e000,
  0x000040,0x200040,0x400040,0x600040,0x800040,0xa00040,0xc00040,0xe00040,0x002040,0x202040,0x402040,0x602040,0x802040,0xa02040,0xc02040,0xe02040,
  0x004040,0x204040,0x404040,0x604040,0x804040,0xa04040,0xc04040,0xe04040,0x006040,0x206040,0x406040,0x606040,0x806040,0xa06040,0xc06040,0xe06040,
  0x008040,0x208040,0x408040,0x608040,0x808040,0xa08040,0xc08040,0xe08040,0x00a040,0x20a040,0x40a040,0x60a040,0x80a040,0xa0a040,0xc0a040,0xe0a040,
  0x00c040,0x20c040,0x40c040,0x60c040,0x80c040,0xa0c040,0xc0c040,0xe0c040,0x00e040,0x20e040,0x40e040,0x60e040,0x80e040,0xa0e040,0xc0e040,0xe0e040,
  0x000080,0x200080,0x400080,0x600080,0x800080,0xa00080,0xc00080,0xe00080,0x002080,0x202080,0x402080,0x602080,0x802080,0xa02080,0xc02080,0xe02080,
  0x004080,0x204080,0x404080,0x604080,0x804080,0xa04080,0xc04080,0xe04080,0x006080,0x206080,0x406080,0x606080,0x806080,0xa06080,0xc06080,0xe06080,
  0x008080,0x208080,0x408080,0x608080,0x808080,0xa08080,0xc08080,0xe08080,0x00a080,0x20a080,0x40a080,0x60a080,0x80a080,0xa0a080,0xc0a080,0xe0a080,
  0x00c080,0x20c080,0x40c080,0x60c080,0x80c080,0xa0c080,0xc0c080,0xe0c080,0x00e080,0x20e080,0x40e080,0x60e080,0x80e080,0xa0e080,0xc0e080,0xe0e080,
  0x0000c0,0x2000c0,0x4000c0,0x6000c0,0x8000c0,0xa000c0,0xc000c0,0xe000c0,0x0020c0,0x2020c0,0x4020c0,0x6020c0,0x8020c0,0xa020c0,0xc020c0,0xe020c0,
  0x0040c0,0x2040c0,0x4040c0,0x6040c0,0x8040c0,0xa040c0,0xc040c0,0xe040c0,0x0060c0,0x2060c0,0x4060c0,0x6060c0,0x8060c0,0xa060c0,0xc060c0,0xe060c0,
  0x0080c0,0x2080c0,0x4080c0,0x6080c0,0x8080c0,0xa080c0,0xc080c0,0xe080c0,0x00a0c0,0x20a0c0,0x40a0c0,0x60a0c0,0x80a0c0,0xa0a0c0,0xc0a0c0,0xe0a0c0,
  0x00c0c0,0x20c0c0,0x40c0c0,0x60c0c0,0x80c0c0,0xa0c0c0,0xfffbf0,0xa0a0a4,0x808080,0xff0000,0x00ff00,0xffff00,0x0000ff,0xff00ff,0x00ffff,0xffffff
};

__attribute((aligned(4))) const unsigned int PALETTE_800480_256_MICKEY[256] =
{
  0xff00ff,0xffffff,0xffcf00,0x6699ff,0x000000,0x211c1d,0x0e070a,0x351e28,0xf2a7c8,0x694957,0xd36999,0x8b4565,0xe874a9,0x69384e,0xa5597b,0x542e3f,
  0xb77493,0x7c5567,0xdd98b7,0x3f2833,0x1d1017,0x7f94ee,0x7f93ee,0x8095ef,0x7991e9,0x7b93ec,0x8099f1,0x8097f0,0x748fe6,0x7a99ee,0x82a0f6,0x819ef4,
  0x6687dc,0x6a8bdf,0x6d8fe3,0x6f92e6,0x7597eb,0x7c9df1,0x81a4f9,0x83a6fb,0x83a4f9,0x82a4f8,0x82a3f7,0x7296e9,0x779bef,0x81a5fa,0x80a3ef,0x83a4ed,
  0x7996d6,0x819cd6,0x6598fc,0x6090f0,0x5a86e0,0x4d73c0,0x6596f9,0x547dd0,0x354f83,0x6799fd,0x6494f5,0x5f8de8,0x699afd,0x5982d4,0x6a9af9,0x6694f0,
  0x6c9cfd,0x638fe5,0x6e9efd,0x6692ea,0x5e85d7,0x1b273e,0x6d9bf7,0x6089da,0x6f9dfb,0x638cdd,0x658ee0,0x6992e4,0x6890e1,0x729ff5,0x6d96e9,0x6b94e5,
  0x6d95e6,0x7099ea,0x6e96e4,0x688cd7,0x739bec,0x769fef,0x7aa2f3,0x749be7,0x7297e4,0x7fa8fa,0x7da6f6,0x82abfd,0x81aafb,0x80a9fa,0x80a9f9,0x83acfd,
  0x81aaf9,0x799ee9,0x82a9f9,0x7091d6,0x83abf9,0x82aaf8,0x82aaf7,0x7da0ea,0x81a4ee,0x80a3ec,0x82a7ee,0x82a5ed,0x82a4eb,0x83a5ec,0x82a3e9,0x73a4fa,
  0x73a3fa,0x6e9bf0,0x75a5fc,0x75a4fa,0x74a3f9,0x76a5fb,0x76a4fa,0x78a6fc,0x7199e5,0x83acfa,0x89aceb,0x8dade7,0x293e62,0x0d141f,0x4567a1,0x21324d,
  0x141e2e,0x769fe2,0x7ba4e6,0x7fa9eb,0x6e8dc1,0x90aee0,0x060a10,0x80aae9,0x80a9e6,0x070a0e,0x7fa9e4,0x88a3c7,0x5a86ac,0x76afe0,0x6ea4d2,0x6799c4,
  0x3b5468,0x4d6776,0x739098,0x2c373a,0x8baab3,0x9dbec7,0xa5d0da,0xb1dee9,0x9fa2a0,0xa7ac14,0xdae11b,0xccd219,0xbec417,0xb7b47b,0x9f8b05,0xc7ba60,
  0x68590b,0xe5c527,0xe9ca2c,0xd8bf40,0xceb949,0xe2cb58,0xfdcd00,0xfbcc00,0xf8ca00,0xf0c300,0xc09c00,0xfecf01,0xfdce01,0xf4c601,0xe0b502,0xd0a902,
  0xfdce03,0xe7ba03,0xf9cb04,0xfdce05,0xd6ae05,0xf8cb09,0xfdcf0a,0xe9bc0a,0xfdd00e,0xd8b20c,0xf5ca0f,0xfdd010,0xf1c411,0xdeb710,0xf1c81a,0xe2bc18,
  0x3f3407,0xe9c21b,0xedc721,0xf2cc26,0xfcd52b,0xf7d12b,0xfcd52e,0xfdd732,0xe0c135,0xf1d040,0xfdc503,0xfbc303,0xfcc404,0xefbd04,0xfbc405,0xf6c005,
  0xfbc407,0x866a04,0xfbc508,0xfdc709,0xfbc609,0xfbc60a,0xf1c10a,0xfdc80c,0xfbc70c,0xf7c40c,0xfcce1a,0xfcc303,0xfbc304,0xfdc506,0x0e0b01,0xf3e9cc,
  0x3e3d3a,0x110c00,0x2e2203,0x1f1702,0x7a7771,0x2e2d2b,0x4e4941,0x65625d,0xf29100,0xdd8400,0xb66c00,0x7a4900,0xa97428,0xf5a83b,0xe09a36,0xb5ac9e,
  0xc7882f,0x8d8376,0xebdbc6,0xdbccb8,0x9a9287,0x3e2100,0x562e01,0xfdead5,0xc5beb6,0x59544f,0x050200,0x0b0400,0x0f0d0d,0xefefef,0xd7d7d7,0x000000
};


void ark_disp_set_osd_format_2(int id, unsigned int format)
{
    /* Definition of osd layer data format in register
     *    0: palette
     *    1: bmp(24bit)
     *    2: yuyv
     *    3: yuv
     *    4: rgb_i555
     *    5: rgb_565
     *    6: rgb_a888
     *    7: rgb_888
     *    8: rgba1555
     *    9: rgb_a1888
     *    10: rgb_a4888
     *    11: rgb_666
     *    12: rgb_a1666
     */
    if (id == 0) {
        /* CLCD_OSD1_CTL Reg0x074 */
        rLCD_OSD1_CTL &= ~(0xF << 12);   /* clear bit 15-12 */
        rLCD_OSD1_CTL |= (format << 12); /* 15-12: osd layer data format */
    } else if (id == 1) {
        /* CLCD_OSD2_CTL REG0x88 */
        rLCD_OSD2_CTL &= ~(0xF << 12);   /* clear bit 15-12 */
        rLCD_OSD2_CTL |= (format << 12); /* 15-12: osd layer data format */
    } else if (id == 2) {
         /* CLCD_OSD3_CTL REG0x98 */
        rLCD_OSD3_CTL &= ~(0xF << 12);   /* clear bit 15-12 */
        rLCD_OSD3_CTL |= (format << 12); /* 15-12: osd layer data format */
    }
}   

/* Definition of osd layer data format input to registers
 *    0: palette
 *    1: bmp(24bit)
 *    2: yuyv
 *    3: yuv
 *    4: rgb_i555
 *    5: rgb_565
 *    6: rgb_a888
 *    7: rgb_888
 *    8: rgba1555
 *    9: rgb_a1888
 *    10: rgb_a4888
 *    11: rgb_666
 *    12: rgb_a1666
 */
void ark_disp_set_osd_format(int id, unsigned int format,
    unsigned int rgb_order, unsigned int yuyv_order)
{
    unsigned int yuv_ycbcr_bypass, rgb_ycbcr_bypass;

    switch (format) {
    case ARK_DISP_OSD_PIXFMT_YUYV:
        yuv_ycbcr_bypass = 1;
        rgb_ycbcr_bypass = 1;	
        break;
    case ARK_DISP_OSD_PIXFMT_YUV:
        yuv_ycbcr_bypass = 1;
        rgb_ycbcr_bypass = 1; // 1=no rgb to ycbcr conversion		
        break;
    case ARK_DISP_OSD_PIXFMT_RGBA888:
    case ARK_DISP_OSD_PIXFMT_RGB888:
    case ARK_DISP_OSD_PIXFMT_RGB565:
    case ARK_DISP_OSD_PIXFMT_RGBI555:
        yuv_ycbcr_bypass = 1;
        rgb_ycbcr_bypass = 0;	
        break;
    default:
        yuv_ycbcr_bypass = 1; // 1=no yuv to ycbcr conversion
        rgb_ycbcr_bypass = 0; // 0=enable rgb to ycbcr conversion
        break;
    }

    /* set osd layer source data format */
    ark_disp_set_osd_format_2(id, format);
    if (id == 0) {
        /* set osd1 data buffer start addr group0 in memory */
        //set_win1_baseAddr((UINT32 )buf);	

        /* CLCD_OSD1_CTL Reg0x074 */
        rLCD_OSD1_CTL &= ~(0x7F << 16); // clear bit 22-16
        // 22-21: 00=yuyv
        // 20-18: 000=rgb 
        // 17: 0=enable yuv to ycbcr
        // 16: 0=enable rgb to ycbcr

        rLCD_OSD1_CTL |=
            yuyv_order << 21 |
            rgb_order << 18 |
            yuv_ycbcr_bypass << 17 |
            rgb_ycbcr_bypass << 16;
    } else if (id == 1) {
        /* set osd2 data buffer addr group0 in memory */
        //set_win2_baseAddr((UINT32 )buf);

        /* CLCD_OSD2_CTL REG0x88 */
        rLCD_OSD2_CTL &= ~(0x7F << 16); // clear bit 22-16
        // 22-21: 00=yuyv
        // 20-18: 000=rgb 
        // 17: 0=enable yuv to ycbcr
        // 16: 0=enable rgb to ycbcr

        rLCD_OSD2_CTL |=
            yuyv_order << 21 |
            rgb_order << 18 |
            yuv_ycbcr_bypass << 17 |
            rgb_ycbcr_bypass << 16;
    } else if (id == 2) {
        /* set osd3 data buffer addr group0 in memory */
        //set_win3_baseAddr((UINT32 )buf);

        // CLCD_OSD3_CTL REG0x98
        rLCD_OSD3_CTL &= ~(0x7F << 16); // clear bit 22-16
        // 22-21: 00=yuyv
        // 20-18: 000=rgb 
        // 17: 0=enable yuv to ycbcr
        // 16: 0=enable rgb to ycbcr

        rLCD_OSD3_CTL |=
            yuyv_order << 21 |
            rgb_order << 18 |
            yuv_ycbcr_bypass << 17 |
            rgb_ycbcr_bypass << 16;
    }
}

void ark_disp_set_osd_source_size(int id, int width, int height)
{
    switch (id)
    {
    case 0:
        /* CLCD_OSD1_SOURCE_SIZE REG0x170 */
        rLCD_OSD1_SOURCE_SIZE =
            height<<12 | // 23-12: height of osd1 source image in memory
            width;;      // 11-0: width of osd1 source image in memory
        break;
    case 1:
        /* CLCD_OSD2_SOURCE_SIZE REG0x174 */
        rLCD_OSD2_SOURCE_SIZE =
            height<<12 | // 23-12: height of osd2 source image in memory
            width;       // 11-0: width of osd2 source image in memory
        break;
    case 2:
        /* CLCD_OSD3_SOURCE_SIZE REG0x178 */
        rLCD_OSD3_SOURCE_SIZE =
            height<<12 | // 23-12: height of osd3 layer on display
            width;       // 11-0: width of osd3 layer on display
        break;
    default:
        break;
    }
}

void ark_disp_set_osd_layer_size(int id, int width, int height)
{
    switch (id)
    {
    case 0:
        /* CLCD_OSD1_SIZE REG0x78 */
        rLCD_OSD1_SIZE =
            height<<12 | // 23-12: height of osd1 layer on display
            width;       // 11-0: width of osd1 layer on display
        break;
    case 1:
        /* CLCD_OSD2_SIZE REG0x8c */
        rLCD_OSD2_SIZE =
            height<<12 | // 23-12: height of osd2 layer on display
            width;       // 11-0: width of osd2 layer on display
        break;
    case 2:
        /* CLCD_OSD3_SIZE REG0x9c */
        rLCD_OSD3_SIZE =
            height<<12 | // 23-12: height of osd3 layer on display
            width;       // 11-0: width of osd3 layer on display
        break;
    default:
        break;
    }
}

void ark_disp_set_osd_layer_position(int id, int x, int y)
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
    if (y < 0) {
        y_sign = 1;
        y_start = 0 - y;
    } else {
        y_sign = 0;
        y_start = y;
    }

    switch (id)
    {
    case 0:
        /* CLCD_OSD1_POSITION REG0x7c */
        rLCD_OSD1_POSITION =
            y_sign << 25 |  // sign of y
            y_start << 13 | // 24-13: y value of osd1 position on display
            x_sign << 12 |  // sign of x
            x_start << 0;   // 11-0: x value of osd1 position on display
        break;
    case 1:
        /* CLCD_OSD2_POSITION REG0x90 */
        rLCD_OSD2_POSITION =
            y_sign << 25 |  // sign of y
            y_start << 13 | // 24-13: y value of osd2 position on display
            x_sign << 12 |  // sign of x
            x_start << 0;   // 11-0: x value of osd2 position on display
        break;
    case 2:
        /* CLCD_OSD3_POSITION REG0xa0 */
        rLCD_OSD3_POSITION =
            y_sign << 25 |  // sign of y
            y_start << 13 | // y value of osd3 layer position on display
            x_sign << 12 |  // sign of x
            x_start << 0;   // x value of osd3 layer position on display
        break;
    default:
        break;
    }
}

void ark_disp_set_osd_win_point(int id, int x, int y)
{
    switch (id)
    {
    case 0:
        /* CLCD_OSD1_WINDOW_POINT REG0x160 */
        rLCD_OSD1_WIN_POINT =
            y << 12 | // 23-12: y value of osd1 window start point 
            x;        // 11-0: x value of osd1 window start point
        break;
    case 1:
        /* CLCD_OSD2_WINDOW_POINT REG0x164 */
        rLCD_OSD2_WIN_POINT =
            y << 12 | // 23-12: y value of osd2 window start point
            x;        // 11-0: x value of osd2 window start point
        break;
    case 2:
        /* CLCD_OSD3_WINDOW_POINT REG0x168 */
        rLCD_OSD3_WIN_POINT =
            y << 12 | // 23-12: y value of osd3 window start point
            x;        // 11-0: x value of osd3 window start point
        break;
    default:
        break;
    }
    /* NOTE: the above set the start point of osd window on the source
     * osd image in memory
     */
}

void ark_disp_set_osd_priority_lcd(int id, int level)
{
    unsigned int pos;

    switch (id)
    {
    case 0: /* osd1 */
        pos = 8;  // 10-8: osd1 bld_priority
        break;
    case 1: /* osd2 */
        pos = 16; // 18-16: osd2 bld_priority 
        break;
    case 2: /* osd3 */
        pos = 24; // 26-24: osd3 bld_priority 
        break;
    default:
        return;
    }

    /* CLCD_BLD_MODE_LCD_REG0 REG0x060 */
    rLCD_BLD_MODE_LCD_REG0 &= ~(0x7 << pos);  // clear priority bits
    rLCD_BLD_MODE_LCD_REG0 |= (level << pos); // set blending priority of osd
}

void ark_disp_set_osd_priority_tvenc(int id, int level)
{
    unsigned int pos;

    switch (id)
    {
    case 0: /* osd1 */
        pos = 8;  // 10-8: osd1 bld_priority
        break;
    case 1: /* osd2 */
        pos = 16; // 18-16: osd2 bld_priority 
        break;
    case 2: /* osd3 */
        pos = 24; // 26-24: osd3 bld_priority 
        break;
    default:
        return;
    }

    /* CLCD_BLD_MODE_TV_REG0 REG0x068 */
    rLCD_BLD_MODE_TV_REG0 &= ~(0x7 << pos);  // clear priority bits
    rLCD_BLD_MODE_TV_REG0 |= (level << pos); // set blending priority of osd
}

void ark_disp_set_osd_clk_sel(int id, unsigned int sel)
{
    //chitest TBD ...
    unsigned int pos;

    switch (id)
    {
    case 0: /* osd1 */
        pos = 13;
        break;
    case 1: /* osd2 */
        pos = 12;
        break;
    case 2: /* osd3 */
        pos = 11;
        break;
    default:
        return;
    }
    if (sel)
        rSYS_LCD_CLK_CFG |= (1 << pos);  /* osd_sel = 1, lcd_pixel_clk */
    else
        rSYS_LCD_CLK_CFG &= ~(1 << pos); /* osd_sel = 0, lcd_clk */
}

unsigned int ark_disp_get_osd_en_lcd(int id)
{
    unsigned int pos;

    /* CLCD_CONTROL REG0x04 */
    switch (id)
    {
    case 0: // osd1
        pos = 7;
        break;
    case 1: // osd2
        pos = 8;
        break;
    case 2: // osd3
        pos = 9;
        break;
    default: // osd1 (by default)
        pos = 7;
        break;
    }
    return ((rLCD_CONTROL >> pos) & 0x1);
}

void ark_disp_set_osd_en_lcd(int id, unsigned int enable)
{
    unsigned int pos;

    /* CLCD_CONTROL REG0x04 */
    switch (id)
    {
    case 0: // osd1
        pos = 7;
        break;
    case 1: // osd2
        pos = 8;
        break;
    case 2: // osd3
        pos = 9;
        break;
    default:
        return;
    }
    if (enable)
        rLCD_CONTROL |= (1 << pos);  /* enable osd layer on LCD screen */
    else
        rLCD_CONTROL &= ~(1 << pos); /* disable osd layer on LCD screen */
}

unsigned int ark_disp_get_osd_en_tvenc(int id)
{
    unsigned int pos;

    /* TV_CONTROL REG0x2b0 */
    switch (id)
    {
    case 0: // osd1
        pos = 4;
        break;
    case 1: // osd2
        pos = 5;
        break;
    case 2: // osd3
        pos = 6;
        break;
    default: // osd1 (by default)
        pos = 4;
        break;
    }
    return ((rLCD_TV_CONTROL >> pos) & 0x1);
}

void ark_disp_set_osd_en_tvenc(int id, unsigned int enable)
{
    unsigned int pos;

    /* CLCD_CONTROL REG0x04 */
    switch (id)
    {
    case 0: // osd1
    	if (enable) {
			rSYS_LCD_CLK_CFG |=(1<<13);
			rLCD_TV_CONTROL |= 1<<4;	
			rLCD_CONTROL &= ~(1 << 7); 
		} else {
			rSYS_LCD_CLK_CFG &=~(1<<13);
			rLCD_TV_CONTROL &=~(1<<4);		
		}
        break;
    case 1: // osd2
    	if (enable) {
			rSYS_LCD_CLK_CFG |=(1<<12);
			rLCD_TV_CONTROL |= 1<<5;	
			rLCD_CONTROL &= ~(1 << 8); 
		} else {
			rSYS_LCD_CLK_CFG &=~(1<<12);
			rLCD_TV_CONTROL &=~(1<<5);		
		}
        break;
    case 2: // osd3
    	if (enable) {
			rSYS_LCD_CLK_CFG |=(1<<11);
			rLCD_TV_CONTROL |= 1<<6;	
			rLCD_CONTROL &= ~(1 << 9); 
		} else {
			rSYS_LCD_CLK_CFG &=~(1<<11);
			rLCD_TV_CONTROL &=~(1<<6);		
		}
        break;
    }
}

void ark_disp_set_osd_alpha(int id, unsigned char alpha)
{
    switch (id)
    {
    case 0:
        /* CLCD_OSD1_CTL Reg0x074 */
        rLCD_OSD1_CTL &= ~0xFF;
        rLCD_OSD1_CTL |= alpha; // 0-7: osd1 blending coef
        break;
    case 1:
        /* CLCD_OSD2_CTL Reg0x088 */
        rLCD_OSD2_CTL &= ~0xFF;
        rLCD_OSD2_CTL |= alpha; // 0-7: osd1 blending coef
        break;
    case 2:
        /* CLCD_OSD3_CTL Reg0x098 */
        rLCD_OSD3_CTL &= ~0xFF;
        rLCD_OSD3_CTL |= alpha; // 0-7: osd1 blending coef
        break;
    default:
        break;
    }
}


unsigned int ark_disp_get_osd_hue(int id)
{
    unsigned int val;

    switch (id)
    {
    case 0:
        /* OSD1_VP_REG_1 REG0x144 */
        val = rLCD_OSD1_VP_REG_1;
        break;
    case 1:
        /* OSD2_VP_REG_1 REG0x14c */
        val = rLCD_OSD2_VP_REG_1;
        break;
    case 2:
        /* OSD3_VP_REG_1 REG0x154 */
        val = rLCD_OSD3_VP_REG_1;
        break;
    default:
        /* OSD1_VP_REG_1 REG0x144 */
        val = rLCD_OSD1_VP_REG_1;
        break;
    }

    val = (val >> 24) & 0xFF; /* Hue: bit31-24 */
    return val;
}

unsigned int ark_disp_get_osd_saturation(int id)
{
    unsigned int val;

    switch (id)
    {
    case 0:
        /* OSD1_VP_REG_1 REG0x144 */
        val = rLCD_OSD1_VP_REG_1;
        break;
    case 1:
        /* OSD2_VP_REG_1 REG0x14c */
        val = rLCD_OSD2_VP_REG_1;
        break;
    case 2:
        /* OSD3_VP_REG_1 REG0x154 */
        val = rLCD_OSD3_VP_REG_1;
        break;
    default:
        /* OSD1_VP_REG_1 REG0x144 */
        val = rLCD_OSD1_VP_REG_1;
        break;
    }

    val = (val >> 16) & 0xFF; /* Saturation: bit23-16 */
    return val;
}

unsigned int ark_disp_get_osd_brightness(int id)
{
    unsigned int val;

    switch (id)
    {
    case 0:
        /* OSD1_VP_REG_1 REG0x144 */
        val = rLCD_OSD1_VP_REG_1;
        break;
    case 1:
        /* OSD2_VP_REG_1 REG0x14c */
        val = rLCD_OSD2_VP_REG_1;
        break;
    case 2:
        /* OSD3_VP_REG_1 REG0x154 */
        val = rLCD_OSD3_VP_REG_1;
        break;
    default:
        /* OSD1_VP_REG_1 REG0x144 */
        val = rLCD_OSD1_VP_REG_1;
        break;
    }

    val = (val >> 8) & 0xFF; /* Brightness: bit15-8 */
    return val;
}

unsigned int ark_disp_get_osd_contrast(int id)
{
    unsigned int val;

    switch (id)
    {
    case 0:
        /* OSD1_VP_REG_1 REG0x144 */
        val = rLCD_OSD1_VP_REG_1;
        break;
    case 1:
        /* OSD2_VP_REG_1 REG0x14c */
        val = rLCD_OSD2_VP_REG_1;
        break;
    case 2:
        /* OSD3_VP_REG_1 REG0x154 */
        val = rLCD_OSD3_VP_REG_1;
        break;
    default:
        /* OSD1_VP_REG_1 REG0x144 */
        val = rLCD_OSD1_VP_REG_1;
        break;
    }

    val = (val >> 0) & 0xFF; /* Contrast: bit7-0 */
    return val;
}


void ark_disp_set_osd_hue(int id, unsigned int hue)
{
    switch (id)
    {
    case 0:
        /* OSD1_VP_REG_1 REG0x144 */
        rLCD_OSD1_VP_REG_1 &= ~(0xFF << 24); /* Hue: bit31-24 */
        rLCD_OSD1_VP_REG_1 |= hue << 24;
        break;
    case 1:
        /* OSD2_VP_REG_1 REG0x14c */
        rLCD_OSD2_VP_REG_1 &= ~(0xFF << 24); /* Hue: bit31-24 */
        rLCD_OSD2_VP_REG_1 |= hue << 24;
        break;
    case 2:
        /* OSD3_VP_REG_1 REG0x154 */
        rLCD_OSD3_VP_REG_1 &= ~(0xFF << 24); /* Hue: bit31-24 */
        rLCD_OSD3_VP_REG_1 |= hue << 24;
        break;
    default:
        break;
    }
}

void ark_disp_set_osd_saturation(int id, unsigned int saturation)
{
    switch (id)
    {
    case 0:
        /* OSD1_VP_REG_1 REG0x144 */
        rLCD_OSD1_VP_REG_1 &= ~(0xFF << 16); /* Saturation: bit23-16 */
        rLCD_OSD1_VP_REG_1 |= saturation << 16;
        break;
    case 1:
        /* OSD2_VP_REG_1 REG0x14c */
        rLCD_OSD2_VP_REG_1 &= ~(0xFF << 16); /* Saturation: bit23-16 */
        rLCD_OSD2_VP_REG_1 |= saturation << 16;
        break;
    case 2:
        /* OSD3_VP_REG_1 REG0x154 */
        rLCD_OSD3_VP_REG_1 &= ~(0xFF << 16); /* Saturation: bit23-16 */
        rLCD_OSD3_VP_REG_1 |= saturation << 16;
        break;
    default:
        break;
    }
}

void ark_disp_set_osd_brightness(int id, unsigned int brightness)
{
    switch (id)
    {
    case 0:
        /* OSD1_VP_REG_1 REG0x144 */
        rLCD_OSD1_VP_REG_1 &= ~(0xFF << 8); /* Brightness: bit15-8 */
        rLCD_OSD1_VP_REG_1 |= brightness << 8;
        break;
    case 1:
        /* OSD2_VP_REG_1 REG0x14c */
        rLCD_OSD2_VP_REG_1 &= ~(0xFF << 8); /* Brightness: bit15-8 */
        rLCD_OSD2_VP_REG_1 |= brightness << 8;
        break;
    case 2:
        /* OSD3_VP_REG_1 REG0x154 */
        rLCD_OSD3_VP_REG_1 &= ~(0xFF << 8); /* Brightness: bit15-8 */
        rLCD_OSD3_VP_REG_1 |= brightness << 8;
        break;
    default:
        break;
    }
}

void ark_disp_set_osd_contrast(int id, unsigned int contrast)
{
    switch (id)
    {
    case 0:
        /* OSD1_VP_REG_1 REG0x144 */
        rLCD_OSD1_VP_REG_1 &= ~(0xFF << 0); /* Contrast: bit7-0 */
        rLCD_OSD1_VP_REG_1 |= contrast << 0;
        break;
    case 1:
        /* OSD2_VP_REG_1 REG0x14c */
        rLCD_OSD2_VP_REG_1 &= ~(0xFF << 0); /* Contrast: bit7-0 */
        rLCD_OSD2_VP_REG_1 |= contrast << 0;
        break;
    case 2:
        /* OSD3_VP_REG_1 REG0x154 */
        rLCD_OSD3_VP_REG_1 &= ~(0xFF << 0); /* Contrast: bit7-0 */
        rLCD_OSD3_VP_REG_1 |= contrast << 0;
        break;
    default:
        break;
    }
}

void ark_disp_set_osd_alpha_blend_en_lcd(int id, int enable)
{
    unsigned int pos;

    switch (id)
    {
    case 0: /* osd1 */
        pos = 13;
        break;
    case 1: /* osd2 */
        pos = 15;
        break;
    case 2: /* osd3 */
        pos = 17;
        break;
    default:
        return;
    }

    /* CLCD_BLD_MODE_LCD_REG1 REG0x064 */
    if (enable) {
        rLCD_BLD_MODE_LCD_REG1 |= 1 << pos;
            /* enable blending of osd layer with back color */
    } else {
        rLCD_BLD_MODE_LCD_REG1 &= ~(1 << pos);
            /* disable blending of osd layer with back color */
    }
}

void ark_disp_set_osd_alpha_blend_en_tvenc(int id, int enable)
{
    unsigned int pos;

    switch (id)
    {
    case 0: /* osd1 */
        pos = 13;
        break;
    case 1: /* osd2 */
        pos = 15;
        break;
    case 2: /* osd3 */
        pos = 17;
        break;
    default:
        return;
    }

    /* CLCD_BLD_MODE_TV_REG1 REG0x6c */
    if (enable) {
        rLCD_BLD_MODE_TV_REG1 |= 1 << pos;
            /* enable blending of osd layer with back color */
    } else {
        rLCD_BLD_MODE_TV_REG1 &= ~(1 << pos);
            /* disable blending of osd layer with back color */
    }
}

void ark_disp_set_osd_per_pix_alpha_blend_en_lcd(int id, int enable)
{
    unsigned int pos;

    switch (id)
    {
    case 0: /* osd1 */
        pos = 12;
        break;
    case 1: /* osd2 */
        pos = 14;
        break;
    case 2: /* osd3 */
        pos = 16;
        break;
    default:
        return;
    }

    /* CLCD_BLD_MODE_LCD_REG1 REG0x064 */
    if (enable) {
        rLCD_BLD_MODE_LCD_REG1 |= 1 << pos;
            /* enable using of pixel alpha (alpha value from pixel data) */
    } else {
        rLCD_BLD_MODE_LCD_REG1 &= ~(1 << pos);
            /* enable using of layer alpha (alpha value from register) */
    }
}

void ark_disp_set_osd_per_pix_alpha_blend_en_tvenc(int id, int enable)
{
    unsigned int pos;

    switch (id)
    {
    case 0: /* osd1 */
        pos = 12;
        break;
    case 1: /* osd2 */
        pos = 14;
        break;
    case 2: /* osd3 */
        pos = 16;
        break;
    default:
        return;
    }

    /* CLCD_BLD_MODE_TV_REG1 REG0x06c */
    if (enable) {
        rLCD_BLD_MODE_TV_REG1 |= 1 << pos;
            /* enable using of pixel alpha (alpha value from pixel data) */
    } else {
        rLCD_BLD_MODE_TV_REG1 &= ~(1 << pos);
            /* enable using of layer alpha (alpha value from register) */
    }
}

void ark_disp_set_osd_blend_mode_lcd(int id, unsigned int mode)
{
    unsigned int pos;

    switch (id)
    {
    case 0: /* osd1 */
        pos = 12; // 15-12: blending mode of osd1
        break;
    case 1: /* osd2 */
        pos = 20; // 23-20: blending mode of osd2
        break;
    case 2: /* osd3 */
        pos = 28; // 31-28: blending mode of osd3
        break;
    default:
        return;
    }

    /* CLCD_BLD_MODE_LCD_REG0 REG0x060 */
    rLCD_BLD_MODE_LCD_REG0 &= ~(0xF << pos); // clear bits
    rLCD_BLD_MODE_LCD_REG0 |= mode << pos;   // set blending mode of osd
            // 0000: the whole blending;
            // 0001: the whole overwrite;
            // 0010: key color transparence absolutely, the other overwrite;
            // 0011: key color transparence absolutely, the other blending;
            // 0100: key color overwrite, the other blending;
            // 0101: key color overwrite, the other transparence absolutely;
            // 0110: key color blending, the other overwrite;
            // 0111:key color blending, the other transparence absolutely;
            // 1000: the whole blending;
            // 1001: the whole overwrite(layer1 x a);
            // 1010: key color transparence, the other overwrite (xa);
            // 1011: key color transparence, the other blending;
            // 1100: key color overwrite(xa), the other blending;
            // 1101: key color overwrite(xa), the other transparence;
            // 1110: key color blending, the other overwrite(xa);
}

void ark_disp_set_osd_blend_mode_tvenc(int id, unsigned int mode)
{
    unsigned int pos;

    switch (id)
    {
    case 0: /* osd1 */
        pos = 12; // 15-12: blending mode of osd1
        break;
    case 1: /* osd2 */
        pos = 20; // 23-20: blending mode of osd2
        break;
    case 2: /* osd3 */
        pos = 28; // 31-28: blending mode of osd3
        break;
    default:
        return;
    }

    /* CLCD_BLD_MODE_TV_REG0 REG0x068 */
    rLCD_BLD_MODE_TV_REG0 &= ~(0xF << pos); // clear bits
    rLCD_BLD_MODE_TV_REG0 |= mode << pos;   // set blending mode of osd
            // 0000: the whole blending;
            // 0001: the whole overwrite;
            // 0010: key color transparence absolutely, the other overwrite;
            // 0011: key color transparence absolutely, the other blending;
            // 0100: key color overwrite, the other blending;
            // 0101: key color overwrite, the other transparence absolutely;
            // 0110: key color blending, the other overwrite;
            // 0111:key color blending, the other transparence absolutely;
            // 1000: the whole blending;
            // 1001: the whole overwrite(layer1 x a);
            // 1010: key color transparence, the other overwrite (xa);
            // 1011: key color transparence, the other blending;
            // 1100: key color overwrite(xa), the other blending;
            // 1101: key color overwrite(xa), the other transparence;
            // 1110: key color blending, the other overwrite(xa);
}

void ark_disp_set_osd_colorkey(
    int id, int enable, unsigned int y, unsigned int cb, unsigned int cr)
{
    switch (id)
    {
    case 0:
        /* CLCD_COLOR_KEY_MASK_VALUE_OSD1 REG0xec */
        rLCD_COLOR_KEY_MASK_VALUE_OSD1 =
            enable << 24 | // 1=enable osd1 layer color key mask
            y << 16 | // 23-16: osd1 layer color key mask value of y
            cb << 8 | // 15-8: osd1 layer color key mask value of cb
            cr << 0;  // 7-0: osd1 layer color key mask value of cr
        break;
    case 1:
        /* CLCD_COLOR_KEY_MASK_VALUE_OSD2 REG0xf0 */
    	rLCD_COLOR_KEY_MASK_VALUE_OSD2 =
            enable << 24 | y << 16 | cb << 8 | cr << 0;
        break;
    case 2:
        /* CLCD_COLOR_KEY_MASK_VALUE_OSD3 REG0xf4 */
    	rLCD_COLOR_KEY_MASK_VALUE_OSD3 =
            enable << 24 | y << 16 | cb << 8 | cr << 0;
        break;
    default:
        break;
    }
}

void ark_disp_get_osd_colorkey(int id, unsigned int *colorkey)
{
    switch (id)
    {
    case 0:
        /* CLCD_COLOR_KEY_MASK_VALUE_OSD1 REG0xec */
        *colorkey = rLCD_COLOR_KEY_MASK_VALUE_OSD1;
        break;
    case 1:
        /* CLCD_COLOR_KEY_MASK_VALUE_OSD2 REG0xf0 */
    	*colorkey = rLCD_COLOR_KEY_MASK_VALUE_OSD2;
        break;
    case 2:
        /* CLCD_COLOR_KEY_MASK_VALUE_OSD3 REG0xf4 */
    	*colorkey = rLCD_COLOR_KEY_MASK_VALUE_OSD3;
        break;
    default:
        break;
    }
}


void ark_disp_set_osd_colorkey_thld(
    int id, unsigned int y_thld, unsigned int cb_thld, unsigned int cr_thld)
{
    switch (id)
    {
    case 0:
        /* CLCD_COLOR_KEY_MASK_THLD_OSD1 REG0xdc */
        rLCD_COLOR_KEY_MASK_THLD_OSD1 =
            y_thld << 16 | // 23-16: osd1 layer color key mask threshold of y
            cb_thld << 8 | // 15-8: osd1 layer color key mask threshold of cb
            cr_thld << 0;  // 7-80 osd1 layer color key mask threshold of cr
        break;
    case 1:
        /* CLCD_COLOR_KEY_MASK_THLD_OSD2 REG0xe0 */
        rLCD_COLOR_KEY_MASK_THLD_OSD2 =
            y_thld << 16 | // 23-16: osd2 layer color key mask threshold of y
            cb_thld << 8 | // 15-8: osd2 layer color key mask threshold of cb
            cr_thld << 0;  // 7-80 osd2 layer color key mask threshold of cr
        break;
    case 2:
        /* CLCD_COLOR_KEY_MASK_THLD_OSD3 REG0xe4 */
        rLCD_COLOR_KEY_MASK_THLD_OSD3 =
            y_thld << 16 | // 23-16: osd3 layer color key mask threshold of y
            cb_thld << 8 | // 15-8: osd3 layer color key mask threshold of cb
            cr_thld << 0;  // 7-80 osd3 layer color key mask threshold of cr
        break;
    default:
        break;
    }
}

void ark_disp_get_osd_colorkey_thld(int id, unsigned int *thld)
{
    switch (id)
    {
    case 0:
        /* CLCD_COLOR_KEY_MASK_THLD_OSD1 REG0xdc */
        *thld = rLCD_COLOR_KEY_MASK_THLD_OSD1;
        break;
    case 1:
        *thld = rLCD_COLOR_KEY_MASK_THLD_OSD2;
        break;
    case 2:
        /* CLCD_COLOR_KEY_MASK_THLD_OSD3 REG0xe4 */
        *thld = rLCD_COLOR_KEY_MASK_THLD_OSD3;
        break;
    default:
        break;
    }
}


void ark_disp_set_osd_layer_cut(
    int id,
    unsigned int left, unsigned int right, unsigned int up, unsigned int down
    )
{
    if (id == 0) {
        /* CLCD_BLD_CUT_LEFT_RIGHT_OSD1 REG0x104 */
        rLCD_BLD_CUT_LEFT_RIGHT_OSD1 =
            right<<12 |  // 23-12: blending window cut num of right side
            (left & 0xfff)<<0; // 11-0: blending window cut num of left side

        /* CLCD_BLD_CUT_UP_DOWN_OSD1 REG0x108 */
        rLCD_BLD_CUT_UP_DOWN_OSD1 =
            down<<12 |  // 23-12: blending window cut number of down side
            (up & 0xfff)<<0; // 11-0: blending window cut number of up side
    } else if (id == 1) {
        /* CLCD_BLD_CUT_LEFT_RIGHT_OSD2 REG0x10c */
        rLCD_BLD_CUT_LEFT_RIGHT_OSD2 =
            right<<12 |
            (left & 0xfff)<<0;

        /* CLCD_BLD_CUT_UP_DOWN_OSD2 REG0x110 */
        rLCD_BLD_CUT_UP_DOWN_OSD2 =
            down<<12 |
            (up & 0xfff)<<0;
    } else if (id == 2) {
        /* CLCD_BLD_CUT_LEFT_RIGHT_OSD2 REG0x114 */
        rLCD_BLD_CUT_LEFT_RIGHT_OSD3 =
            right<<12 |
            (left & 0xfff)<<0;

        /* CLCD_BLD_CUT_UP_DOWN_OSD2 REG0x118 */
        rLCD_BLD_CUT_UP_DOWN_OSD3 =
            down<<12 |
            (up & 0xfff)<<0;
    }
}

void ark_disp_palette_config(unsigned int num, unsigned int * data_base)
{
    unsigned int i;

    /* LCDPalette REG0x400 */
    for (i = 0; i < num; i++) {
        *(rLCD_Palette_BASE + i) = data_base[i];
    }
}

void ark_disp_set_osd_palette(int palette_color_num)
{
    /* CLCD_OSD3_CTL REG0x98 */
    rLCD_OSD3_CTL |= 1<<9; // 1=init of palette regfile mode

    // NOTE: set this bit before config color data in LCD palette; after
    // that, clear this bit to enable displaying of the image on osd layer

    switch (palette_color_num)
    {
    case 0:
        ark_disp_palette_config(2, (unsigned int * )PALETTE_2_320240);

        rLCD_OSD1_CTL &= ~(3<<10); // 11-10: 0=1bpp (black-white)
        rLCD_OSD2_CTL &= ~(3<<10);
        rLCD_OSD3_CTL &= ~(3<<10);	
        break;
    case 1:
        ark_disp_palette_config(4,(unsigned int * )PALETTE_4_640320);

        rLCD_OSD1_CTL &= ~(3<<10); // clear bit 11-10
        rLCD_OSD1_CTL |= (1<<10);  // 11-10: 2bpp (4-color)
        rLCD_OSD2_CTL &= ~(3<<10);
        rLCD_OSD2_CTL |= (1<<10);
        rLCD_OSD3_CTL &= ~(3<<10);	
        rLCD_OSD3_CTL |= (1<<10);
        break;
    case 2:
        ark_disp_palette_config(16,(unsigned int * )PALETTE_16_720480);

        rLCD_OSD1_CTL &= ~(3<<10); // clear bit 11-10
        rLCD_OSD1_CTL |= (2<<10);  // 11-10: 2=4bpp (16-color)
        rLCD_OSD2_CTL &= ~(3<<10);
        rLCD_OSD2_CTL |= (2<<10);
        rLCD_OSD3_CTL &= ~(3<<10);	
        rLCD_OSD3_CTL |= (2<<10);
        break;
    case 3:
        ark_disp_palette_config(256,(unsigned int * )PALETTE_800480_256_MICKEY);

        rLCD_OSD1_CTL |= 3<<10; // 11-10: 3=8bpp (256-color)
        rLCD_OSD2_CTL |= 3<<10;
        rLCD_OSD3_CTL |= 3<<10;
        break;
    default:
        break;
    }

    /* CLCD_OSD3_CTL REG0x98 */
    rLCD_OSD3_CTL &= ~(1<<9); // clear this bit after config done 
}

void ark_disp_set_osd_data_addr(int id, unsigned int addr)
{
    if (id == 0) {
        /* CLCD_OSD1_ADDR REG0x80 */
        rLCD_OSD1_ADDR = addr; // osd1 data buffer addr group0 in memory
    } else if (id == 1) {
        /* CLCD_OSD2_ADDR REG0x94 */
        rLCD_OSD2_ADDR = addr; // osd2 data buffer addr group0 in memory
    } else if (id == 2) {
        /* CLCD_OSD3_ADDR REG0xa4 */
        rLCD_OSD3_ADDR = addr; // osd3 data buffer addr group0 in memory
    }
}

void ark_disp_set_osd_data_addr_cvbs(int id, unsigned int format,
    unsigned int width, unsigned int height, unsigned int addr
    )
{
    if (id == 0) {
        /* CLCD_OSD1_ADDR REG0x80 */
        rLCD_OSD1_ADDR = addr; // osd1 data buffer addr group0 in memory

        switch (format)
        {
        case ARK_DISP_OSD_PIXFMT_YUYV:
        case ARK_DISP_OSD_PIXFMT_RGBI555:
        case ARK_DISP_OSD_PIXFMT_RGB565:
            /* CLCD_OSD1_ADDR_GROUP1 REG0x2f0 */
            rLCD_OSD1_ADDR_GROUP1 = addr + width * 2; // start addr group1
                     // start addr of osd1 image data for double buffer mode
            break;
        case ARK_DISP_OSD_PIXFMT_YUV:
        case ARK_DISP_OSD_PIXFMT_RGB888:
        case ARK_DISP_OSD_PIXFMT_RGBA888:
            rLCD_OSD1_ADDR_GROUP1 = addr + width * 4;
                     // start addr of osd1 image data for double buffer mode
            break;
        default:
            break;
        }
    } else if (id == 1) {
        /* CLCD_OSD2_ADDR REG0x94 */
        rLCD_OSD2_ADDR = addr; // osd2 data buffer addr group0 in memory

        switch (format)
        {
        case ARK_DISP_OSD_PIXFMT_YUYV:
        case ARK_DISP_OSD_PIXFMT_RGBI555:
        case ARK_DISP_OSD_PIXFMT_RGB565:
            // CLCD_OSD2_ADDR_GROUP1 REG0x2f4
            rLCD_OSD2_ADDR_GROUP1 = addr + width * 2;
            // osd2 data buffer start addr group1 in memory
            break;
        case ARK_DISP_OSD_PIXFMT_YUV:
        case ARK_DISP_OSD_PIXFMT_RGB888:
        case ARK_DISP_OSD_PIXFMT_RGBA888:
            rLCD_OSD2_ADDR_GROUP1 = addr + width * 4;
            // osd2 data buffer start addr group1 in memory
            break;
        default:
            break;
        }
    } else if (id == 2) {
        /* CLCD_OSD3_ADDR REG0xa4 */
        rLCD_OSD3_ADDR = addr; // osd3 data buffer addr group0 in memory

        switch (format)
        {
        case ARK_DISP_OSD_PIXFMT_YUYV:
        case ARK_DISP_OSD_PIXFMT_RGBI555:
        case ARK_DISP_OSD_PIXFMT_RGB565:
            // CLCD_OSD3_ADDR_GROUP1 REG0x2f8
            rLCD_OSD3_ADDR_GROUP1 = addr + width * 2;
            // osd3 data buffer start addr group1 in memory
            break;
        case ARK_DISP_OSD_PIXFMT_YUV:
        case ARK_DISP_OSD_PIXFMT_RGB888:
        case ARK_DISP_OSD_PIXFMT_RGBA888:
            rLCD_OSD3_ADDR_GROUP1 = addr + width * 4;
            // osd3 data buffer start addr group1 in memory
            break;
        default:
            break;
        }
    }
}

void ark_disp_set_osd_double_buffer_en(int id, int enable)
{
    if (id == 0) {
        if (enable) {
            // CLCD_OSD1_CTL Reg0x74
            rLCD_OSD1_CTL |= 1<<24; // 1=enable double buffer mode
        } else {
            rLCD_OSD1_CTL &= ~(1<<24);
        }
    } else if (id == 1) {
        if (enable) {
            // CLCD_OSD2_CTL Reg0x88
            rLCD_OSD2_CTL |= 1<<24; // 1=enable double buffer mode
        } else {
            rLCD_OSD2_CTL &= ~(1<<24);
        }
    } else if (id == 2) {
        if (enable) {
            // CLCD_OSD3_CTL Reg0x88
            rLCD_OSD3_CTL |= 1<<24; // 1=enable double buffer mode
        } else {
            rLCD_OSD3_CTL &= ~(1<<24);
        }
    }
}

void ark_disp_set_osd_frame_mode(int id, int frame)
{
    /* frame mode select for interlace data capture (read) of osd layer,
     * normally used in cvbs, itu656, ypbpr
     */
    if (id == 0) {
        if (frame) {
            // CLCD_OSD1_CTL Reg0x074
            rLCD_OSD1_CTL |= 1<<26; //1=frame mode for interlace get data
                                    // from memory
        } else {
            rLCD_OSD1_CTL &= ~(1<<26); //0=field mode
        }
    } else if (id == 1) {
        if (frame) {
            // CLCD_OSD2_CTL Reg0x088
            rLCD_OSD2_CTL |= 1<<26;
        } else {
            rLCD_OSD2_CTL &= ~(1<<26);
        }
    } else if (id == 2) {
        if (frame) {
            // CLCD_OSD3_CTL Reg0x098
            rLCD_OSD3_CTL |= 1<<26;
        } else {
            rLCD_OSD3_CTL &= ~(1<<26);
        }
    }
}

void ark_disp_set_osd_frame_invert(int id, int invert)
{
    /* field invert select for frame mode and interlace data capture of osd
     * layer
     */
    if (id == 0) {
        if (invert) {
            // CLCD_OSD1_CTL Reg0x074
            rLCD_OSD1_CTL |= 1<<25; //1=invert
        } else {
            rLCD_OSD1_CTL &= ~(1<<25);
        }
    } else if (id == 1) {
        if (invert) {
            // CLCD_OSD2_CTL Reg0x088
            rLCD_OSD2_CTL |= 1<<25;
        } else {
            rLCD_OSD2_CTL &= ~(1<<25);
        }
    } else if (id == 2) {
        if (invert) {
            // CLCD_OSD3_CTL Reg0x098
            rLCD_OSD3_CTL |= 1<<25;
        } else {
            rLCD_OSD3_CTL &= ~(1<<25);
        }
    }
}
