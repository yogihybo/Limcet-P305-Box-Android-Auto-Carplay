#include <asm-generic/gpio.h>
#include <asm/arch/ark-common.h>
#include "ark1668_lcd.h"

extern struct screen_info *g_screen_info;
extern display_updatepara g_display_para;

unsigned int ark_sys_read(unsigned int reg_offset)
{
	return *((volatile unsigned int *)(SYS_BASE+reg_offset));
}

void ark_sys_write(unsigned int reg_offset, unsigned long value)
{
	*((volatile unsigned int *)(SYS_BASE+reg_offset)) = value;
}

void ark_sys_pad_config(unsigned int reg_offset, unsigned int mask, 
	unsigned int bit_offset, unsigned int val)
{
	unsigned int reg;

	reg = ark_sys_read(reg_offset);
	reg &= ~(mask << bit_offset);
	reg |= ((val & mask) << bit_offset);
	ark_sys_write(reg_offset, reg);
}

void ark_osd_en_layer(int id, unsigned int enable)
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

void ark_set_osd_frame_mode(int id, int frame)
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
#if ARK_DISPLAY_ALL_MODE
void ark_osd_en_layer_tvenc(int id, unsigned int enable)
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

void ark_video_en_layer(int id, int enable)
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
    default: 
        return;
    }
    if (enable)
        rLCD_CONTROL |= (1 << pos);  /* enable video layer on LCD screen */
    else
        rLCD_CONTROL &= ~(1 << pos); /* disable video layer on LCD screen */
}

void ark_video_en_layer_tvenc(int id, int enable)
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

#endif
void ark_lcd_select_pad(void)
{
	rSYS_PAD_CTRL00 = (1<<28) |(1<<24) |(1<<20) |(1<<16) |(1<<12) |(1<<8) | (1<<4) |(1<<0);
	rSYS_PAD_CTRL01 = (1<<28) |(1<<24) |(1<<20) |(1<<16) |(1<<12) |(1<<8) | (1<<4) |(1<<0);
    rSYS_PAD_CTRL02 = (1<<28) |(1<<24) |(1<<20) |(1<<16) |(1<<12) |(1<<8) | (1<<4) |(1<<0);
    rSYS_PAD_CTRL03 = (1<<12) |(1<<8) | (1<<4) |(1<<0);
}

void ark_lcd_clk_cfg(void)
{
	rSYS_PER_CLK_EN |= 1 << 4;
	rSYS_LCD_CLK_CFG = (0<<31) | (0x36<<25) | (1<<23) | (4<<19) | (1<<7) | (3<<4) | 0; 
	//rSYS_LCD_CLK_CFG = 0xa0e80090;
}

void ark_select_pad_for_pwm0(void)	
{
	u32 val;
	val = rSYS_PAD_CTRL09;
	val &= ~(0x3<<16);
	val |= (0x1<<16);
	val |= (0x2<<0);
	rSYS_PAD_CTRL09 = val;
}

void ark_select_pad_for_pwm1(void)	
{
	u32 val;
	val = rSYS_PAD_CTRL09;
	val &= ~(0x3<<2);
	val |= (0x2<<2);
	val &= ~(0x03 << 18);
	val |= (0x1<<18);//set GPIO79 as pwm1 function
	rSYS_PAD_CTRL09 = val;
}

void ark_select_pad_for_pwm3(void)	
{
	u32 val;
	val = rSYS_PAD_CTRL09;
	val &= ~(0x3<<22);
	val |= (1 << 22);
	rSYS_PAD_CTRL09 = val;
}
void ark_backlight_config_f(int screen_id)
{

	if (screen_id == SCREEN_QUN700) {
		/////
	}else if (screen_id == SCREEN_CLAA101) {
		rPWM_ENA1 	= 0x0;
		rPWM_CNTR1	= (50000 * 24) / 1000;
		rPWM_DUTY1	= (50000 * 24) / 1000;
		rPWM_ENA1 	= 0x1;
		ark_select_pad_for_pwm1();
		udelay(200);
		//SetGPIODataDirection(81,euOutputPad);
		//SetGPIOPadData(81, 1);  //open backlight	
		gpio_direction_output(81,1);
	}else if (screen_id == SCREEN_C101EAN) {
		/////
	}
	
}

void ark_backlight_config(int screen_id)
{
	int bkl_val = 30;

	if (screen_id == SCREEN_QUN700) {
		//SetGPIODataDirection(81,euOutputPad);
		mdelay(50);
		rPWM_ENA1 	= 0x0;
		rPWM_CNTR1	= (50000 * 24) / 1000;
		rPWM_DUTY1	= (bkl_val*500 * 24)/ 1000; 
		rPWM_ENA1 	= 0x1;
		ark_select_pad_for_pwm1();
		//SetGPIOPadData(81, 1);  //open backlight
		gpio_direction_output(81,1);
	}else if (screen_id == SCREEN_CLAA101) {
		rPWM_ENA1	= 0x0;
		rPWM_CNTR1	= (50000 * 24) / 1000;
		rPWM_DUTY1	= (bkl_val*500 * 24)/ 1000; 
		rPWM_ENA1	= 0x1;
		ark_select_pad_for_pwm1();
	}else if (screen_id == SCREEN_C101EAN) {
		//selct gpio110(power), gpio65(leden), gpio56(backlight) pad
		//power on when system power on, no need config here 
		//rSYS_PAD_CTRL0B &= ~(1 << 25); 
		rSYS_PAD_CTRL08 &= ~(3 << 6); 
		rSYS_PAD_CTRL06 &= ~(0xF << 4); 

		//config init status
		//rGPIO_PD_MOD &= ~(1 << 14);
		//rGPIO_PD_RDATA &= (1 << 14);
		rGPIO_PC_MOD &= ~(1 << 1);
		rGPIO_PC_RDATA &= (1 << 1);
		rGPIO_PB_MOD &= ~(1 << 24);
		rGPIO_PB_RDATA &= (1 << 24);	
		udelay(100);

		//power on
		//rGPIO_PD_RDATA |= (1 << 14);
		//mdelay(30);

		//leden on
		rGPIO_PC_RDATA |= (1 << 1);
		mdelay(200);
		
		//config backlight
		rPWM_ENA3 	= 0x0;
		rPWM_CNTR3 	= (50000 * 24) / 1000;;
		rPWM_DUTY3 	= (bkl_val*500 * 24)/ 1000; ;
		rPWM_ENA3	= 0x1;		
		ark_select_pad_for_pwm3();	
	}
       
}

void ark_set_video_alpha(unsigned char alpha)
{
    rLCD_VIDEO_VIDEO2_BLD_COEF &= ~0xFF;
	rLCD_VIDEO_VIDEO2_BLD_COEF |= alpha;
}

void ark_set_video2_alpha(unsigned char alpha)
{
   rLCD_VIDEO_VIDEO2_BLD_COEF &= ~(0xFF<<8);
   rLCD_VIDEO_VIDEO2_BLD_COEF |= alpha<<8;
}

void ark_set_win1_alpha(u8 alpha)
{
   rLCD_OSD1_CTL &= ~0xFF;
   rLCD_OSD1_CTL |= alpha;
}

void ark_set_win2_alpha(u8 alpha)
{
   rLCD_OSD2_CTL &= ~0xFF;
   rLCD_OSD2_CTL |= alpha;
}

void ark_set_win3_alpha(u8 alpha)
{
   rLCD_OSD3_CTL &= ~0xFF;
   rLCD_OSD3_CTL |= alpha;
}

#if ARK_DISPLAY_ALL_MODE
void ark_set_gamma(void)
{

	rLCD_GAMMA_REG_0 = 3;
	rLCD_GAMMA_REG_1 = (0X09 << 24) | (0X09 << 16) | (0X04 << 8) | 0X04;
	rLCD_GAMMA_REG_2 = (0X15 << 24) | (0X15 << 16) | (0X0F << 8) | 0X0F;
	rLCD_GAMMA_REG_3 = (0X24 << 24) | (0X24 << 16) | (0X1C << 8) | 0X1C;
	rLCD_GAMMA_REG_4 = (0X34 << 24) | (0X34 << 16) | (0X2C << 8) | 0X2C;
	rLCD_GAMMA_REG_5 = (0X44 << 24) | (0X44 << 16) | (0X3C << 8) | 0X3C;
	rLCD_GAMMA_REG_6 = (0X56 << 24) | (0X56 << 16) | (0X4D << 8) | 0X4D;
	rLCD_GAMMA_REG_7 = (0X68 << 24) | (0X68 << 16) | (0X5F << 8) | 0X5F;
	rLCD_GAMMA_REG_8 = (0X7C << 24) | (0X7C << 16) | (0X72 << 8) | 0X72;
	rLCD_GAMMA_REG_9 = (0X8F << 24) | (0X8F << 16) | (0X85 << 8) | 0X85;
	rLCD_GAMMA_REG_10 = (0XA3 << 24) | (0XA3 << 16) | (0X99 << 8) | 0X99;
	rLCD_GAMMA_REG_11 = (0XB5 << 24) | (0XB5 << 16) | (0XAC << 8) | 0XAC;
	rLCD_GAMMA_REG_12 = (0XC6 << 24) | (0XC6 << 16) | (0XBD << 8) | 0XBD;
	rLCD_GAMMA_REG_13 = (0XD5 << 24) | (0XD5 << 16) | (0XCD << 8) | 0XCD;
	rLCD_GAMMA_REG_14 = (0XE3 << 24) | (0XE3 << 16) | (0XDC << 8) | 0XDC;
	rLCD_GAMMA_REG_15 = (0XF1 << 24) | (0XF1 << 16) | (0XEA << 8) | 0XEA;
	rLCD_GAMMA_REG_16 = (0XFF << 24) | (0XFF << 16) | (0XF8 << 8) | 0XF8;
	rLCD_GAMMA_REG_17 = (0X09 << 24) | (0X09 << 16) | (0X04 << 8) | 0X04;
	rLCD_GAMMA_REG_18 = (0X15 << 24) | (0X15 << 16) | (0X0F << 8) | 0X0F;
	rLCD_GAMMA_REG_19 = (0X24 << 24) | (0X24 << 16) | (0X1C << 8) | 0X1C;
	rLCD_GAMMA_REG_20 = (0X34 << 24) | (0X34 << 16) | (0X2C << 8) | 0X2C;
	rLCD_GAMMA_REG_21 = (0X44 << 24) | (0X44 << 16) | (0X3C << 8) | 0X3C;
	rLCD_GAMMA_REG_22 = (0X56 << 24) | (0X56 << 16) | (0X4D << 8) | 0X4D;
	rLCD_GAMMA_REG_23 = (0X68 << 24) | (0X68 << 16) | (0X5F << 8) | 0X5F;
	rLCD_GAMMA_REG_24 = (0X7C << 24) | (0X7C << 16) | (0X72 << 8) | 0X72;
	rLCD_GAMMA_REG_25 = (0X8F << 24) | (0X8F << 16) | (0X85 << 8) | 0X85;
	rLCD_GAMMA_REG_26 = (0XA3 << 24) | (0XA3 << 16) | (0X99 << 8) | 0X99;
	rLCD_GAMMA_REG_27 = (0XB5 << 24) | (0XB5 << 16) | (0XAC << 8) | 0XAC;
	rLCD_GAMMA_REG_28 = (0XC6 << 24) | (0XC6 << 16) | (0XBD << 8) | 0XBD;
	rLCD_GAMMA_REG_29 = (0XD5 << 24) | (0XD5 << 16) | (0XCD << 8) | 0XCD;
	rLCD_GAMMA_REG_30 = (0XE3 << 24) | (0XE3 << 16) | (0XDC << 8) | 0XDC;
	rLCD_GAMMA_REG_31 = (0XF1 << 24) | (0XF1 << 16) | (0XEA << 8) | 0XEA;
	rLCD_GAMMA_REG_32 = (0XFF << 24) | (0XFF << 16) | (0XF8 << 8) | 0XF8;
	rLCD_GAMMA_REG_33 = (0X09 << 24) | (0X09 << 16) | (0X04 << 8) | 0X04;
	rLCD_GAMMA_REG_34 = (0X15 << 24) | (0X15 << 16) | (0X0F << 8) | 0X0F;
	rLCD_GAMMA_REG_35 = (0X24 << 24) | (0X24 << 16) | (0X1C << 8) | 0X1C;
	rLCD_GAMMA_REG_36 = (0X34 << 24) | (0X34 << 16) | (0X2C << 8) | 0X2C;
	rLCD_GAMMA_REG_37 = (0X44 << 24) | (0X44 << 16) | (0X3C << 8) | 0X3C;
	rLCD_GAMMA_REG_38 = (0X56 << 24) | (0X56 << 16) | (0X4D << 8) | 0X4D;
	rLCD_GAMMA_REG_39 = (0X68 << 24) | (0X68 << 16) | (0X5F << 8) | 0X5F;
	rLCD_GAMMA_REG_40 = (0X7C << 24) | (0X7C << 16) | (0X72 << 8) | 0X72;
	rLCD_GAMMA_REG_41 = (0X8F << 24) | (0X8F << 16) | (0X85 << 8) | 0X85;
	rLCD_GAMMA_REG_42 = (0XA3 << 24) | (0XA3 << 16) | (0X99 << 8) | 0X99;
	rLCD_GAMMA_REG_43 = (0XB5 << 24) | (0XB5 << 16) | (0XAC << 8) | 0XAC;
	rLCD_GAMMA_REG_44 = (0XC6 << 24) | (0XC6 << 16) | (0XBD << 8) | 0XBD;
	rLCD_GAMMA_REG_45 = (0XD5 << 24) | (0XD5 << 16) | (0XCD << 8) | 0XCD;
	rLCD_GAMMA_REG_46 = (0XE3 << 24) | (0XE3 << 16) | (0XDC << 8) | 0XDC;
	rLCD_GAMMA_REG_47 = (0XF1 << 24) | (0XF1 << 16) | (0XEA << 8) | 0XEA;
	rLCD_GAMMA_REG_48 = (0XFF << 24) | (0XFF << 16) | (0XF8 << 8) | 0XF8;

}
#endif

void ark_gamma_init(void)
{
#if ARK_DISPLAY_ALL_MODE

	int i;
	unsigned int * reg_pos = &rLCD_GAMMA_REG_1;

	//set gamma from flash
	if ((g_display_para.gammainfo.gamma_en== 0x03) && (g_display_para.flag_gamma== GAMMA_INFO_FLAG))
	{
		printf("set gamma info from flash.\n"); 
		rLCD_GAMMA_REG_0 = 3;
		for (i = 0;i < GAMMA_REG_MAX;i++)
		{
			*reg_pos = g_display_para.gammainfo.gamma_regval[i];
			//printf("gamma[%d]0x%x = 0x%x.\n",i,reg_pos,*reg_pos); 
			reg_pos++;
		}
	}
	else
	{
		//ark_set_gamma();
		//printf("set gamma info default.\n"); 
	}
#endif
}
void ark_init_clcd_reg(void)
{
	rLCD_CONTROL = 
		0<<29 | // 0=Prgb bit reverse disable
		0<<28 | // 0=0-255 out yuv range in LCD layer
				// (1=16-235, for "601" standard)
		5<<23 | // 26-23: 0=axi_read_cmd_id
		0<<21 | // 22-21: 0=generate intr at start of LCD vsync
		0<<18 | // 20-18: 0=BGR (RGB output mode)
				// this reg used to adjust sRGB screen data polarity
		0<<15 | // 0=deinterlace output mode in LCD layer (?v????X?Ҧ?)
		0<<11 | // 12-11: 0=generate intr at start of TV vsync
		0<<10 | // 0=disable write back for video 2 layer
		0<<9  | // disable osd layer 3
		0<<8  | // disable osd layer 2
		0<<7  | // disable osd layer 1
		0<<6  | // disable video 2 layer
		0<<5  | // disable video layer
		0<<2  | // 4-2: 0=parallel screen 24 bit
		0<<1  | // 0=srgb output signal
				// (1=output syuv422 signal)
		1<<0;   // 1=enable LCD display output 

	rLCD_TIMING0 = (HSW<<20) | (HBP<<10) | (HFP<<0);	
	rLCD_TIMING1 = (VFP<<19) | (VSW<<13) | ((LCD_W-1)<<0);
	rLCD_TIMING2 = (IOE<<23) | (IHS<<22) | (IVS<<21) | ((LCD_H-1)<<10) | (VBP<<0);	
		
	rLCD_Y2R_COEF321 = 298<<0 | 91<<10 | 425<<20;
	rLCD_Y2R_COEF654 = 96<<0 | 184<<10| 465<<20;
	rLCD_Y2R_COEF7 = (rLCD_Y2R_COEF7&0xffffcc00) | 41<<0 | 1<<12 | 0<<13;

	rLCD_BACK_COLOR = 
        0x10<<16 | // 23-16: y of back color
        0x80<<8 | // 15-8: Cb of back color
        0x80;     // 7-0: Cr of back color;		

	/*set vp*/
	rLCD_VIDEO_VP_REG_0  = 0x17;				//all vp bypass
	rLCD_VIDEO2_VP_REG_0 = 0x17;				//all vp bypass

	/*set vde*/
	rLCD_VIDEO_VP_REG_1  = (0x40<<16) | (0x80<<8) | 0x80;
	rLCD_VIDEO2_VP_REG_1 = (0x40<<16) | (0x80<<8) | 0x80;
	rLCD_OSD1_VP_REG_1 = (0x40<<16) | (0x80<<8) | 0x80; 
	rLCD_OSD2_VP_REG_1 = (0x40<<16) | (0x80<<8) | 0x80; 
	rLCD_OSD3_VP_REG_1 = (0x40<<16) | (0x80<<8) | 0x80; 

	/*set alpha*/
	ark_set_video_alpha(0xFF);
	ark_set_video2_alpha(0xFF);
	ark_set_win1_alpha(0xFF);
	ark_set_win2_alpha(0xFF);
	ark_set_win3_alpha(0xFF);

    rLCD_DITHERING = 1 << 23;

	rLCD_EANBLE = 1;	
}

void ark_set_video_priority(int level)
{
    rLCD_BLD_MODE_LCD_REG0 &= ~(0x7<<0);
	rLCD_BLD_MODE_LCD_REG0 |= (level<<0);
}
		 
void ark_set_video2_priority(int level)
{
    rLCD_BLD_MODE_LCD_REG1 &= ~(0x7<<0);
    rLCD_BLD_MODE_LCD_REG1 |= (level<<0);
}

void ark_set_win1_priority(int level)
{
    rLCD_BLD_MODE_LCD_REG0 &= ~(0x7<<8);
    rLCD_BLD_MODE_LCD_REG0 |= (level << 8);
}

void ark_set_win2_priority(int level)
{
    rLCD_BLD_MODE_LCD_REG0 &= ~(0x7<<16);
    rLCD_BLD_MODE_LCD_REG0 |= (level << 16);
}

void ark_set_win3_priority(int level)
{
    rLCD_BLD_MODE_LCD_REG0 &= ~(0x7<<24);
    rLCD_BLD_MODE_LCD_REG0 |= (level<<24);
}

void ark_set_video_priority_tvenc(int level)
{
	rLCD_BLD_MODE_TV_REG0 &= ~(0x7<<0);
	rLCD_BLD_MODE_TV_REG0 |= (level <<0);
}
		 
void ark_set_video2_priority_tvenc(int level)
{
	rLCD_BLD_MODE_TV_REG1 &= ~(0x7<<0);// video2
	rLCD_BLD_MODE_TV_REG1 |= (level <<0);
}

void ark_set_win1_priority_tvenc(int level)
{
	rLCD_BLD_MODE_TV_REG0 &= ~(0x7<<8);// win1
	rLCD_BLD_MODE_TV_REG0 |= (level << 8);
}

void ark_set_win2_priority_tvenc(int level)
{
	rLCD_BLD_MODE_TV_REG0 &= ~(0x7<<16);// win2
	rLCD_BLD_MODE_TV_REG0 |= (level << 16);
}

void ark_set_win3_priority_tvenc(int level)
{
	rLCD_BLD_MODE_TV_REG0 &= ~(0x7<<24);// win3
	rLCD_BLD_MODE_TV_REG0 |= (level <<24);
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

int ark_set_window_priority(int video_pri, int video2_pri, int win1_pri,int win2_pri,int win3_pri)
{
	if(video_pri+video2_pri+win1_pri+win2_pri+win3_pri != 10)
	{
		return 0;
	}

	ark_set_video_priority(video_pri);
	ark_set_video2_priority(video2_pri);
	ark_set_win1_priority(win1_pri);
	ark_set_win2_priority(win2_pri);
	ark_set_win3_priority(win3_pri);

	return 1;
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
void ark_display_video_image(unsigned int width,unsigned int height,unsigned int * buf,int format)
{
	unsigned int rgb_ycbcr_bypass=0;
	unsigned int y_uv_order=0;

	if(format >= SEQ_Y_UV422)
	{
		y_uv_order = 1;
		format &= 1;
		rgb_ycbcr_bypass = 1;
	}
	else if(format <= YUV444)//rgb565
	{
		rgb_ycbcr_bypass = 1;	
	}
	else
	{
		rgb_ycbcr_bypass = 0;	
	}

	/*0x400322 ----> 0x420312*/	
	rLCD_VIDEO_CTL = (1<<22) /*0x3c*/
					| (y_uv_order<<21)  // 0
					| (1<<17)
					| (1<<9) 
					| (1<<8) 
					| (1<<5) 
					| (rgb_ycbcr_bypass<<4) // 0
					| (format<<0);	// 2 

	if(format == SEQ_YUV420)
		rLCD_VIDEO_CTL |= (1<<11);
/*swidth=3120 c30, sheight=4208 1070*/
	rLCD_VIDEO_WIN_POINT 	= 0; 								/*0x15c*/
	rLCD_VIDEO_WIN_SIZE 	= (height<<12) | (width<<0); 		/*0x40*/ 
	rLCD_VIDEO_POSITION 	= 0;								/*0x4c*/
	rLCD_VIDEO_SIZE 		= (height<<12) | (width<<0);		/*0x44*/
	rLCD_VIDEO_SOURCE_SIZE 	= (height << 12) | (width);			/*0x16c*/
	rLCD_VIDEO_ADDR1 		= (unsigned int )buf;   			/*0x054*/
	rLCD_VIDEO_ADDR2 		= (unsigned int )buf + width*height; /*0x058*//*c3bc280*/

	if(format == SEQ_YUV420)
	{
		rLCD_VIDEO_ADDR3 = rLCD_VIDEO_ADDR2 + width*height/4;
	}
	else 
	{
		rLCD_VIDEO_ADDR3 = rLCD_VIDEO_ADDR2 + width*height/2;  /*0x5c*/
	}

   	rLCD_CONTROL |= (1<<5);
}

int ark_set_window_priority_tvenc(int video_pri, int video2_pri, int win1_pri,int win2_pri,int win3_pri)
{
	if(video_pri+video2_pri+win1_pri+win2_pri+win3_pri != 10)
	{
		return 0;
	}

	ark_set_video_priority_tvenc(video_pri);
	ark_set_video2_priority_tvenc(video2_pri);
	ark_set_win1_priority_tvenc(win1_pri);
	ark_set_win2_priority_tvenc(win2_pri);
	ark_set_win3_priority_tvenc(win3_pri);

	return 1;
}

void ark_set_osd_image(enum DISP_OSD_LAYER_ID  layer_id,
	                  int format, int width, int height)
{
	unsigned int rgb_ycbcr_bypass;
	unsigned int rgb_order = 0; 
	unsigned int yuv_order = 0;
	unsigned int order;

	order = DispGetYUVOrder(format);
	format = DispGetYUVFormat(format);
	
	if(format == DISP_YUV422 || format == DISP_YUV444)
	{
		rgb_ycbcr_bypass = 1;	
		yuv_order = order;
	}
	else
	{
		rgb_ycbcr_bypass = 0;	
		rgb_order = order;
	}

	switch(layer_id)
	{
	case OSD1_LAYER:
		rLCD_OSD1_SIZE = (height<<12) | width;
		rLCD_OSD1_SOURCE_SIZE = (height << 12) | (width);
		rLCD_OSD1_WIN_POINT = (0 << 12) | (0);
		rLCD_OSD1_CTL &= ~(0x7FF<<12);
		rLCD_OSD1_CTL |= (yuv_order << 21) | (rgb_order << 18) |
			(1 << 17) | (rgb_ycbcr_bypass << 16) | (format << 12);
		break;
	case OSD2_LAYER:
		rLCD_OSD2_SIZE = (height<<12) | width;
		rLCD_OSD2_SOURCE_SIZE = (height << 12) | (width);
		rLCD_OSD2_WIN_POINT = (0 << 12) | (0);
		rLCD_OSD2_CTL &= ~(0x7FF<<12);
		rLCD_OSD2_CTL |= (yuv_order << 21) | (rgb_order << 18) |
			(1 << 17) | (rgb_ycbcr_bypass << 16) | (format << 12);		
		break;
	case OSD3_LAYER:
		rLCD_OSD3_SIZE = (height<<12) | width;
		rLCD_OSD3_SOURCE_SIZE = (height << 12) | (width);
		rLCD_OSD3_WIN_POINT = (0 << 12) | (0);
		rLCD_OSD3_CTL &= ~(0x7FF<<12);
		rLCD_OSD3_CTL |= (yuv_order << 21) | (rgb_order << 18) |
			(1 << 17) | (rgb_ycbcr_bypass << 16) | (format << 12);		
		break;
	default:
		printf("error osd layer_id %d.\n", layer_id);
		break;
	}
}
#if ARK_DISPLAY_ALL_MODE
void ark_set_video_scaler(enum DISP_OSD_LAYER_ID  layer_id,
	                  int format, int src_width, int src_height,
	                  int dst_width, int dst_height, int interlace)
{
	unsigned int rgb_ycbcr_bypass=0;
	unsigned int vblank,hblank;
	unsigned int rgb_order = 0; 
	unsigned int yuv_order = 0;

	if(DispGetYUVFormat(format) == DISP_SEQ_YUV420)
	{
		if(src_width & 7)
		{
			printf("Parameter error, width is not the multiple of 8.\r\n");
			return;
		}
	}    	
	
	if(DispGetYUVFormat(format) <= DISP_YUV444)
	{
		rgb_ycbcr_bypass = 1;	
		yuv_order = DispGetYUVOrder(format);
	}
	else
	{
		rgb_ycbcr_bypass = 0;	
		rgb_order = DispGetYUVOrder(format);
	}	

	if(layer_id == VIDEO_LAYER)
	{ 
		rLCD_VIDEO_CTL = (1<<22) | ((yuv_order&3)<<17) | ((rgb_order&7)<<14)
			| (1<<8) | (1<<5) | (rgb_ycbcr_bypass<<4)  | (DispGetYUVFormat(format)<<0);	
		if(format == DISP_SEQ_Y_UV420 || format == DISP_SEQ_Y_UV422)
			rLCD_VIDEO_CTL |= (1<<21);
		if(DispGetYUVFormat(format) == DISP_SEQ_YUV420)         
			rLCD_VIDEO_CTL |= (1<<11);	
		rLCD_VIDEO_WIN_POINT = (0<< 12) | (0);
		rLCD_VIDEO_WIN_SIZE = (src_height<<12) | (src_width<<0);
		rLCD_VIDEO_POSITION = 0;
		rLCD_VIDEO_SIZE = (dst_height<<12) | (dst_width<<0);
		rLCD_VIDEO_SOURCE_SIZE = (src_height << 12) | (src_width);
		rLCD_VIDEO_SCALE_CTL = (1<<7) | (1<<5);
		if(dst_width < src_width)	   // enable filter when horizontal down scaler
			rLCD_VIDEO_SCALE_CTL |= (1<<6);
		/*When need cut after scaler,the dest size of the scaler must be the size 
		you wanted plus the cut number you wanted*/
		rLCD_VIDEO_RIGHT_BOTTOM_CUT_NUM = (0<<8) | (0);
		rLCD_VIDEO_SCAL_CTL0 = (0<<18) | (src_width*1024/dst_width);
		rLCD_VIDEO_SCAL_CTL1 = (0<<18) | (src_height*1024/dst_height);
		if(interlace)
		{
			rLCD_TV_CONTROL &= ~(1<<8);
			//when v scaler cof is 0x400,v scaler  bypass, now we  should change the cof to
			//force v scaler, otherwise there was sawtooth on picture
			if((rLCD_VIDEO_SCAL_CTL1 & 0x3FFFF) == 0x400)
				rLCD_VIDEO_SCAL_CTL1 = rLCD_VIDEO_SCAL_CTL1 - 1;
			rLCD_VIDEO_SCALE_CTL &= ~(7<<9);
			rLCD_VIDEO_SCALE_CTL |= (1<<9) | (1<<11);
		}		
		

	}
	else if(layer_id == VIDEO2_LAYER)
	{
		rLCD_VIDEO2_CTL = (1<<22) | ((yuv_order&3)<<17) | ((rgb_order&7)<<14)
						  | (1<<8) | (1<<5) | (rgb_ycbcr_bypass<<4)  | (DispGetYUVFormat(format)<<0);	
		if(format == DISP_SEQ_Y_UV420 || format == DISP_SEQ_Y_UV422)
			rLCD_VIDEO2_CTL |= (1<<21);
		if(DispGetYUVFormat(format) == DISP_SEQ_YUV420)         
			rLCD_VIDEO2_CTL |= (1<<13);	
		rLCD_VIDEO2_WIN_POINT = (0 << 12) | (0);
		rLCD_VIDEO2_WIN_SIZE = (src_height<<12) | (src_width<<0);
		rLCD_VIDEO2_POSITION = 0;
		rLCD_VIDEO2_SIZE = (dst_height<<12) | (dst_width<<0);
		rLCD_VIDEO2_SOURCE_SIZE = (src_height << 12) | (src_width);
		rLCD_VIDEO2_SCALE_CTL = (1<<7) | (1<<5);
		if(dst_width < src_width)	   // enable filter when horizontal down scaler
			rLCD_VIDEO2_SCALE_CTL |= (1<<6);
		/*When need cut after scaler,the dest size of the scaler must be the size 
		you wanted plus the cut number you wanted*/
		rLCD_VIDEO2_RIGHT_BOTTOM_CUT_NUM = (0<<8) | (0);
		rLCD_VIDEO2_SCAL_CTL0 = (0<<18) | (src_width*1024/dst_width);
		rLCD_VIDEO2_SCAL_CTL1 = (0<<18) | (src_height*1024/dst_height);		
		if(interlace)
		{
			rLCD_TV_CONTROL &= ~(1<<8);
			//when v scaler cof is 0x400,v scaler  bypass, now we  should change the cof to
			//force v scaler, otherwise there was sawtooth on picture
			if((rLCD_VIDEO2_SCAL_CTL1 & 0x3FFFF) == 0x400)
				rLCD_VIDEO2_SCAL_CTL1 = rLCD_VIDEO2_SCAL_CTL1 - 1;
			rLCD_VIDEO2_SCALE_CTL &= ~(7<<9);
			rLCD_VIDEO2_SCALE_CTL |= (1<<9) | (1<<11);
		}	
		if(rLCD_CONTROL & (1<<10))
		{
			rLCD_CONTROL &= ~(1<<10);
			rLCD_VIDEO2_CTL |= (1<<24);
			rLCD_VIDEO2_CTL &= ~(1<<24);			
		}
		
	}	
}

void ark_set_video_addr(enum DISP_VIDEO_LAYER_ID  layer_id, 
	                    unsigned int yrgbaddr, unsigned int cbcraddr,
	                    unsigned int craddr)
{
	if( layer_id == VIDEO_LAYER ) {
		rLCD_VIDEO_ADDR1 = yrgbaddr;
		rLCD_VIDEO_ADDR2 = cbcraddr;
		rLCD_VIDEO_ADDR3 = craddr;
	} else if ( layer_id == VIDEO2_LAYER ) {
		rLCD_VIDEO2_ADDR1 = yrgbaddr;
		rLCD_VIDEO2_ADDR2 = cbcraddr;
		rLCD_VIDEO2_ADDR3 = craddr;	
	} else printf("error video layer_id %d.\n", layer_id);
}

#endif
void ark_set_osd_addr(enum DISP_OSD_LAYER_ID  layer_id, unsigned int addr)
{
	if( layer_id == OSD1_LAYER ) rLCD_OSD1_ADDR = addr;
	else if ( layer_id == OSD2_LAYER ) rLCD_OSD2_ADDR = addr;
	else if ( layer_id == OSD3_LAYER ) rLCD_OSD3_ADDR = addr;
	else printf("error osd layer_id %d.\n", layer_id);
}

void ark_disp_wait_lcd_frame_int(void)
{
	// wait until LCD timing point intr happens (which is VSync here)
	rLCD_INTERRUPT_STATUS = 0;
	while(!(rLCD_INTERRUPT_STATUS & 0x01));
	// the timing point is set at bit22-21 on CLCD_CONTROL reg
}

void ark_disp_wait_tvenc_frame_int(void)
{
	// wait until TV timing point intr happens (which is VSYNC here)
	rLCD_INTERRUPT_STATUS = 0;
	while(!(rLCD_INTERRUPT_STATUS & 0x08));	
	// NOTE: the timing point is set at bit12-11 on CLCD_CONTROL reg
}

#if ARK_DISPLAY_ALL_MODE

void ark_disp_set_tvenc_output_mode(int interlace)
{
	if (interlace)
		rLCD_TV_CONTROL |= (1 << 8);
	else
		rLCD_TV_CONTROL &= ~(1 << 8);
}
void tvenc_reset(void)
{
	rSYS_ANALOG_REG0 &= ~(3<<19);
	rSYS_ANALOG_REG1 &= ~((3<<22) | (1<<17) | (1<<15) | (7<<3) | 1);
	rLCD_YPBPR_CTRL0 &= ~1;
	rLCD_TV_CONTROL = 0;
}

void set_dds_freq(unsigned int dds_clk)
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
	dto_inc = dds_cofe*dds_clk/(24*32);

	val = rSYS_DDS_CLK_CFG;
	val &= ~(0x3fffff);
	val |= dto_inc;
	rSYS_DDS_CLK_CFG = val;  
}
#endif
int is_interlace_tvenc(struct screen_info *screen)
{
	if(screen->screen_type == SCREEN_TYPE_VGA)
		return 0;
	else if(screen->screen_type == SCREEN_TYPE_CVBS || 
		    screen->screen_type == SCREEN_TYPE_ITU656)
		return 1;
	else if(screen->screen_type == SCREEN_TYPE_YPBPR)
	{
		if(screen->format == 0 || screen->format == 1 || screen->format == 6 ||
		   screen->format == 7 || screen->format == 8)
			return 1;
		else 
			return 0;
	}

	return 0;
}
#if ARK_DISPLAY_ALL_MODE
void initializa_tvenc_vga(struct screen_info *screen)
{
	tvenc_reset();

	//select pad
	rSYS_PAD_CTRL03 &= ~(0xFF<<8);
	rSYS_PAD_CTRL03 |= (5<<8) | (5<<12);

	//config timing
	rLCD_TIMING0_TV = ((screen->hsw-1)<<20)|((screen->hbp-1)<<10)|((screen->hfp-1)<<0);
	rLCD_TIMING1_TV = (screen->vfp<<19)|((screen->vsw-1)<<13)|(screen->width - 1);
	//rLCD_TIMING2_TV =(0<<23)|(0<<22)|(0<<21)|((screen->height - 1)<<10)| screen->vbp;
	rLCD_TIMING2_TV =(screen->de_active<<23)|(screen->hsync_active<<22)|(screen->vsync_active<<21)|((screen->height - 1)<<10)| screen->vbp;
	rLCD_TIMING_FRAME_START_CNT_TV = (screen->vsw-1) / 2;

	rLCD_TV_CONTROL |= (1<<31) | (1<<10) | 1;
 	rLCD_YPBPR_CTRL0 = (1<<0);

	//config clk
	rSYS_DEVICE_CLK_CFG2 &=~(0xf<<20); 
	rSYS_DEVICE_CLK_CFG2 |=(0x2<<20); //int_tv_clk = dds_clk:

	set_dds_freq(screen->clk_freq);
	rSYS_LCD_CLK_CFG &= ~(0x3f<<25);
	rSYS_LCD_CLK_CFG |= (1<<31)|(screen->clk_div1<<25);  // clk_freq/clk_div1 = ?M			

	udelay(10);
		
	//enable DAC
	rSYS_ANALOG_REG0 |= 3<<19;
	rSYS_ANALOG_REG1 |= (1<<22) | (1<<17) | (7<<3) | 1;	
	
}

static void cvbs_init_ntsc(void)
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
	#define      cr_burst_amp                   0x00   //pal 0x20     ntsc 0x00
	#define      slave_mode                     0x1
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
	#define      n20                            0x04
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
	#define      uv_order                       0
	#define      pal_mode                       0       //pal 0x1    ntsc 0x0 
	#define      invert_top                     0
	#define      sys625_50                      0
	#define      cphase_rst                     3
	#define      n22                            0
	#define      agc_pulse_level                0xa3
	#define      bp_pulse_level                 0xc8
	#define      n4                             0x15
	#define      n6                             0x05
	#define      n2                             0x15
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
	
	#define 	 vsync5 						0//  1 modify 190802  ntsc 0 pal 1
#if defined(CONFIG_CVBS_INIT_NEW)
	//#define 	 black_level					0x11a//0x110  modify 190802
	#define 	 black_level					0xf2//0x11a//0x110  modify 191123
	#define 	 white_level					0x380//0x36c modify 190802
	#define 	 sync_level 					0x10//0x0 modify 190802

	#define 	 cb_burst_amp					0x60 //0x20 modify 190802
	#define 	 cb_gain						0xf9//0x89	modify 190802
	#define 	 cr_gain						0xf9//0x89	modify 190802
	#define 	 firstvideoline 				0x15//0xe  modify 190802
	//#define 	 vsync5 						0//  1 modify 190802
	#define 	 vbi_blank_level				0xf0//0x128 modify 190802
	//printf("cvbs_init_ntsc,change level.\r\n");
#else
	#define 	 black_level					0xf2
	#define 	 white_level					0x320
	#define 	 sync_level 					0x48
	
	#define 	 cb_burst_amp					0x20
	#define 	 cb_gain						0x89
	#define 	 cr_gain						0x89
	#define 	 firstvideoline 				0xe
	//#define 	 vsync5 						1
	#define 	 vbi_blank_level				0x128
#endif
	rLCD_TV_PARAM_REG0 = chroma_freq_ntsc ;//\u017d\CB\u017d\u0160\B6\u0161\D2\E5 N\D6\C6 P\D6\C6
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

static void cvbs_init_pal(void)
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

#if defined(CONFIG_CVBS_INIT_NEW)
	#define 	 black_level					0x110
	#define 	 white_level					0x36c
	#define 	 sync_level 					0x0
	
	printf("cvbs_init_pal,change level.\r\n");
#else	
	#define 	 black_level					0xf2
	#define 	 white_level					0x320
	#define 	 sync_level 					0x48
#endif

	rLCD_TV_PARAM_REG0 = chroma_freq_palbg;//chroma_freq_ntsc ;//\u017d\CB\u017d\u0160\B6\u0161\D2\E5 N\D6\C6 P\D6\C6
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

void initializa_tvenc_cvbs(struct screen_info *screen)
{
	unsigned int val;
	
	tvenc_reset();

	//clk enable
	rSYS_PER_CLK_EN |= (1<<20);
	
	//config timing
	rLCD_TIMING0_TV = ((screen->hsw-1)<<20)|(0<<10)|(0<<0);
	rLCD_TIMING1_TV = (0<<19)|((screen->vsw-1)<<13)|(screen->width - 1);
	rLCD_TIMING2_TV =(0<<23)|(0<<22)|(0<<21)|((screen->height/2 - 1)<<10)| screen->vbp;
	rLCD_TIMING_FRAME_START_CNT_TV = (screen->vsw-1) / 2;
	rLCD_TV_HV_DELAY = 39;

	if(screen->format == CVBS_FORMAT_NTSC) {
		rLCD_TV_CONTROL |= (1<<1);	
		cvbs_init_ntsc();
	} else if (screen->format == CVBS_FORMAT_PAL) {
		rLCD_TV_CONTROL &= ~(1<<1);
		cvbs_init_pal();
	}
	rLCD_TV_CONTROL |= 1;
#if defined(CONFIG_CVBS_INIT_NEW)
	rLCD_TV_CONTROL |= (1<<9);//add 20190802
#endif
	//config clk
	set_dds_freq(216);
	//set_dds_freq(screen->clk_freq);
	
	//set div to get 13.5M clk		
	val = rSYS_LCD_CLK_CFG;
	val &= ~(0x7f<<25);
	val |= (4<<25);//add
	rSYS_LCD_CLK_CFG = val;	
	
	//select dds clk src
	val = rSYS_DEVICE_CLK_CFG2;                       
	val &= ~(0x1FF<<20);                              
	val |= (2<<20) | (4<<24) | (1<<28);               
	rSYS_DEVICE_CLK_CFG2 = val;  
	udelay(10);
		
	//enable DAC

	rSYS_ANALOG_REG1 |= (3<<22) | (1<<17) | (1<<15) | (1<<5);
}

void cvbs_dac_enable(void)
{
	int val = 0;
	char *s = NULL;

	s = getenv("uboot_cvbs_disable");
	if (s){
		strict_strtoul(s, 19, &val);
		if (val == 1){
                        rSYS_ANALOG_REG1 &= ~(1<<5);
                        printf("+++cvbs dac disable.\r\n");
                        return;
                }
	}

        rSYS_ANALOG_REG1 |= (1<<5);
        printf("cvbs dac enable.\r\n");
}

void initializa_tvenc_ypbpr(struct screen_info *screen)
{
	int lps = screen->height;
	int scan_mode = 0;
	unsigned int val;

	if(is_interlace_tvenc(screen)) {
		lps /= 2;
		scan_mode = 1;
	}	
	
	tvenc_reset();

	//enable clk
	rSYS_PER_CLK_EN |= (1<<24);

	//config timing
	rLCD_TIMING0_TV = ((screen->hsw-1)<<20)|(0<<10)|(0<<0);
	rLCD_TIMING1_TV = (0<<19)|((screen->vsw-1)<<13)|(screen->width - 1);
	rLCD_TIMING2_TV =(0<<23)|(0<<22)|(0<<21)|((lps - 1)<<10)| 0;
	rLCD_TIMING_FRAME_START_CNT_TV = (screen->vsw-1) / 2;
	rLCD_TV_HV_DELAY = 39;
	
	rLCD_TV_CONTROL = (scan_mode<<8) | (1<<0);
	
	if(screen->format <= 3)
		rLCD_YPBPR_CTRL0 = (1<<7)|(scan_mode<<6)|(screen->format<<2)|(1<<1)|(1<<0);
	else
		rLCD_YPBPR_CTRL0 = (0<<7)|(scan_mode<<6)|(screen->format<<2)|(1<<1)|(1<<0);

	rLCD_YPBPR_CTRL0 |= (1 << 0);

	//config clk
	//set_dds_freq(297);
	set_dds_freq(screen->clk_freq);
	
	val = rSYS_LCD_CLK_CFG;
	val &= ~(0x3f<<25);
	val |= (1<<31);
	
#if 1
	val |= screen->clk_div1<<25;
#else	
	switch(screen->format)
	{
	case YPBPR_FORMAT_480I:					//13.5M
	case YPBPR_FORMAT_576I:
		val |= 22<<25;
		break;
	case YPBPR_FORMAT_480P:					//27M
	case YPBPR_FORMAT_576P:
		val |= 11<<25;
		break;
	case YPBPR_FORMAT_720P60HZ:
	case YPBPR_FORMAT_720P50HZ:
	case YPBPR_FORMAT_1080I60HZ:
	case YPBPR_FORMAT_1080I50HZ:
	case YPBPR_FORMAT_1080I50HZ_1250: 		//74.25M
		val |= 4<<25;
		break;
	case YPBPR_FORMAT_1080P60HZ:         	//148.5Mhz
	case YPBPR_FORMAT_1080P50HZ:
		val |= 2<<25;
		break;
	default:
		break;
	}
#endif
	rSYS_LCD_CLK_CFG = val;

	//select dds clk src
	val = rSYS_DEVICE_CLK_CFG2;                       
	val &= ~(0x1FF<<20);                              
	val |= (2<<20) | (4<<24) | (1<<28);               
	rSYS_DEVICE_CLK_CFG2 = val;	

	udelay(10);
		
	//enable DAC
	rSYS_ANALOG_REG0 |= 3<<19; 
	rSYS_ANALOG_REG1 |= (1<<17) | (7<<3) | 1;
}

void initializa_tvenc_itu656(struct screen_info *screen)
{
	unsigned int val;
	
	tvenc_reset();

	//select pad
	rSYS_PAD_CTRL00 &= ~(0xFFFFFF<<8);
	rSYS_PAD_CTRL00 |= (7<<8) | (7<<12) | (7<<16) | (7<<20) | (7<<24) | (7<<28);
	
	rSYS_PAD_CTRL01 &= ~(0xFF<<8);
	rSYS_PAD_CTRL01 |= (7<<8) | (7<<12);

	rSYS_PAD_CTRL03 &= ~(0xF<<4);
	rSYS_PAD_CTRL03 |= (7<<4);  
	
	//config timing
	rLCD_TIMING0_TV = ((screen->hsw-1)<<20)|(0<<10)|(0<<0);
	rLCD_TIMING1_TV = (0<<19)|((screen->vsw-1)<<13)|(screen->width - 1);
	rLCD_TIMING2_TV =(0<<23)|(0<<22)|(0<<21)|((screen->height/2 - 1)<<10)| (0<<0);
	rLCD_TIMING_FRAME_START_CNT_TV = (screen->vsw-1) / 2;
	rLCD_TV_HV_DELAY = 0x22;

	rLCD_TV_CONTROL &= ~(1 << 1);
	rLCD_TV_CONTROL |= (1<<8)|(1<<7)|(screen->format<<1)|(1<<0);

	//config clk
    set_dds_freq(216);
	
    val = rSYS_DEVICE_CLK_CFG2;                      
	val &= ~(0x1FF<<20);//clk_54m = int_tv_clk/(clk_54m_div? clk_54m_div:1)
	val |= (2<<20) | (4<<24) | (1<<28);//clk_54m =216M(DDS)/4 = 54m
	rSYS_DEVICE_CLK_CFG2 = val;   
	 
	val = rSYS_LCD_CLK_CFG;
	val &= ~(1<<31); // clk_13p5m
	val &= ~(0x3f<<25);
	val |= (1<<24);// 1: ~ clk_27m  
	rSYS_LCD_CLK_CFG = val;

	//enable DAC
	rSYS_ANALOG_REG1 |= 1<<17;// rgb DAC enable
	rSYS_ANALOG_REG1 &=~(7<<3);	

}
#endif
void ark_display_initialize_common(void)
{
#if ARK_DISPLAY_ALL_MODE
	vp_info *vp  = &g_display_para.vpinfo;

        //set vp
        rLCD_VIDEO_VP_REG_0 = 0x17;						//all vp bypass
        rLCD_VIDEO2_VP_REG_0 = 0x17;						//all vp bypass

	rLCD_VIDEO_VP_REG_1  = (vp->video_hue<<24) | (vp->video_saturation<<16) | (vp->video_brightness<<8) | vp->video_contrast;
	rLCD_VIDEO2_VP_REG_1 = (vp->video2_hue<<24)| (vp->video2_saturation<<16)| (vp->video2_brightness<<8)| vp->video2_contrast;
	rLCD_OSD1_VP_REG_1 	 = (vp->osd1_hue<<24)  | (vp->osd1_saturation<<16)  | (vp->osd1_brightness<<8)  | vp->osd1_contrast;
	rLCD_OSD2_VP_REG_1   = (vp->osd2_hue<<24)  | (vp->osd2_saturation<<16)  | (vp->osd2_brightness<<8)  | vp->osd2_contrast;
	rLCD_OSD3_VP_REG_1   = (vp->osd3_hue<<24)  | (vp->osd3_saturation<<16)  | (vp->osd3_brightness<<8)  | vp->osd3_contrast;
#else
        rLCD_VIDEO_VP_REG_0 = 0x17;                                             //all vp bypass
        rLCD_VIDEO2_VP_REG_0 = 0x17;                                            //all vp bypass

        rLCD_VIDEO_VP_REG_1  = (0x0<<24) | (0x40<<16) | (0x80<<8) | 0x80;
        rLCD_VIDEO2_VP_REG_1 = (0x0<<24) | (0x40<<16) | (0x80<<8) | 0x80;
        rLCD_OSD1_VP_REG_1   = (0x0<<24) | (0x40<<16) | (0x80<<8) | 0x80;
        rLCD_OSD2_VP_REG_1   = (0x0<<24) | (0x40<<16) | (0x80<<8) | 0x80;
        rLCD_OSD3_VP_REG_1   = (0x0<<24) | (0x40<<16) | (0x80<<8) | 0x80;

#endif
	/*printf("vp set: video=0x%08x, video2=0x%08x, osd1=0x%08x, osd2=0x%08x, osd3=0x%08x \r\n", 
			rLCD_VIDEO_VP_REG_1, rLCD_VIDEO2_VP_REG_1,rLCD_OSD1_VP_REG_1, rLCD_OSD2_VP_REG_1,rLCD_OSD3_VP_REG_1);*/	

	//set gamma
	ark_gamma_init();

        //set alpha
        rLCD_VIDEO_VIDEO2_BLD_COEF &= ~0xFFFF;
        rLCD_VIDEO_VIDEO2_BLD_COEF |= 0xFFFF;

        rLCD_OSD1_CTL &= ~0xFF;
        rLCD_OSD1_CTL |= 0xFF;

        rLCD_OSD2_CTL &= ~0xFF;
        rLCD_OSD2_CTL |= 0xFF;

        rLCD_OSD3_CTL &= ~0xFF;
        rLCD_OSD3_CTL |= 0xFF;

	//set blend
	#ifdef BOOT_CONFIG_PIXEL_ALPHA	
	rLCD_BLD_MODE_LCD_REG0 &= ~(0xF << 12);
	rLCD_BLD_MODE_LCD_REG0 |= (0 << 12);	
	rLCD_BLD_MODE_LCD_REG1 |= (3 << 12);	
	rLCD_BLD_MODE_TV_REG0 &= ~(0xF << 12);
	rLCD_BLD_MODE_TV_REG0 |= (0 << 12); 
	rLCD_BLD_MODE_TV_REG1 |= (3 << 12); 

	rLCD_BLD_MODE_LCD_REG0 &= ~(0xF << 28);
	rLCD_BLD_MODE_LCD_REG0 |= (0 << 28);	
	rLCD_BLD_MODE_LCD_REG1 |= (3 << 16);	
	rLCD_BLD_MODE_TV_REG0 &= ~(0xF << 28);
	rLCD_BLD_MODE_TV_REG0 |= (0 << 28); 
	rLCD_BLD_MODE_TV_REG1 |= (3 << 16); 
	#else
        rLCD_BLD_MODE_LCD_REG0 &= ~(0xF << 12);
        rLCD_BLD_MODE_LCD_REG0 |= (2 << 12);	
        rLCD_BLD_MODE_LCD_REG1 |= (1 << 13);	
        rLCD_BLD_MODE_TV_REG0 &= ~(0xF << 12);
        rLCD_BLD_MODE_TV_REG0 |= (2 << 12);	
        rLCD_BLD_MODE_TV_REG1 |= (1 << 13);	
	rLCD_COLOR_KEY_MASK_VALUE_OSD1 = (1 << 24) | (BLACK_Y<<16) | (BLACK_U<<8) | BLACK_V;  
	#endif
	rLCD_VIDEO_BURST_CTL = 0x1B84;
	rLCD_VIDEO2_BURST_CTL = 0x1B84;
	
	rLCD_EANBLE = 1;		
}

void ark_display_initialize_rgbif(struct screen_info *screen)
{
	unsigned int val;
	
	if(screen->pad_unset == 1)
	{
		rSYS_PAD_CTRL00 = 0;
		rSYS_PAD_CTRL01 = 0;
		rSYS_PAD_CTRL02 = 0;
		rSYS_PAD_CTRL03&= ~0xFFFF;
		rGPIO_PA_MOD   |= (0xFFFFFFF << 2); 
		
		//printf("rgbif pad unset: PAD_CTRL00=0x%08x,PAD_CTRL01=0x%08x,PAD_CTRL02=0x%08x,PAD_CTRL03=0x%08x,rGPIO_PA_MOD=0x%08x \r\n", 
				//rSYS_PAD_CTRL00, rSYS_PAD_CTRL01,rSYS_PAD_CTRL02, rSYS_PAD_CTRL03,rGPIO_PA_MOD);	
	}
	else
	{
		rSYS_PAD_CTRL00 = (1<<28) |(1<<24) |(1<<20) |(1<<16) |(1<<12) |(1<<8) | (1<<4) |(1<<0);
		rSYS_PAD_CTRL01 = (1<<28) |(1<<24) |(1<<20) |(1<<16) |(1<<12) |(1<<8) | (1<<4) |(1<<0);
		rSYS_PAD_CTRL02 = (1<<28) |(1<<24) |(1<<20) |(1<<16) |(1<<12) |(1<<8) | (1<<4) |(1<<0);
		rSYS_PAD_CTRL03 = (1<<12) |(1<<8) | (1<<4) |(1<<0); 	
	}
	
	rSYS_PER_CLK_EN |= 1 << 4;

	val = rSYS_LCD_CLK_CFG;
	val &= ~((0x1F<<19) | 0x7FF);

	val |= (1<<23)
		| (screen->clk_div1 << 19)	//srgb_clock div factor, which divided from lcd clock defined in bit7
		| (screen->clk_source << 7)  //lcd clock select from system pll
		| (screen->clk_div2 << 4)  //lcd clock div factor, which divided from srgb_clock, syspll/13/1 = 393/13 =30.23M
		| 5;   //scaler clock div factor, it derived from lcd clock select switch defined in bit 7 

	rSYS_LCD_CLK_CFG = val;

	//config video2 scaler clk
	val = rSYS_DEVICE_CLK_CFG0;
	val &= ~(0xF << 24);
	val |= 4 << 24;
	rSYS_DEVICE_CLK_CFG0 = val;

	rSYS_CLK_DLY_REG &= ~(1 << 16);
	rSYS_CLK_DLY_REG |= (screen->vclk_active << 16);
	rLCD_CONTROL = (rLCD_CONTROL & (0xB << 6)) | (0<<29) | (0<<28) | (6<<23) | (3<<21) | (screen->rgb_mode<<18) | (1<<0);	
        	
	rLCD_TIMING0 = (screen->hsw<<20) | (screen->hbp<<10) | (screen->hfp<<0);	
    rLCD_TIMING1 = (screen->vfp<<19) | (screen->vsw<<13) | ((screen->width-1)<<0);
    rLCD_TIMING2 = (screen->hsync_active << 22) | (screen->vsync_active << 21) | 
		           (screen->de_active << 23) | ((screen->height-1)<<10) | screen->vbp;

	rLCD_Y2R_COEF321 = 298<<0
              |91<<10
              |425<<20;
    rLCD_Y2R_COEF654 = 96<<0
             |184<<10
             |465<<20;
    rLCD_Y2R_COEF7 = (rLCD_Y2R_COEF7 & 0xffffcc00)
             |41<<0
             |1<<12
             |0<<13;

    rLCD_BACK_COLOR = (BLACK_Y<<16) | (BLACK_U<<8) | BLACK_V;		//set lcd back color

	ark_set_window_priority(4, 1, 2, 0, 3);
	ark_set_window_priority_tvenc(0, 4, 1, 2, 3);

    ark_display_initialize_common();

}

void ark_display_initialize_lvdsif(struct screen_info *screen)
{
	unsigned int val;

	rSYS_PAD_CTRL08 |= (1<<31);	             // 1, select LVDS PAD
 	rSYS_PAD_CTRL09 &= ~(0x1ffff<<0);   // select : b,c,d,ck  pn 
	rSYS_PAD_CTRL00 &= ~(0xff<<0);         //select:  lvds_ap/an

	rSYS_ANALOG_REG1 |= 1 << 26;

	val = rSYS_LCD_CLK_CFG;
	val &= ~((0x1F<<19) | 0x7FF);

	val |= (1<<23)
		| (screen->clk_div1 << 19)	//srgb_clock div factor, which divided from lcd clock defined in bit7
		| (screen->clk_source << 7)  //lcd clock select from system pll
		| (screen->clk_div2 << 4)  //lcd clock div factor, which divided from srgb_clock, syspll/13/1 = 393/13 =30.23M
		| 5;   //scaler clock div factor, it derived from lcd clock select switch defined in bit 7 

	rSYS_LCD_CLK_CFG = val;

	//config video2 scaler clk
	val = rSYS_DEVICE_CLK_CFG0;
	val &= ~(0xF << 24);
	val |= 4 << 24;
	rSYS_DEVICE_CLK_CFG0 = val;

	rSYS_CLK_DLY_REG &= ~(1 << 16);
	rSYS_CLK_DLY_REG |= (screen->vclk_active << 16);
	rLCD_CONTROL = (rLCD_CONTROL & (0xB << 6)) | (0<<29) | (0<<28) | (6<<23) | (3<<21) | (screen->rgb_mode<<18) | (1<<0);	
        	
	rLCD_TIMING0 = (screen->hsw<<20) | (screen->hbp<<10) | (screen->hfp<<0);	
        rLCD_TIMING1 = (screen->vfp<<19) | (screen->vsw<<13) | ((screen->width-1)<<0);
        rLCD_TIMING2 = (screen->hsync_active << 22) | (screen->vsync_active << 21) | 
        	           (screen->de_active << 23) | ((screen->height-1)<<10) | screen->vbp;

	rLCD_Y2R_COEF321 = 298<<0
              |91<<10
              |425<<20;
        rLCD_Y2R_COEF654 = 96<<0
             |184<<10
             |465<<20;
        rLCD_Y2R_COEF7 = (rLCD_Y2R_COEF7 & 0xffffcc00)
             |41<<0
             |1<<12
             |0<<13;

        rLCD_BACK_COLOR = (BLACK_Y<<16) | (BLACK_U<<8) | BLACK_V;		//set lcd back color

	ark_set_window_priority(4, 1, 2, 0, 3);
	ark_set_window_priority_tvenc(0, 4, 1, 2, 3);

        ark_display_initialize_common();
        rSYS_LVDS_CTRL_CFG = screen->lvds_cfg;
        
        //printf("lvds pad unset: rSYS_LVDS_CTRL_CFG=0x%08x,rLCD_TIMING0=0x%08x,rLCD_TIMING1=0x%08x,rLCD_TIMING2=0x%08x,rLCD_OSD2_VP_REG_1=0x%08x \r\n", 
                        //rSYS_LVDS_CTRL_CFG, rLCD_TIMING0,rLCD_TIMING1, rLCD_TIMING2,rLCD_OSD2_VP_REG_1);        

}

#if ARK_DISPLAY_ALL_MODE
void ark_display_initialize_itu601(struct screen_info *screen)
{
	int screen_id = screen->screen_id;
	unsigned int val;
	rSYS_PAD_CTRL00 = (1<<28) |(1<<24) |(1<<20) |(1<<16) |(1<<12) |(1<<8) | (1<<4) |(1<<0);
	//	 lcd_de,lcd_clk,lcd_vsync,lcd_hsync
	rSYS_PAD_CTRL03 = (1<<12) |(1<<8) | (1<<4) |(1<<0); 	

	rSYS_PER_CLK_EN |= 1 << 4;

	if(screen->clk_source == 0x02)
		set_dds_freq(screen->clk_freq);

	val = rSYS_LCD_CLK_CFG;
	val &= ~((0x1F<<19) | 0x7FF);

	val = (0<<31)
		| (0x36<<25)
		| (1<<23)
		| (screen->clk_div1 << 19)//(6<<19)	//srgb_clock div factor, which divided from lcd clock defined in bit7
		| (screen->clk_source << 7)//(1<<7)  //lcd clock select from system pll
		| (screen->clk_div2 << 4) //(2<<4)  //lcd clock div factor, which divided from srgb_clock, 
		| 6;   //scaler clock div factor, it derived from lcd clock select switch defined in bit 7 

	rSYS_LCD_CLK_CFG = val;

	//config video2 scaler clk
	val = rSYS_DEVICE_CLK_CFG0;
	val &= ~(0xF << 24);
	val |= 4 << 24;
	rSYS_DEVICE_CLK_CFG0 = val;

	rSYS_CLK_DLY_REG &= ~(1 << 16);
	rSYS_CLK_DLY_REG |= (screen->vclk_active << 16);
	rLCD_CONTROL = (rLCD_CONTROL & (0xB << 6)) | ((screen->vfp? 3 : 0)<<21) | (screen->rgb_mode<<18) | (1<<0);	
        	
	rLCD_TIMING0 = (screen->hsw<<20) | (screen->hbp<<10) | (screen->hfp<<0);	
    rLCD_TIMING1 = (screen->vfp<<19) | (screen->vsw<<13) | ((screen->width-1)<<0);
    rLCD_TIMING2 = (screen->hsync_active << 22) | (screen->vsync_active << 21) | 
		           (screen->de_active << 23) | ((screen->height-1)<<10) | screen->vbp;	

	rLCD_Y2R_COEF321 = 298<<0
              |91<<10
              |425<<20;
    rLCD_Y2R_COEF654 = 96<<0
             |184<<10
             |465<<20;
    rLCD_Y2R_COEF7 = (rLCD_Y2R_COEF7 & 0xffffcc00)
             |41<<0
             |1<<12
             |0<<13;

    rLCD_BACK_COLOR = (BLACK_Y<<16) | (BLACK_U<<8) | BLACK_V;		//set lcd back color

	ark_set_window_priority(4, 1, 2, 0, 3);
	ark_set_window_priority_tvenc(0, 4, 1, 2, 3);

    ark_display_initialize_common();
		
	rLCD_CONTROL &=~(0x7<<2);
	rLCD_CONTROL|=(0x3<<2);//Screen type:  srgb
	rLCD_CONTROL |=(0x1<<1); //Srgb_yuv_rgb: yuv

	rLCD_EXCTRL2&=~((0x3<<16)|(0x7<<18)|(0x7<<21));
	rLCD_EXCTRL2|=(0x2<<16)|(0x5<<18)|(0x5<<21);

	rSYS_ANALOG_REG1 &=~(7<<3);	
	
}


void ark_display_initialize_vga(struct screen_info *screen)
{
    unsigned int val;

	val = rSYS_LCD_CLK_CFG;
	val &= ~0x3FF;
	val |= (1 << 7) | 3;
    rSYS_LCD_CLK_CFG = val;

	//config video2 scaler clk
	val = rSYS_DEVICE_CLK_CFG0;
	val &= ~(0xF << 24);
	val |= 4 << 24;
	rSYS_DEVICE_CLK_CFG0 = val;

	rLCD_BACK_COLOR_TV = (BLACK_Y<<16) | (BLACK_U<<8) | BLACK_V;		//set tv back color

	ark_set_window_priority_tvenc(4, 1, 2, 0, 3);

	ark_display_initialize_common();

	initializa_tvenc_vga(screen);
}

void ark_display_initialize_cvbs(struct screen_info *screen)
{
    unsigned int val;

	val = rSYS_LCD_CLK_CFG;
	val &= ~0x3FF;
	val |= (2 << 7) | 3;
    rSYS_LCD_CLK_CFG = val;

	//config video2 scaler clk
	val = rSYS_DEVICE_CLK_CFG0;
	val &= ~(0xF << 24);
	val |= 2 << 24;
	rSYS_DEVICE_CLK_CFG0 = val;

	rLCD_BACK_COLOR_TV = (BLACK_Y<<16) | (BLACK_U<<8) | BLACK_V;		//set tv back color
	
	ark_set_window_priority_tvenc(4, 1, 2, 0, 3);

	ark_display_initialize_common();

	initializa_tvenc_cvbs(screen);
}

void ark_display_initialize_ypbpr(struct screen_info *screen)
{
    unsigned int val;

	val = rSYS_LCD_CLK_CFG;
	val &= ~0x3FF;
	val |= (1 << 7) | 3;
    rSYS_LCD_CLK_CFG = val;

	rLCD_BACK_COLOR_TV = (BLACK_Y<<16) | (BLACK_U<<8) | BLACK_V;		//set tv back color
    
    ark_set_window_priority_tvenc(4, 1, 2, 0, 3);

	ark_display_initialize_common();

	initializa_tvenc_ypbpr(screen);
}

void ark_display_initialize_itu656(struct screen_info *screen)
{
    unsigned int val;

	val = rSYS_LCD_CLK_CFG;
	val &= ~(0xF << 7);//val &= ~0x3FF;
	val |= (1 << 7);//val |= (1 << 7) | 3;
    rSYS_LCD_CLK_CFG = val;

	rLCD_BACK_COLOR_TV = (BLACK_Y<<16) | (BLACK_U<<8) | BLACK_V;		//set tv back color
    
    ark_set_window_priority_tvenc(4, 1, 2, 0, 3);

	ark_display_initialize_common();

	initializa_tvenc_itu656(screen);
}
#endif
void ark_display_initialize_port(struct screen_info *screen)
{
	enum screen_type_id screen_type =  screen->screen_type;
	
	switch(screen_type) {
	case SCREEN_TYPE_RGB:
		ark_display_initialize_rgbif(screen);
		break;
	case SCREEN_TYPE_LVDS:
		ark_display_initialize_lvdsif(screen);
		break;
#if ARK_DISPLAY_ALL_MODE                
	case SCREEN_TYPE_ITU601:
		ark_display_initialize_itu601(screen);
		break;
	case SCREEN_TYPE_VGA:
		ark_display_initialize_vga(screen);
		break;
	case SCREEN_TYPE_CVBS:
		ark_display_initialize_cvbs(screen);
		break;
	case SCREEN_TYPE_YPBPR:
		ark_display_initialize_ypbpr(screen);
		break;
	case SCREEN_TYPE_ITU656:
		ark_display_initialize_itu656(screen);
		break;
#endif
	default:
		break;
	}
}


