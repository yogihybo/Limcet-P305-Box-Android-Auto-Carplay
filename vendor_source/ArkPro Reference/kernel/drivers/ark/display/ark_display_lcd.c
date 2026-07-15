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
 *      ark_display_lcd.c
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

#define GPIO_BANK_C_START               64
#define GPIO_BANK_C_END                 95

//#define GPIO72          72
//#define GPIO73          73

//typedef enum
//{
//        euDataLow = 0,          //?͵?ƽ
//        euDataHigh,                     //?ߵ?ƽ
//        euDataNone,
//}EU_GPIO_Data;

//typedef enum
//{
//        euLowLevelTrig = 0,     //?͵?ƽ?????ж?
//        euHightLevelTrig,       //?ߵ?ƽ?????ж?
//}EU_TRIG_LEVEL;

//typedef enum
//{
//        euInputPad = 0, //??????
//        euOutputPad,    //??????
//        euNoneSettingPad,
//}EU_GPIO_Direction;


//#define rGPIO_PC_MOD     (*(volatile unsigned int *)(ark_gpio_mmio_base + 0x40))
//#define rGPIO_PC_RDATA   (*(volatile unsigned int *)(ark_gpio_mmio_base + 0x44))

void ark_disp_select_pwm0_pad(void)
{
	unsigned int val;

	//use the second pad mutex config
	val = rSYS_PAD_CTRL09;
	val &= ~0x3;
	val |= 0x2;       //choose one pad of the channel 0
	rSYS_PAD_CTRL09 = val;

	printk(KERN_ERR "%s %d:rSYS_PAD_CTRL09=0x%08x\n",
			__FUNCTION__, __LINE__, val);
}

void ark_disp_select_pwm1_pad(void)
{
	unsigned int val;

	//use the second pad mutex config
	val = rSYS_PAD_CTRL09;
	val &= ~(0x3<<2);
	val |= (0x2<<2);          //choose one pad of the channel 0     
	rSYS_PAD_CTRL09  = val;

	printk(KERN_ERR "%s %d:rSYS_PAD_CTRL09=0x%08x\n",
			__FUNCTION__, __LINE__, val);
}

void ark_disp_disable_led(void)
{
#if 0
	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);
	gpio_set_value(LED_GPIO, 0);
#endif
}

void ark_disp_enable_led(void)
{
#if 0
	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);
	gpio_set_value(LED_GPIO, 1);
#endif

}

void ark_disp_lcdl2rscanpadhigh(void)
{
#if 0
	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);
	gpio_set_value(LCDL2RSCANPAD, 1);
#endif

}

void ark_disp_lcdl2rscanpadlow(void)
{
#if 0
	ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);
	gpio_set_value(LCDL2RSCANPAD, 0);
#endif
}


void ark_disp_config_gamma(void)
{
#define CUSTOM_EN       0x1
#define GAMMA_EN        0x1

#define custom_r_val01  0x01
#define custom_r_val02  0x04
#define custom_r_val03  0x06
#define custom_r_val04  0x0a
#define custom_r_val05  0x0f
#define custom_r_val06  0x15
#define custom_r_val07  0x1c
#define custom_r_val08  0x22
#define custom_r_val09  0x29
#define custom_r_val10  0x2f
#define custom_r_val11  0x36
#define custom_r_val12  0x3c
#define custom_r_val13  0x43
#define custom_r_val14  0x49
#define custom_r_val15  0x50
#define custom_r_val16  0x56
#define custom_r_val17  0x5b
#define custom_r_val18  0x61
#define custom_r_val19  0x66
#define custom_r_val20  0x6b
#define custom_r_val21  0x6f
#define custom_r_val22  0x73
#define custom_r_val23  0x77
#define custom_r_val24  0x7b
#define custom_r_val25  0x7f
#define custom_r_val26  0x82
#define custom_r_val27  0x86
#define custom_r_val28  0x89
#define custom_r_val29  0x8d
#define custom_r_val30  0x90
#define custom_r_val31  0x93
#define custom_r_val32  0x96
#define custom_r_val33  0x99
#define custom_r_val34  0x9c
#define custom_r_val35  0x9f
#define custom_r_val36  0xa1
#define custom_r_val37  0xa4
#define custom_r_val38  0xa6
#define custom_r_val39  0xa9
#define custom_r_val40  0xab
#define custom_r_val41  0xae
#define custom_r_val42  0xb1
#define custom_r_val43  0xb4
#define custom_r_val44  0xb6
#define custom_r_val45  0xb9
#define custom_r_val46  0xbc
#define custom_r_val47  0xbe
#define custom_r_val48  0xc1
#define custom_r_val49  0xc4
#define custom_r_val50  0xc7
#define custom_r_val51  0xca
#define custom_r_val52  0xcd
#define custom_r_val53  0xd0
#define custom_r_val54  0xd4
#define custom_r_val55  0xd7
#define custom_r_val56  0xdb
#define custom_r_val57  0xdf
#define custom_r_val58  0xe2
#define custom_r_val59  0xe6
#define custom_r_val60  0xea
#define custom_r_val61  0xef
#define custom_r_val62  0xf4
#define custom_r_val63  0xf9
#define custom_g_val01  0x01
#define custom_g_val02  0x04
#define custom_g_val03  0x06
#define custom_g_val04  0x0a
#define custom_g_val05  0x0f
#define custom_g_val06  0x15
#define custom_g_val07  0x1c
#define custom_g_val08  0x22
#define custom_g_val09  0x29
#define custom_g_val10  0x2f
#define custom_g_val11  0x36
#define custom_g_val12  0x3c
#define custom_g_val13  0x43
#define custom_g_val14  0x49
#define custom_g_val15  0x50
#define custom_g_val16  0x56
#define custom_g_val17  0x5b
#define custom_g_val18  0x61
#define custom_g_val19  0x66
#define custom_g_val20  0x6b
#define custom_g_val21  0x6f
#define custom_g_val22  0x73
#define custom_g_val23  0x77
#define custom_g_val24  0x7b
#define custom_g_val25  0x7f
#define custom_g_val26  0x82
#define custom_g_val27  0x86
#define custom_g_val28  0x89
#define custom_g_val29  0x8d
#define custom_g_val30  0x90
#define custom_g_val31  0x93
#define custom_g_val32  0x96
#define custom_g_val33  0x99
#define custom_g_val34  0x9c
#define custom_g_val35  0x9f
#define custom_g_val36  0xa1
#define custom_g_val37  0xa4
#define custom_g_val38  0xa6
#define custom_g_val39  0xa9
#define custom_g_val40  0xab
#define custom_g_val41  0xae
#define custom_g_val42  0xb1
#define custom_g_val43  0xb4
#define custom_g_val44  0xb6
#define custom_g_val45  0xb9
#define custom_g_val46  0xbc
#define custom_g_val47  0xbe
#define custom_g_val48  0xc1
#define custom_g_val49  0xc4
#define custom_g_val50  0xc7
#define custom_g_val51  0xca
#define custom_g_val52  0xcd
#define custom_g_val53  0xd0
#define custom_g_val54  0xd4
#define custom_g_val55  0xd7
#define custom_g_val56  0xdb
#define custom_g_val57  0xdf
#define custom_g_val58  0xe2
#define custom_g_val59  0xe6
#define custom_g_val60  0xea
#define custom_g_val61  0xef
#define custom_g_val62  0xf4
#define custom_g_val63  0xf9
#define custom_b_val01  0x01
#define custom_b_val02  0x04
#define custom_b_val03  0x06
#define custom_b_val04  0x0a
#define custom_b_val05  0x0f
#define custom_b_val06  0x15
#define custom_b_val07  0x1c
#define custom_b_val08  0x22
#define custom_b_val09  0x29
#define custom_b_val10  0x2f
#define custom_b_val11  0x36
#define custom_b_val12  0x3c
#define custom_b_val13  0x43
#define custom_b_val14  0x49
#define custom_b_val15  0x50
#define custom_b_val16  0x56
#define custom_b_val17  0x5b
#define custom_b_val18  0x61
#define custom_b_val19  0x66
#define custom_b_val20  0x6b
#define custom_b_val21  0x6f
#define custom_b_val22  0x73
#define custom_b_val23  0x77
#define custom_b_val24  0x7b
#define custom_b_val25  0x7f
#define custom_b_val26  0x82
#define custom_b_val27  0x86
#define custom_b_val28  0x89
#define custom_b_val29  0x8d
#define custom_b_val30  0x90
#define custom_b_val31  0x93
#define custom_b_val32  0x96
#define custom_b_val33  0x99
#define custom_b_val34  0x9c
#define custom_b_val35  0x9f
#define custom_b_val36  0xa1
#define custom_b_val37  0xa4
#define custom_b_val38  0xa6
#define custom_b_val39  0xa9
#define custom_b_val40  0xab
#define custom_b_val41  0xae
#define custom_b_val42  0xb1
#define custom_b_val43  0xb4
#define custom_b_val44  0xb6
#define custom_b_val45  0xb9
#define custom_b_val46  0xbc
#define custom_b_val47  0xbe
#define custom_b_val48  0xc1
#define custom_b_val49  0xc4
#define custom_b_val50  0xc7
#define custom_b_val51  0xca
#define custom_b_val52  0xcd
#define custom_b_val53  0xd0
#define custom_b_val54  0xd4
#define custom_b_val55  0xd7
#define custom_b_val56  0xdb
#define custom_b_val57  0xdf
#define custom_b_val58  0xe2
#define custom_b_val59  0xe6
#define custom_b_val60  0xea
#define custom_b_val61  0xef
#define custom_b_val62  0xf4
#define custom_b_val63  0xf9

	rLCD_GAMMA_REG_0  = (custom_r_val01<<2)|(GAMMA_EN<<1)|CUSTOM_EN;
	rLCD_GAMMA_REG_1  = (custom_r_val05<<24)|(custom_r_val04<<16)|(custom_r_val03<<8)|custom_r_val02;
	rLCD_GAMMA_REG_2  = (custom_r_val09<<24)|(custom_r_val08<<16)|(custom_r_val07<<8)|custom_r_val06;
	rLCD_GAMMA_REG_3  = (custom_r_val13<<24)|(custom_r_val12<<16)|(custom_r_val11<<8)|custom_r_val10;
	rLCD_GAMMA_REG_4  = (custom_r_val17<<24)|(custom_r_val16<<16)|(custom_r_val15<<8)|custom_r_val14;
	rLCD_GAMMA_REG_5  = (custom_r_val21<<24)|(custom_r_val20<<16)|(custom_r_val19<<8)|custom_r_val18;
	rLCD_GAMMA_REG_6  = (custom_r_val25<<24)|(custom_r_val24<<16)|(custom_r_val23<<8)|custom_r_val22;
	rLCD_GAMMA_REG_7  = (custom_r_val29<<24)|(custom_r_val28<<16)|(custom_r_val27<<8)|custom_r_val26;

	rLCD_GAMMA_REG_8  = (custom_r_val33<<24)|(custom_r_val32<<16)|(custom_r_val31<<8)|custom_r_val30;
	rLCD_GAMMA_REG_9  = (custom_r_val37<<24)|(custom_r_val36<<16)|(custom_r_val35<<8)|custom_r_val34;
	rLCD_GAMMA_REG_10  = (custom_r_val41<<24)|(custom_r_val40<<16)|(custom_r_val39<<8)|custom_r_val38;

	rLCD_GAMMA_REG_11  = (custom_r_val45<<24)|(custom_r_val44<<16)|(custom_r_val43<<8)|custom_r_val42;
	rLCD_GAMMA_REG_12  = (custom_r_val49<<24)|(custom_r_val48<<16)|(custom_r_val47<<8)|custom_r_val46;
	rLCD_GAMMA_REG_13  = (custom_r_val53<<24)|(custom_r_val52<<16)|(custom_r_val51<<8)|custom_r_val50;

	rLCD_GAMMA_REG_14  = (custom_r_val57<<24)|(custom_r_val56<<16)|(custom_r_val55<<8)|custom_r_val54;
	rLCD_GAMMA_REG_15  = (custom_r_val61<<24)|(custom_r_val60<<16)|(custom_r_val59<<8)|custom_r_val58;
	rLCD_GAMMA_REG_16  = (custom_g_val02<<24)|(custom_g_val01<<16)|(custom_r_val63<<8)|custom_r_val62;

	rLCD_GAMMA_REG_17  = (custom_g_val06<<24)|(custom_g_val05<<16)|(custom_g_val04<<8)|custom_g_val03;
	rLCD_GAMMA_REG_18 = (custom_g_val10<<24)|(custom_g_val09<<16)|(custom_g_val08<<8)|custom_g_val07;
	rLCD_GAMMA_REG_19 = (custom_g_val14<<24)|(custom_g_val13<<16)|(custom_g_val12<<8)|custom_g_val11;
	rLCD_GAMMA_REG_20 = (custom_g_val18<<24)|(custom_g_val17<<16)|(custom_g_val16<<8)|custom_g_val15;
	rLCD_GAMMA_REG_21 = (custom_g_val22<<24)|(custom_g_val21<<16)|(custom_g_val20<<8)|custom_g_val19;
	rLCD_GAMMA_REG_22 = (custom_g_val26<<24)|(custom_g_val25<<16)|(custom_g_val24<<8)|custom_g_val23;
	rLCD_GAMMA_REG_23 = (custom_g_val30<<24)|(custom_g_val29<<16)|(custom_g_val28<<8)|custom_g_val27;
	rLCD_GAMMA_REG_24 = (custom_g_val34<<24)|(custom_g_val33<<16)|(custom_g_val32<<8)|custom_g_val31;

	rLCD_GAMMA_REG_25 = (custom_g_val38<<24)|(custom_g_val37<<16)|(custom_g_val36<<8)|custom_g_val35;

	rLCD_GAMMA_REG_26 = (custom_g_val42<<24)|(custom_g_val41<<16)|(custom_g_val40<<8)|custom_g_val39;
	rLCD_GAMMA_REG_27 = (custom_g_val46<<24)|(custom_g_val45<<16)|(custom_g_val44<<8)|custom_g_val43;
	rLCD_GAMMA_REG_28 = (custom_g_val50<<24)|(custom_g_val49<<16)|(custom_g_val48<<8)|custom_g_val47;
	rLCD_GAMMA_REG_29 = (custom_g_val54<<24)|(custom_g_val53<<16)|(custom_g_val52<<8)|custom_g_val51;
	rLCD_GAMMA_REG_30 = (custom_g_val58<<24)|(custom_g_val57<<16)|(custom_g_val56<<8)|custom_g_val55;
	rLCD_GAMMA_REG_31 = (custom_g_val62<<24)|(custom_g_val61<<16)|(custom_g_val60<<8)|custom_g_val59;

	rLCD_GAMMA_REG_32 = (custom_b_val03<<24)|(custom_b_val02<<16)|(custom_b_val01<<8)|custom_g_val63;
	rLCD_GAMMA_REG_33 = (custom_b_val07<<24)|(custom_b_val06<<16)|(custom_b_val05<<8)|custom_b_val04;
	rLCD_GAMMA_REG_34 = (custom_b_val11<<24)|(custom_b_val10<<16)|(custom_b_val09<<8)|custom_b_val08;
	rLCD_GAMMA_REG_35 = (custom_b_val15<<24)|(custom_b_val14<<16)|(custom_b_val13<<8)|custom_b_val12;
	rLCD_GAMMA_REG_36 = (custom_b_val19<<24)|(custom_b_val18<<16)|(custom_b_val17<<8)|custom_b_val16;
	rLCD_GAMMA_REG_37 = (custom_b_val23<<24)|(custom_b_val22<<16)|(custom_b_val21<<8)|custom_b_val20;
	rLCD_GAMMA_REG_38 = (custom_b_val27<<24)|(custom_b_val26<<16)|(custom_b_val25<<8)|custom_b_val24;
	rLCD_GAMMA_REG_39 = (custom_b_val31<<24)|(custom_b_val30<<16)|(custom_b_val29<<8)|custom_b_val28;

	rLCD_GAMMA_REG_40 = (custom_b_val35<<24)|(custom_b_val34<<16)|(custom_b_val33<<8)|custom_b_val32;
	rLCD_GAMMA_REG_41 = (custom_b_val39<<24)|(custom_b_val38<<16)|(custom_b_val37<<8)|custom_b_val36;
	rLCD_GAMMA_REG_42 = (custom_b_val43<<24)|(custom_b_val42<<16)|(custom_b_val41<<8)|custom_b_val40;
	rLCD_GAMMA_REG_43 = (custom_b_val47<<24)|(custom_b_val46<<16)|(custom_b_val45<<8)|custom_b_val44;
	rLCD_GAMMA_REG_44 = (custom_b_val51<<24)|(custom_b_val50<<16)|(custom_b_val49<<8)|custom_b_val48;
	rLCD_GAMMA_REG_45 = (custom_b_val55<<24)|(custom_b_val54<<16)|(custom_b_val53<<8)|custom_b_val52;
	rLCD_GAMMA_REG_46 = (custom_b_val59<<24)|(custom_b_val58<<16)|(custom_b_val57<<8)|custom_b_val56;
	rLCD_GAMMA_REG_47 = (custom_b_val63<<24)|(custom_b_val62<<16)|(custom_b_val61<<8)|custom_b_val60;
}

void ark_disp_config_D_tcon_800x480(void)
{
	//--------------------Digital tcon 800x480---------------
#define tconc0               0x1e
#define tconc1               0x18 // 0x05
#define tconc2               0x1d // 0x38
#define tconc3               0x80
#define tconc4               0x00

#define rgb_psel             0x0    //16
#define line656_inv_sel      0x0    //19
#define field_inv_sel        0x0    //20
#define hsyn_inv_sel         0x0    //21
#define vsyn_inv_sel         0x0    //22
#define test                 0x0    //23
#define lcd_lr_reg           0x1    //24   ??????
#define lcd_ud_reg           0x1    //25   ??????
#define lcd_edgsl_reg        0x0    //26
#define lcd_rev_reg          0x0    //27
#define tcon_ratio_en        0x0    //28
#define tcon_digital_en      0x0    //29
#define tcon_digital_inv	 0x0

#define r_ratio              0xff
#define g_ratio              0xff
#define b_ratio              0xff

#define tcph1_rise_l         0x14
#define tcph1_fall_l         0x1e
#define tcph2_rise_l         0x00
#define tcph2_fall_l         0x00

#define tcph3_rise_l         0x00
#define tcph3_fall_l         0x00
#define tcph4_rise_l         0x00
#define tcph4_fall_l         0x0e

#define sthl_rise_l          0x1b
#define sthl_fall_l          0x1f
#define sthr_rise_l          0x1b
#define sthr_fall_l          0x1f

#define ckv_rise_l           0x10
#define ckv_fall_l           0x14
#define vcom_trig_l          0x00
#define vcom2_trig_l         0x00

#define winh_rise_l          0x00//0x18
#define winh_fall_l          0x00//0xff
#define winv_rise_l          0x08
#define winv_fall_l          0x00//0x80

#define winh_rise_h          0x00
#define winh_fall_h          0x01
#define winv_rise_h          0x00
#define winv_fall_h          0x00

#define tcpv0_rise_l         0x00
#define tcpv0_fall_l         0x00
#define ckvsp_start_l        0x01
#define ckvsp_end_l          0x01

#define stvl_rise_l          0x08
#define stvl_fall_l          0x09
#define stvr_rise_l          0x08
#define stvr_fall_l          0x09

#define r_bias               0x80
#define g_bias               0x80
#define b_bias               0x80

#define r_bias_n             0x80
#define g_bias_n             0x80
#define b_bias_n             0x80

#define CKV_OEN              0x0
#define STHL_OEN             0x1
#define STHR_OEN             0x0
#define TCPH_OEN             0x1a
#define CKH3_OEN             0x1
#define CKH2_OEN             0x1
#define CKH1_OEN             0x1
#define STVR_OEN             0x0
#define STVL_OEN             0x1
#define TCPV0_OEN            0x0

#define TCON_SCREEN_ENABLE   0x1
#define RGB_ORDER            0x2 // gbr
#define RGB_INV              0x0 // 0: no inv
#define DTCON_EN             0x1 // 1: D_TCON  0: A_TCON ????
#define TCON_EN              0x1
#define RATION               0x1
#define STHLR_INV            0x0
#define RGB_CLK_INV          0x0
#define TCPH3_INV            0x0
#define VCOM_ACT             0x1
#define VCOM_INV		     0x0
#define LR                   0x0
#define UD                   0x1

#define H_SHIFT              0x19
#define V_SHIFT              0x5

	/* TCON_REG0 REG0x188 */
	rLCD_TCON_REG0 =
		STVL_OEN<<26 |
		TCON_SCREEN_ENABLE<<25 |
		RGB_ORDER<<22 |
		RGB_INV<<21 |
		DTCON_EN<<20 |
		TCON_EN<<19 |
		line656_inv_sel<<16 |
		vsyn_inv_sel<<12 |
		hsyn_inv_sel<<11 |
		field_inv_sel<<10 |
		RATION<<7 |
		STHLR_INV<<6 |
		RGB_CLK_INV<<5 |
		TCPH3_INV<<4 |
		VCOM_ACT<<3 |
		VCOM_INV<<2 |
		LR<<1 |
		UD<<0;

	/* TCON_REG0 REG0x18c */
	rLCD_TCON_REG1 =
		H_SHIFT<<12 |
		V_SHIFT<<0;

	/* TCON_REG0 REG0x190 */
	rLCD_TCON_REG2 = 0; // for test

	rLCD_TCON_REG3 =
		tcph1_fall_l<<12 |
		tcph1_rise_l<<0;

	rLCD_TCON_REG4 =
		tcph2_fall_l<<12 |
		tcph2_rise_l<<0;                  

	rLCD_TCON_REG5 =
		tcph3_fall_l<<12 |
		tcph3_rise_l<<0;

	rLCD_TCON_REG6 =
		tcph4_fall_l<<12 |
		tcph4_rise_l<<0;   

	rLCD_TCON_REG7 =
		sthl_fall_l<<12 |
		sthl_rise_l<<0;   

	rLCD_TCON_REG8 =
		sthr_fall_l<<12 |
		sthr_rise_l<<0;

	rLCD_TCON_REG9 =
		ckv_fall_l<<12 |
		ckv_rise_l<<0;

	rLCD_TCON_REG10 =
		vcom2_trig_l<<12 |
		vcom_trig_l<<0;

	rLCD_TCON_REG11 =
		winh_fall_l<<12 |
		winh_rise_l<<0;

	rLCD_TCON_REG12 =
		ckvsp_end_l<<24 |
		ckvsp_start_l<<16 |
		tcpv0_fall_l<<8 |
		tcpv0_rise_l<<0;

	rLCD_TCON_REG13 =
		stvr_fall_l<<24 |
		stvr_rise_l<<16 |
		stvl_fall_l<<8 |
		stvl_rise_l<<0;

	rLCD_TCON_REG14 =
		winv_fall_l<<12 |
		winv_rise_l<<0;

	rLCD_TCON_REG15 =
		b_bias<<16 |
		g_bias<<8 |
		r_bias<<0;

	rLCD_TCON_REG16 =
		b_bias_n<<16 |
		g_bias_n<<8 |
		r_bias_n<<0;

	rLCD_TCON_REG17 =
		b_ratio<<16 |
		g_ratio<<8 |
		r_ratio<<0;

	rLCD_TCON_REG18 = 0xfff<<20;
}


/* uncomment the line below to use HV_MODE */
//#define ARK_DISP_USE_HV_MODE

#ifdef ARK_DISP_USE_HV_MODE
// HV_MODE
#define HSW     0x15
#define HBP     0x54
#define HFP     0x15

#define VFP     0x18
#define VSW     0x50
#define VBP     0x20
#else
// DE_MODE
#define HSW		82
#define HBP		64
#define HFP		64


#define VFP     36
#define VSW     16//20
#define VBP     40
#endif

#define IOE     0
#define IHS     0
#define IVS     0


#define HSD070IDW1_LCD

void ark_disp_config_lcd_clk(void)
{
	unsigned int val;

	//chitest
	// #define rSYS_PER_CLK_EN *((volatile unsigned int *)(SYS_BASE+0x50))
	// for system clock control .... (how???)
	rSYS_PER_CLK_EN |= 1 << 4;

#ifdef HSD070IDW1_LCD
	//  val = (0<<31) | (0x1a<<25) | (1<<23) | (4<<19) | (1<<7) | (5<<4) | 0;   //3//syspll/1/5 =150/5 =30M  
	//  val = (0<<31) | (0x36<<25) | (1<<23) | (1<<19) | (1<<7) | (5<<4) | 0;   //3//syspll/1/5 =150/5 =30M  
	//    val = (0<<31) | (0x36<<25) | (1<<23) | (13<<19) | (1<<7) | (1<<4) | 0;
	val = 0xA0E80090;
	//val = (0<<31) | (0x36<<25) | (1<<23) | (2<<19) | (1<<7) | (5<<4) | 2;

	//val = (0<<31) | (0x1b<<25) | (1<<23) | (1<<19) | (1<<7) | (5<<4) | 0;   //3//syspll/1/5 =150/5 =30M  


	/* NOTE 130408: in FPGA, bit [31:25] are used to configure pixel clock
	 *              0=25MHz, 1=27MHz, 2=40MHz, 3=54MHz, 4=65MHz, 5=74MHz, 6=148MHz
	 */
#if defined(CONFIG_ARK_FPGA)
	val = 1 << 25; // 27 MHz
#endif
#else
	val = (0<<31) | (0x36<<25) | (1<<23) | (1<<19) | (1<<7) | (5<<4) | 0;   //3//syspll/1/5 =150/5 =30M  

	//	val = (0<<31) | (0x36<<25) | (1<<23) | (2<<19) | (1<<7) | (5<<4) | 0;   //3//syspll/2/5 =150/10 =15M    ok
	//	val = (0<<31) | (0x36<<25) | (1<<23) | (2<<19) | (1<<7) | (4<<4) | 0;   //3//syspll/2/4 =150/8 =18.75M   
#endif

	//chitest
	// configure PLL to output 30MHz signal
	// SYSREG 0x54
	printk("%s %d: original rSYS_LCD_CLK_CFG=0x%08x\n",
			__FUNCTION__, __LINE__, rSYS_LCD_CLK_CFG);

	rSYS_LCD_CLK_CFG = val;

	//    lcd_busy_delay(1000000);

	//    printk("%s %d: new rSYS_LCD_CLK_CFG=0x%08x\n",
	//        __FUNCTION__, __LINE__, rSYS_LCD_CLK_CFG);
}

void ark_disp_select_lcd_panel_pad(void)
{
#if  1 //enable digital panel
	// REG 0x1c0
	rSYS_PAD_CTRL00 =
		(1<<28) | //R7
		(1<<24) | //R6
		(1<<20) | //R5
		(1<<16) | //R4
		(1<<12) | //R3
		(1<<8) |  //R2
		(1<<4) |  //R1
		(1<<0);   //R0
	// REG 0x1c4
	rSYS_PAD_CTRL01 =
		(1<<28) | //G7
		(1<<24) | //G6
		(1<<20) | //G5
		(1<<16) | //G4
		(1<<12) | //G3
		(1<<8) |  //G2
		(1<<4) |  //G1
		(1<<0);   //G0
	// REG 0x1c8
	rSYS_PAD_CTRL02 =
		(1<<28) | //B7
		(1<<24) | //B6
		(1<<20) | //B5
		(1<<16) | //B4
		(1<<12) | //B3
		(1<<8) |  //B2
		(1<<4) |  //B1
		(1<<0);   //B0
	// REG 0x1cc
	rSYS_PAD_CTRL03 =
		(1<<12) | //lcd_hsync
		(1<<8) |  //lcd_vsync
		(1<<4) |  //lcd_clk
		(1<<0);	  //lcd_de
#endif

#if  0 //  Digital   TCON
	rSYS_PAD_CTRL00 = (3<<28) |(3<<24) |(3<<20) |(3<<16) |(3<<12) |(3<<8) | (3<<4) ;
	rSYS_PAD_CTRL01 = (3<<28) |(3<<24) |(3<<20) |(3<<16) |(3<<12) |(3<<8) | (3<<4) |(3<<0);
	rSYS_PAD_CTRL02 = (3<<28) |(3<<24) |(3<<20) |(3<<16) |(3<<12) |(3<<8) | (3<<4) |(3<<0);
	rSYS_PAD_CTRL03 = (3<<12) |(3<<8) | (3<<4) |(3<<0);	
#endif
}

extern int boot_complete;
int ark_disp_set_lcd_panel_type(int panel_type)
{
	/* disable LED */

	ark_disp_disable_led();

	ark_disp_lcdl2rscanpadhigh();

	/* initialize LCD control register */

	// CLCD_CONTROL REG0x04
	rLCD_CONTROL =
		0<<29 | // 0=Prgb bit reverse disable
		0<<28 | // 0=0-255 out yuv range in LCD layer
		// (1=16-235, for "601" standard)
		ARK_DISP_LCD_AXI_CMD_ID_RD<<23 | // 26-23: 0=axi_read_cmd_id
		0<<21 | // 22-21: 0=generate intr at start of LCD vsync
		0<<18 | // 20-18: 0=BGR (RGB output mode)
		// this reg used to adjust sRGB screen data polarity
		0<<15 | // 0=deinterlace output mode in LCD layer (?v????X?Ҧ?)
		0<<11 | // 12-11: 0=generate intr at start of TV vsync
		0<<10 | // 0=disable write back for video 2 layer
		0<<9 | // disable osd layer 3
		0<<8 | // disable osd layer 2
		//0<<7 | // disable osd layer 1
		0<<6 | // disable video 2 layer
		0<<5 | // disable video layer
		panel_type<<2 | // 4-2: 0=parallel screen 24 bit
		0<<1 | // 0=srgb output signal
		// (1=output syuv422 signal)
		1<<0;  // 1=enable LCD display output
	/* set up LCD timing control registers */
	/* 800x480 screen size */

	// CLCD_TIMING0 REG0x08
	rLCD_TIMING0 =
		HSW<<20 | // 29-20: h sync pulse width (in CLCP periods)
		HBP<<10 | // 19-10: h back porch, in pixel clks (or CLCP periods)
		HFP<<0;   // 9-0: h front porch, in pixel clocks (or CLCP periods)

	// CLCD_TIMING1 REG0x0c
	rLCD_TIMING1 =
		VFP<<19 | // 28-19: v front porch, nb of inactive lines (line clocks)
		// at the end of frame
		VSW<<13 | // 18-13: v sync pulse width, in horizontal sync lines
		// (line clock)
		(ARK_DISP_LCD_WIDTH-1)<<0;
	// 12-0: clocks per line, nb of actual CLCP clks to
	// LCD panel on each line

	// CLCD_TIMING2 REG0x10
	rLCD_TIMING2 =
		IOE<<23 | // invert output enable
		// 0=CLAC out pin is active HIGH in TFT mode,
		// 1=active LOW
		IHS<<22 | // invert horizontal sync
		// 0=CLLP pin is active HIGH, 1=active LOW
		IVS<<21 | // invert vertical sync
		// 0=CLFP pin is active HIGH, 1=active LOW
		(ARK_DISP_LCD_HEIGHT-1)<<10 |
		// 20-10: line per screen, nb of active lines per screen
		VBP<<0;   // 9-0: vertical back porch, nb of inactive lines at the start of a frame

	/* initialize YUV to RGB coefficient registers */

	// Y2R_COEF321(The Coefficient321 Value for Y2R) REG0x11c
	rLCD_Y2R_COEF321 =
		298<<0  // 9-0: K1_REG
		|91<<10 // 19-10: K2_REG
		|425<<20;   // 29-20: K3_REG

	// Y2R_COEF654(The Coefficient654 Value for Y2R) REG0x120
	rLCD_Y2R_COEF654 =
		96<<0   // 9-0: K4_REG
		|184<<10    // 19-10: K5_REG
		|465<<20;   // 29-20: K6_REG


	// Y2R_COEF7(The Coefficient7 Value for Y2R) REG0x124
	rLCD_Y2R_COEF7 =
		(rLCD_Y2R_COEF7&0xffffcc00)
		|41<<0  // 9-0: K7_REG
		|1<<12  // K7_sign
		|0<<13; // K2_sign

	/* set up vp */

	// VIDEO_VP_REG_0 REG0x1d4
	rLCD_VIDEO_VP_REG_0 =
		(0<<12) |   // 0=enable SEPIA y by-pass mode
		(0<<8) |    // 11-8: 0=enable vde module output (normal)
		(0<<5) |    // 7-5: 0=raw data
		(1<<4) |    // 1=enable GLE by pass mode
		(0<<3) |    // 0=enable BLE by pass mode
		(1<<2) |    // 1=enable GRA by pass mode
		(1<<1) |    // 1=enable NE by pass mode
		(1<<0);     // 1=enable SE by pass mode
	//        = 0x17;						//all vp bypass


	// VIDEO2_VP_REG_0 REG0x130
	rLCD_VIDEO2_VP_REG_0 =
		(0<<12) |   // 0=enable SEPIA y by-pass mode
		(0<<8) |    // 11-8: 0=enable vde module output (normal)
		(0<<5) |    // 7-5: 0=raw data
		(1<<4) |    // 1=enable GLE by pass mode
		(0<<3) |    // 0=enable BLE by pass mode
		(1<<2) |    // 1=enable GRA by pass mode
		(1<<1) |    // 1=enable NE by pass mode
		(1<<0);     // 1=enable SE by pass mode    
	//        = 0x17;						//all vp bypass

	ark_disp_select_lcd_panel_pad();

	ark_disp_config_lcd_clk();

	ark_disp_enable_led();

	/* config pwm0 and pwm1 */
	ark_disp_select_pwm0_pad();
	ark_disp_select_pwm1_pad();
	return 0;
}

void ark_disp_set_lcd_en(int enable)
{
	// CLCD_ENABLE REG0x00
	rLCD_EANBLE = enable;
}

void ark_disp_set_lcd_backcolor(unsigned char y, unsigned char cb,
		unsigned char cr)
{
	/* CLCD_LCD_BACK_COLOR REG0x50 */
	rLCD_BACK_COLOR = 
		y<<16 | // 23-16: y of back color
		cb<<8 | // 15-8: Cb of back color
		cr;     // 7-0: Cr of back color
	/* NOTE: if video and osd layers are all smaller than the panel, the left
	 * area will be filled with the back color
	 */
}
