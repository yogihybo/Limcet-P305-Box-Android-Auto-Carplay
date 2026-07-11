/*
 *  linux/arch/arm/mach-ark1680/include/mach/hardware.h
 *
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
 *		hardware.h
 *
 * Description:
 *
 *
 * Author:
 *		Jack Tang
 *
 * Remarks:
 *
 */

#ifndef _ASM_ARCH_HARDWARE_H_
#define _ASM_ARCH_HARDWARE_H_


/* ============================================================================
 *  System Memory Base 
 * ============================================================================
 */

#define IROM_BASE			0x00000000
#define IRAM_BASE			0x00300000

#define DDRII_MEM_BASE			0x40000000

#define NAND_BASE			0xEC000000
#define SDHC0_BASE			0xEC400000
#define SDHC1_BASE			0xEC800000   
#define SDHC2_BASE			0xECC00000

//#define DDR_DATA_BASE			0x20000000
//#define DDR_BASE_ADDR			DDRII_BASE

#define DMA_BASE			0xE0000000
#define USB_BASE			0xE0100000
//#define JPEG_BASE			0xE0200000
#define GMAC_BASE			0xE0300000
#define USB1_BASE			0xE0400000
#define LCD_BASE			0xE0500000
#define SCALER_BASE			0xE0600000
#define PRE_SCALER_BASE			0xE0700000
#define ITU656_BASE			0xE0800000
#define MFC_BASE    	        	0xE0900000

#define DEINTERLACE_BASE		0xE0A00000

//#define DDRII_BASE			0xE0A00000
//#define VICH_BASE			0xE0B00000
//#define VICL_BASE			0xE0C00000

//#define M2MDMA_BASE			0xE0E00000	/* V2D */
//#define GPU_BASE			0xE0F00000	/* D3D */
#define I2S0_BASE			0xE4000000
#define ssp_BASE			0xE4100000
#define I2S1_BASE			0xE4200000
#define I2C_BASE			0xE4300000
#define CAN0_BASE			0xe4400000
#define ADC_BASE			0xe4500000
#define GPIO_BASE			0xe4600000
#define I2S2_BASE			0xE4800000
#define SYS_BASE			0xE4900000
#define CAN1_BASE			0xe4a00000
#define WDT_BASE			0xE4B00000
#define RTC_BASE			0xE4C00000
#define PWM_BASE			0xE4D00000
#define ECSPI_BASE			0xE4f00000

#define HSUART0_BASE			0xE8000000
#define HSUART1_BASE			0xE8100000
#define UART0_BASE			0xE8200000
#define UART1_BASE			0xE8300000


#define UART2_BASE			0xE8400000
#define UART3_BASE			0xE8500000

#define TIMER_BASE			0xE8600000

#define GPU_BASE			0xE9000000	

#define DDRII_BASE			0xE9100000
//#define DDR_DATA_BASE			0x20000000
#define DDR_BASE_ADDR			DDRII_BASE

#define DPU_ADDR			0xE9200000





#endif /* _ASM_ARCH_HARDWARE_H_ */

