#include <asm-generic/gpio.h>
#include <asm/arch/ark-common.h>
#include "ark1668e_lcd.h"

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
        rLCD_VIDEO1_POSITION =
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
	rLCD_VIDEO1_CTL = (1<<22) /*0x3c*/
					| (y_uv_order<<21)  // 0
					| (1<<17)
					| (1<<9) 
					| (1<<8) 
					| (1<<5) 
					| (rgb_ycbcr_bypass<<4) // 0
					| (format<<0);	// 2 

	if(format == SEQ_YUV420)
		rLCD_VIDEO1_CTL |= (1<<11);
/*swidth=3120 c30, sheight=4208 1070*/
	rLCD_VIDEO1_WIN_POINT 	= 0; 								/*0x15c*/
	rLCD_VIDEO1_WIN_SIZE 	= (height<<12) | (width<<0); 		/*0x40*/ 
	rLCD_VIDEO1_POSITION 	= 0;								/*0x4c*/
	rLCD_VIDEO1_SIZE 		= (height<<12) | (width<<0);		/*0x44*/
	rLCD_VIDEO1_SOURCE_SIZE 	= (height << 12) | (width);			/*0x16c*/
	rLCD_VIDEO1_ADDR1 		= (unsigned int )buf;   			/*0x054*/
	rLCD_VIDEO1_ADDR2 		= (unsigned int )buf + width*height; /*0x058*//*c3bc280*/

	if(format == SEQ_YUV420)
	{
		rLCD_VIDEO1_ADDR3 = rLCD_VIDEO1_ADDR2 + width*height/4;
	}
	else 
	{
		rLCD_VIDEO1_ADDR3 = rLCD_VIDEO1_ADDR2 + width*height/2;  /*0x5c*/
	}
	if((format>= 4)&&(format <= 12))
	{
		rLCD_VIDEO1_COLOUR_MATRIX_REG0 =  0 ;//k0
		rLCD_VIDEO1_COLOUR_MATRIX_REG1 =  0 ;//k1
		rLCD_VIDEO1_COLOUR_MATRIX_REG2 =  0 ;//k2    
		rLCD_VIDEO1_COLOUR_MATRIX_REG3 =  0 ;//rgb offset  
		rLCD_VIDEO1_COLOUR_MATRIX_REG4 =  0 ;//k1
		rLCD_VIDEO1_COLOUR_MATRIX_REG5 =  0 ;//k2 	    
	}
	else
	{
	    rLCD_VIDEO1_COLOUR_MATRIX_REG0 =  0x01000100;//k0
	    rLCD_VIDEO1_COLOUR_MATRIX_REG1 =  0x100167;//k1
	    rLCD_VIDEO1_COLOUR_MATRIX_REG2 =  0xf4afa8 ;//k2    
	    rLCD_VIDEO1_COLOUR_MATRIX_REG3 =  0x12d100;//k3 offset  
	    rLCD_VIDEO1_COLOUR_MATRIX_REG4 =  0xf4d000 ;//k4--offset    
	    rLCD_VIDEO1_COLOUR_MATRIX_REG5 =  0xf69086;//rgb offset  	    
//	rLCD_VIDEO_YUV2RGB0 |=(1<<24);
}
   	rLCD_CONTROL |= (1<<5);
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
		if((format>= 4)&&(format <= 12))
		{
		    rLCD_OSD1_COLOUR_MATRIX_REG0 =  0 ;//k0
		    rLCD_OSD1_COLOUR_MATRIX_REG1 =  0 ;//k1
		    rLCD_OSD1_COLOUR_MATRIX_REG2 =  0 ;//k2    
		    rLCD_OSD1_COLOUR_MATRIX_REG3 = 0 ;//rgb offset  
		    rLCD_OSD1_COLOUR_MATRIX_REG4 =  0 ;//k1
		    rLCD_OSD1_COLOUR_MATRIX_REG5 =  0 ;//k2 		    
		}
		else
		{
			rLCD_OSD1_COLOUR_MATRIX_REG0 =  0x01000100;//k0
		    rLCD_OSD1_COLOUR_MATRIX_REG1 =  0x100167;//k1
		    rLCD_OSD1_COLOUR_MATRIX_REG2 =  0xf4afa8 ;//k2    
		    rLCD_OSD1_COLOUR_MATRIX_REG3 =  0x12d100;//k3 offset  
		    rLCD_OSD1_COLOUR_MATRIX_REG4 =  0xf4d000 ;//k4--offset    
		    rLCD_OSD1_COLOUR_MATRIX_REG5 =  0xf69086;//rgb offset  			
		}
		break;
	case OSD2_LAYER:
		rLCD_OSD2_SIZE = (height<<12) | width;
		rLCD_OSD2_SOURCE_SIZE = (height << 12) | (width);
		rLCD_OSD2_WIN_POINT = (0 << 12) | (0);
		rLCD_OSD2_CTL &= ~(0x7FF<<12);
		rLCD_OSD2_CTL |= (yuv_order << 21) | (rgb_order << 18) |
			(1 << 17) | (rgb_ycbcr_bypass << 16) | (format << 12);
		if((format>= 4)&&(format <= 12))
		{
			rLCD_OSD2_COLOUR_MATRIX_REG0 =  0 ;//k0
			rLCD_OSD2_COLOUR_MATRIX_REG1 =  0 ;//k1
			rLCD_OSD2_COLOUR_MATRIX_REG2 =  0 ;//k2    
			rLCD_OSD2_COLOUR_MATRIX_REG3 = 0 ;//rgb offset  
			rLCD_OSD2_COLOUR_MATRIX_REG4 =  0 ;//k1
			rLCD_OSD2_COLOUR_MATRIX_REG5 =  0 ;//k2 		    
		}
		else
		{
			rLCD_OSD2_COLOUR_MATRIX_REG0 =  0x01000100;//k0
			rLCD_OSD2_COLOUR_MATRIX_REG1 =  0x100167;//k1
			rLCD_OSD2_COLOUR_MATRIX_REG2 =  0xf4afa8 ;//k2    
			rLCD_OSD2_COLOUR_MATRIX_REG3 =  0x12d100;//k3 offset  
			rLCD_OSD2_COLOUR_MATRIX_REG4 =  0xf4d000 ;//k4--offset    
			rLCD_OSD2_COLOUR_MATRIX_REG5 =  0xf69086;//rgb offset  			
		}		
		break;
	case OSD3_LAYER:
		rLCD_OSD3_SIZE = (height<<12) | width;
		rLCD_OSD3_SOURCE_SIZE = (height << 12) | (width);
		rLCD_OSD3_WIN_POINT = (0 << 12) | (0);
		rLCD_OSD3_CTL &= ~(0x7FF<<12);
		rLCD_OSD3_CTL |= (yuv_order << 21) | (rgb_order << 18) |
			(1 << 17) | (rgb_ycbcr_bypass << 16) | (format << 12);
		if((format>= 4)&&(format <= 12))
		{
			rLCD_OSD3_COLOUR_MATRIX_REG0 =  0 ;//k0
			rLCD_OSD3_COLOUR_MATRIX_REG1 =  0 ;//k1
			rLCD_OSD3_COLOUR_MATRIX_REG2 =  0 ;//k2    
			rLCD_OSD3_COLOUR_MATRIX_REG3 =  0 ;//rgb offset  
			rLCD_OSD3_COLOUR_MATRIX_REG4 =  0 ;//k1
			rLCD_OSD3_COLOUR_MATRIX_REG5 =  0 ;//k2 		    
		}
		else
		{
			rLCD_OSD3_COLOUR_MATRIX_REG0 =  0x01000100;//k0
			rLCD_OSD3_COLOUR_MATRIX_REG1 =  0x100167;//k1
			rLCD_OSD3_COLOUR_MATRIX_REG2 =  0xf4afa8 ;//k2    
			rLCD_OSD3_COLOUR_MATRIX_REG3 =  0x12d100;//k3 offset  
			rLCD_OSD3_COLOUR_MATRIX_REG4 =  0xf4d000 ;//k4--offset    
			rLCD_OSD3_COLOUR_MATRIX_REG5 =  0xf69086;//rgb offset  		
		}		
		break;
	default:
		printf("error osd layer_id %d.\n", layer_id);
		break;
	}
}

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

