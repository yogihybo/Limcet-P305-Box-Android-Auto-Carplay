/*
 * Copyright(c) 2012 Hong Kong Applied Science and Technology
 * Research Institute Company Limited (ASTRI)
 *
 * Description:
 * 
 * Name:
 * 		config.c
 *
 * Author:
 * 		Jack Tang
 *
 * Remarks:
 * 		
 * 
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/ark/ark_dw_dmac.h>
#include <linux/ark/ark_i2s.h>
#include <linux/ark/sddac.h>
#include <linux/ark/ark_remote.h>
#include <linux/ark/ark_uart.h>
#include <linux/ark/ark_i2c.h>
#include <linux/ark/ark_display_io.h>
#include <linux/ark/ark_deinterlace_io.h>
#include <linux/ark/ark_itu656_io.h>
#include <linux/ark/ark_prescaler_io.h>
#include <linux/ark/ark_nand.h>
#include <linux/mmc/dw_mmc.h>
#include <linux/mmc/host.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/nand.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <mach/hardware.h>
#include <mach/irqs.h>
#include <mach/ark_dma.h>
#include <mach/ark_sys_io.h>

#if defined(CONFIG_USB_MUSB_HDRC) || defined(CONFIG_USB_MUSB_HDRC_MODULE)
#include <linux/usb/musb.h>
#include <linux/dma-mapping.h>
#endif
#include <linux/i2c-gpio.h>
#include <mach/gpio.h>
#include <linux/pwm_backlight.h>
#include <linux/delay.h>

/* AHB DMA controller */
#if defined(CONFIG_ARK_AHB_DW_DMAC) || defined(CONFIG_ARK_AHB_DW_DMAC_MODULE)
static struct dw_dma_platform_data dmac_ahb_data = {
	.nr_channels 		= 4,
	.block_size			= 4095U,
	.nr_masters			= 2,
	.data_width			= { 2, 2, 0, 0 },
	.dma_cap			= DMA_CAP_SLAVE,
	.clk_name			= "dmac_ahb",
};

static struct resource dmac_ahb_resource[] = {
	[0] = {
			.start 	= DMA_BASE,
			.end	= DMA_BASE + 0x3FF,
			.flags	= IORESOURCE_MEM,
	},
	[1] = {
			.start 	= DMA_INT,
			.end 	= DMA_INT,
			.flags 	= IORESOURCE_IRQ,
	},
};

static struct platform_device ark1680_dmac_ahb_device = {
	.name 			= "ark_dw_dmac",
	.id 			= 0,
	.resource 		= dmac_ahb_resource,
	.num_resources 	= ARRAY_SIZE(dmac_ahb_resource),
	.dev			= {
		.platform_data 	= &dmac_ahb_data,
	},
};
#endif

#if defined(CONFIG_ARK_AXI_DW_DMAC) || defined(CONFIG_ARK_AXI_DW_DMAC_MODULE)
static struct dw_dma_platform_data dmac_axi_data = {
	.nr_channels 		= 1,
	.block_size			= 4095U,
	.nr_masters			= 2,
	.data_width			= { 3, 3, 0, 0 },
	.dma_cap			= DMA_CAP_MEMSET,
	.clk_name			= "dmac_axi",
};

static struct resource dmac_axi_resource[] = {
	[0] = {
			.start 	= M2MDMA_BASE,
			.end	= M2MDMA_BASE + 0x3FF,
			.flags	= IORESOURCE_MEM,
	},
	[1] = {
			.start 	= M2MDMA_INT,
			.end 	= M2MDMA_INT,
			.flags 	= IORESOURCE_IRQ,
	},
};

static struct platform_device ark1680_dmac_axi_device = {
	.name 			= "ark_dw_dmac",
	.id 			= 0,
	.resource 		= dmac_axi_resource,
	.num_resources 	= ARRAY_SIZE(dmac_axi_resource),
	.dev			= {
		.platform_data 	= &dmac_axi_data,
	},
};
#endif

void __init ark1680_add_device_dmac(void) 
{
	/* TODO: Configure: clock, reset... */

#if defined(CONFIG_ARK_AHB_DW_DMAC) || defined(CONFIG_ARK_AHB_DW_DMAC_MODULE)
	platform_device_register(&ark1680_dmac_ahb_device);
#endif

#if defined(CONFIG_ARK_AXI_DW_DMAC) || defined(CONFIG_ARK_AXI_DW_DMAC_MODULE)
	platform_device_register(&ark1680_dmac_axi_device);
#endif
}


/* UART controller */
#if defined(CONFIG_ARK1680_UART) || defined(CONFIG_ARK1680_UART_MODULE)
static struct ark1680_uart_vendor_data vendor_ark1680 = {
	.ifls                   = UART011_IFLS_RX4_8|UART011_IFLS_TX4_8,
	.fifosize               = 16,
	.lcrh_tx                = UART011_LCRH,
	.lcrh_rx                = UART011_LCRH,
	/* TODO: change to getting from clk struct once the clock has been tested*/
	.clk					= 24000000, 
	.oversampling           = false,
	.interrupt_may_hang		= false,
	.dma_threshold          = false,
};

static struct ark1680_uart_platdata uartx_data = {
	.vendor			= &vendor_ark1680,
	.data			= NULL,
};

static struct resource uartx_resources[] = {
	[0] = {
			.start  = UART0_BASE,
			.end    = UART0_BASE + 0xff,
			.flags  = IORESOURCE_MEM,
	},
	[1] = {
			.start  = UART0_INT,
			.end    = UART0_INT,
			.flags  = IORESOURCE_IRQ,
	},
//uart1
	[2] = {
			.start  = UART1_BASE,
			.end    = UART1_BASE + 0xff,
			.flags  = IORESOURCE_MEM,
	},
	[3] = {
			.start  = UART1_INT,
			.end    = UART1_INT,
			.flags  = IORESOURCE_IRQ,
	},
//uart2
	[4] = {
			.start  = UART2_BASE,
			.end    = UART2_BASE + 0xff,
			.flags  = IORESOURCE_MEM,
	},
	[5] = {
			.start  = UART2_INT,
			.end    = UART2_INT,
			.flags  = IORESOURCE_IRQ,
	},
//uart3
	[6] = {
			.start  = UART3_BASE,
			.end    = UART3_BASE + 0xff,
			.flags  = IORESOURCE_MEM,
	},
	[7] = {
			.start  = UART3_INT,
			.end    = UART3_INT,
			.flags  = IORESOURCE_IRQ,
	},
//uart4

	[8] = {
			.start  = UART4_BASE,
			.end    = UART4_BASE + 0xff,
			.flags  = IORESOURCE_MEM,
	},
	[9] = {
			.start  = UART4_INT,
			.end    = UART4_INT,
			.flags  = IORESOURCE_IRQ,
	},

//uart5
	[10] = {
		.start  = UART5_BASE,
		.end    = UART5_BASE + 0xff,
		.flags  = IORESOURCE_MEM,
	},
	[11] = {
		.start  = UART5_INT,
		.end    = UART5_INT,
		.flags  = IORESOURCE_IRQ,
	},

};

static struct platform_device ark1680_uartx_device = {
	.name                   = "ark1680-uart",
	.id                     = -1,
	.resource               = uartx_resources,
	.num_resources  		= ARRAY_SIZE(uartx_resources),
	.dev                    = {
		.platform_data  = &uartx_data,
	},
};

#endif /* defined(CONFIG_ARK1680_UART) || defined(CONFIG_ARK1680_UART_MODULE) */

void __init ark1680_add_device_uart(void)
{
#if defined(CONFIG_ARK1680_UART) || defined(CONFIG_ARK1680_UART_MODULE)
        /* TODO: PAD configuration */
		//config uart2,uart3 pad mux
	
	ark_sys_pad_config(SYS_PAD_CTRL08, 0xF, 4, 0x5);
	ark_sys_pad_config(SYS_PAD_CTRL08, 0xF, 8, 0x5);
	ark_sys_pad_config(SYS_PAD_CTRL08, 0xF, 12, 0x5);

        platform_device_register(&ark1680_uartx_device);
		//
//        platform_device_register(&ark1680_uart1_device);	
        //platform_device_register(&ark1680_uart2_device);
//        platform_device_register(&ark1680_uart3_device);	
 //       platform_device_register(&ark1680_uart4_device);
  //      platform_device_register(&ark1680_uart5_device);
#endif
}


/* ====================== SD/MMC Controller ===================== */
static int ark1680_dwmci_get_bus_wd(u32 slot_id)
{
        return 4;
}

static int ark1680_dwmci_init(u32 slot_id, irq_handler_t handler, void *data)
{
        return 0;
}

static struct resource ark1680_dwmci_resource[] = {
        [0] = {
                        .start  = SDHC0_BASE,
                        .end    = SDHC0_BASE + 0xff,
                        .flags  = IORESOURCE_MEM,
        },
        [1] = {
                        .start  = SDHC0_INT,
                        .end    = SDHC0_INT,
                        .flags  = IORESOURCE_IRQ,
        },
};

static struct dw_mci_board ark1680_dwmci_pdata = {
	.num_slots          = 1,
	.quirks             = DW_MCI_QUIRK_HIGHSPEED,
	.bus_hz				= 25 * 1000 * 1000,
	.fifo_depth			= 32,
	.detect_delay_ms	= 200,
	.init				= ark1680_dwmci_init,
	.get_bus_wd			= ark1680_dwmci_get_bus_wd,
	.tx_dws			    = {
		.dma_dev	= &ark1680_dmac_ahb_device.dev,
		.cfg_hi		= DWC_CFGH_DST_PER(DMA_SDMMC) | DWC_CFGH_FIFO_MODE,
		.cfg_lo		= 0,
		.src_master = 0,
		.dst_master = 1,
	},	
	.rx_dws			    = {
		.dma_dev	= &ark1680_dmac_ahb_device.dev,
		.cfg_hi		= DWC_CFGH_SRC_PER(DMA_SDMMC) | DWC_CFGH_FIFO_MODE,
		.cfg_lo		= 0,
		.src_master = 0,
		.dst_master = 1,
	},		
};

static struct platform_device ark1680_sdmmc0_device = {
	.name		= "dw_mmc",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(ark1680_dwmci_resource),
	.resource	= ark1680_dwmci_resource,
	.dev		= {
		.platform_data		= &ark1680_dwmci_pdata,
	},
};

/* ============================================================== */
static struct resource ark1680_dwmci1_resource[] = {
        [0] = {
                        .start  = SDHC1_BASE,
                        .end    = SDHC1_BASE + 0xff,
                        .flags  = IORESOURCE_MEM,
        },
        [1] = {
                        .start  = SDHC1_INT,
                        .end    = SDHC1_INT,
                        .flags  = IORESOURCE_IRQ,
        },
};

static struct dw_mci_board ark1680_dwmci1_pdata = {
	.num_slots          = 1,
	.quirks             = DW_MCI_QUIRK_HIGHSPEED,
	.bus_hz				= 25 * 1000 * 1000,
//	.bus_hz				= 400 * 1000,
	.fifo_depth			= 32,
	.detect_delay_ms	= 200,
	.init				= ark1680_dwmci_init,
	.get_bus_wd			= ark1680_dwmci_get_bus_wd,
};

static struct platform_device ark1680_sdmmc1_device = {
	.name		= "dw_mmc1",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(ark1680_dwmci1_resource),
	.resource	= ark1680_dwmci1_resource,
	.dev		= {
		.platform_data		= &ark1680_dwmci1_pdata,
	},
};
/* ============================================================== */

void __init ark1680_add_device_mci(void)
{
#if defined(CONFIG_MMC_DW) 
	platform_device_register(&ark1680_sdmmc0_device);
	ark_sys_pad_config(SYS_PAD_CTRL06, 0xFFFFFFF, 0, 0x2222222);
//	ark_sys_pad_config(SYS_PAD_CTRL06, 0xFFFFFFF, 0, 0x02552222);
	platform_device_register(&ark1680_sdmmc1_device);
#endif
}

/* ==================== NAND flash partition ==================== */
#define NAND_BLOCK_SIZE         0x20000
static struct mtd_partition ark1680_nand_partitions[] = {
	/* All the partition sizes are listed in terms of NAND block size */
	{
		.name           = "S-Loader",                   
		.offset         = 0,                                        /*0x0000---0x20000*/
		.size           = 1 * NAND_BLOCK_SIZE,       /*128K*/
		.mask_flags     = MTD_POWERUP_LOCK,         /* force read-only */
	},
	{
		.name           = "U-Boot",
		.offset         = 0x20000,                             /*0x20000---0xa0000*/
		.size           = 4 * NAND_BLOCK_SIZE,      /*512K*/
		.mask_flags     = MTD_POWERUP_LOCK,        /* force read-only */
	},
	{
		.name           = "U-Boot Env",
		.offset         = 0xA0000,                              /*0xa0000---0xe0000*/
		.size           = 2 * NAND_BLOCK_SIZE,       /*256K*/
	},	
	{
		.name			= "bootlogo",
		.offset 		= 0xE0000,							   /*0x20000---0xa0000*/
		.size			= 4 * NAND_BLOCK_SIZE, 	 /*512K*/
		.mask_flags 	= MTD_WRITEABLE, 	   /* force  writeable */
	},
	{
		.name           = "Kernel",
		.offset         = 0x160000,   				   /*0xe0000---0x6e0000*/											
		.size           = 32 * NAND_BLOCK_SIZE,	   /*4M*/
	},
	/*reseved*/
	{
		.name           = "File System",
		.offset         = 0x7e0000,                              /*0x7e0000---0x8000000*/
		.size           = MTDPART_SIZ_FULL,               /*120M*/
	},
};

/* ============================================================== */

/* ====================== NAND controller ======================= */

static struct resource ark1680_nand_resource[] = {
        [0] = {
                .start = NAND_BASE,
                .end   = NAND_BASE + 0x2ff,
                .flags = IORESOURCE_MEM,
        },
        [1] = {
                .start = NAND_INT,
                .end   = NAND_INT,
                .flags = IORESOURCE_IRQ,
        }
};

static struct ark_nand_platdata ark1680_nand_pdata = {
	.parts = ark1680_nand_partitions,
	.nr_parts = ARRAY_SIZE(ark1680_nand_partitions),
};

static struct platform_device ark1680_nand_device = {
        .name           = "ark1680-nand",
        .id             = -1,
        .num_resources  = ARRAY_SIZE(ark1680_nand_resource),
        .resource       = ark1680_nand_resource,
		.dev            = {
		.platform_data          = &ark1680_nand_pdata,
	},
};

void __init ark1680_add_device_nand(void)
{
#if defined(CONFIG_MTD_NAND_ARK)
	platform_device_register(&ark1680_nand_device);
#endif
}

static struct resource ark1680_gpio_resource[] = {
        [0] = {
                .start = GPIO_BASE,
                .end   = GPIO_BASE + 0x9C,
                .flags = IORESOURCE_MEM,
        },
        [1] = {
                .start = GPIO_INT,
                .end   = GPIO_INT,
                .flags = IORESOURCE_IRQ,
        }
};


static struct platform_device ark_gpio = {
        .name           = "gpio-ark",
        .id             = -1,
        .num_resources  = ARRAY_SIZE(ark1680_gpio_resource),
        .resource       = ark1680_gpio_resource,
};

void __init ark1680_add_device_gpio(void)
{
	platform_device_register(&ark_gpio);
}

static struct resource ark1680_rtc_resource[] = {
        [0] = {
                .start = RTC_BASE,
                .end   = RTC_BASE + 0xff,
                .flags = IORESOURCE_MEM
        },
        [1] = {
                .start = RTC_PERIOD_INT,
                .end   = RTC_PERIOD_INT,
                .flags = IORESOURCE_IRQ
        },
	[2] = {
                .start = RTC_ALM_INT,
                .end   = RTC_ALM_INT,
                .flags = IORESOURCE_IRQ
        }
};


static struct platform_device ark1680_rtc_device = {
        .name           = "ark1680-rtc1",
        .id             = -1,
        .num_resources  = ARRAY_SIZE(ark1680_rtc_resource),
        .resource       = ark1680_rtc_resource,
};

void __init ark1680_add_device_rtc(void)
{
#if defined(CONFIG_RTC_ARK)
	platform_device_register(&ark1680_rtc_device);
#endif
}

/* ============================================================== */
/*Analog GPIO I2c controller*/
static struct i2c_gpio_platform_data i2c_analog_data = 
{
	.sda_pin = GPIO3,//GPIO118,//656IN2_D7
	.scl_pin = GPIO2,//GPIO117,//656IN2_D6
	.udelay  = 5,
	.timeout = HZ/10,
	.sda_is_open_drain = 0,
	.scl_is_open_drain = 0,
	.scl_is_output_only = 1,
};

static struct platform_device i2c_analog_device = 
{
	.name 			= "i2c-gpio",
	.id				= 1,
	.dev			= {
		.platform_data 	= &i2c_analog_data,
	},
};

static struct i2c_board_info __initdata i2c_analog_devices[] = {

		{	
#if defined(CONFIG_TOUCHSCREEN_HJ_GT9XX) && ! defined(CONFIG_I2C_ARK)			
			I2C_BOARD_INFO("Goodix-TS", (0x5d)),
#endif			
			I2C_BOARD_INFO("FM1288", (0xc0 >> 1)), // 0x60
		},		
		{
			I2C_BOARD_INFO("dvr_ark7116", (0xb0 >> 1)),
		},

};

void __init analog_i2c_add_device_i2c(struct i2c_board_info *devices, int nr_devices) 
{
	/* register i2c board info */
	//ark_sys_pad_config(SYS_PAD_CTRL0B, 0x03, 26, 0x00);
	
	i2c_register_board_info(1, devices, nr_devices);//i2c-1 as the analog i2c 

	platform_device_register(&i2c_analog_device);
}


/* I2C controller */
#if defined(CONFIG_I2C_ARK) || defined(CONFIG_I2C_ARK_MODULE)
static struct resource i2c0_resources[] = {
	[0] = {
			.start 	= IIC_BASE,
			.end 	= IIC_BASE + 0xff,
			.flags 	= IORESOURCE_MEM,
	},
	[1] = {
			.start 	= I2C_INT,
			.end	= I2C_INT,
			.flags	= IORESOURCE_IRQ,
	},
};

static struct ark_i2c_platdata ark_i2c_pdata = {
	.speed_mode		= I2C_STANDARD,
	.hs_master_code	= 0,
};

static struct platform_device ark1680_i2c0_device = {
	.name 			= "ark_i2c",
	.id				= 0,
	.resource 		= i2c0_resources,
	.num_resources 	= ARRAY_SIZE(i2c0_resources),
	.dev			= {
		.platform_data 	= &ark_i2c_pdata,
	},
};

struct i2c_gpio_platform_data ark_i2c_gpio_pdata = {
	.sda_pin		= 117,
	.scl_pin		= 115,
	.udelay 		= 2,
	.timeout		= 200,
	.sda_is_open_drain = 0,
	.scl_is_open_drain = 0,
	.scl_is_output_only = 1,
};

static struct platform_device ark1680_i2c_gpio_device = {
	.name 			= "i2c-gpio",
	.id				= 1,
	.dev			= {
		.platform_data 	= &ark_i2c_gpio_pdata,
	},
};

#endif

static struct i2c_board_info __initdata ark1680_i2c_devices[] = {
#if defined(CONFIG_SND_SOC_CS4270) || defined(CONFIG_SND_SOC_CS4270_MODULE)
	{
		I2C_BOARD_INFO("cs4270", (0x9E >> 1)),
	},
#endif
#if defined(CONFIG_VCODEC_7116_ARK)
        {
                I2C_BOARD_INFO("ark7116", (0xb0 >> 1)),
        },
#endif
	{
                I2C_BOARD_INFO("FM1288", (0xc0 >> 1)),
        }
};

void __init ark1680_add_device_i2c(struct i2c_board_info *devices, int nr_devices) 
{
#if defined(CONFIG_I2C_ARK) || defined(CONFIG_I2C_ARK_MODULE)
	/* PAD configuration - scl & sda */
	ark_sys_pad_config(SYS_PAD_CTRL08, 0xF, 16, 0xA);

	/* register i2c board info */
	i2c_register_board_info(0, devices, nr_devices);

	platform_device_register(&ark1680_i2c0_device);
	//platform_device_register(&ark1680_i2c_gpio_device);
#endif
}


/* I2S controller */
#if defined(CONFIG_SND_ARK_SOC_I2S) || defined(CONFIG_SND_ARK_SOC_I2S_MODULE)
static struct resource i2s_resources[] = {
	[0] = {
			.start	= I2S_BASE,
			.end 	= I2S_BASE + 0xFF,
			.flags	= IORESOURCE_MEM,
	},

	[2] = {
			.start 	= I2S_INT,
			.end		= I2S_INT,
			.flags 	= IORESOURCE_IRQ,
	},
};

static struct ark_i2s_platform_data i2s_pdata = {
	.dma_base		= DMA_I2S_BASE,
	.tx_dws			= {
			.dma_dev	= &ark1680_dmac_ahb_device.dev,
			.cfg_hi		= DWC_CFGH_DST_PER(DMA_I2S_TX) | DWC_CFGH_FIFO_MODE,
			.cfg_lo		= 0,
			.src_master = 0,
			.dst_master = 1,
	},
	.rx_dws			= {
			.dma_dev	= &ark1680_dmac_ahb_device.dev,
			.cfg_hi		= DWC_CFGH_SRC_PER(DMA_I2S_RX) | DWC_CFGH_FIFO_MODE,
			.cfg_lo		= 0,
			.src_master = 0,
			.dst_master = 1,
	},
};

static struct platform_device ark1680_i2s_device = {
	.name 			= "ark_i2s_dev",
	.id				= 0,
	.resource 		= i2s_resources,
	.num_resources 	= ARRAY_SIZE(i2s_resources),
	.dev			= {
		.platform_data 	= &i2s_pdata,	
		.dma_mask			= DMA_BIT_MASK(32),
		.coherent_dma_mask  = DMA_BIT_MASK(32),
	},
};

static struct resource i2s2_resources[] = {
	[0] = {
			.start	= I2S2_BASE,
			.end 	= I2S2_BASE + 0xFF,
			.flags	= IORESOURCE_MEM,
	},

	[2] = {
			.start 	= I2S2_INT,
			.end		= I2S2_INT,
			.flags 	= IORESOURCE_IRQ,
	},
};

static struct ark_i2s_platform_data i2s2_pdata = {
	.dma_base		= DMA_I2S2_BASE,
	.tx_dws			= {
			.dma_dev	= &ark1680_dmac_ahb_device.dev,
			.cfg_hi		= DWC_CFGH_DST_PER(DMA_I2S2_TX) | DWC_CFGH_FIFO_MODE,
			.cfg_lo		= 0,
			.src_master = 0,
			.dst_master = 1,
	},
	.rx_dws			= {
			.dma_dev	= &ark1680_dmac_ahb_device.dev,
			.cfg_hi		= DWC_CFGH_SRC_PER(DMA_I2S2_RX) | DWC_CFGH_FIFO_MODE,
			.cfg_lo		= 0,
			.src_master = 0,
			.dst_master = 1,
	},
};

static struct platform_device ark1680_i2s2_device = {
	.name 			= "ark_i2s_dev",
	.id				= 1,
	.resource 		= i2s2_resources,
	.num_resources 	= ARRAY_SIZE(i2s2_resources),
	.dev			= {
		.platform_data 	= &i2s2_pdata,	
		.dma_mask			= DMA_BIT_MASK(32),
		.coherent_dma_mask  = DMA_BIT_MASK(32),
	},
};
#endif

#if defined(CONFIG_SND_ARK_SOC_DMA) || defined(CONFIG_SND_ARK_SOC_DMA_MODULE)
static struct platform_device ark1680_pcm_device = {
	.name			= "ark-audio",
	.id				= -1,
};
#endif

#if defined(CONFIG_SND_ARK_SOC_ARK_SDDAC) || defined(CONFIG_SND_ARK_SOC_ARK_SDDAC_MODULE)
static struct sddac_platform_data sddac_pdata = {
	.reg_base		= I2S_BASE,
	.reg_size		= 0xFF,
};


static struct platform_device ark1680_sddac_audio = {
	.name			= "ark_sddac",
	.id				= -1,
	.dev			= {
		.platform_data = &sddac_pdata,
	},
};

static struct platform_device ark1680_sddac_dev = {
	.name			= "ark_sddac_dev",
	.id				= -1,
};

static struct platform_device ark1680_sddac_dai_dev = {
	.name			= "ark_sddac_dai",
	.id				= -1,
};

static struct platform_device ark1680_cs4334_dev = {
	.name			= "ark_cs4334_dev",
	.id				= -1,
};

#endif

#if defined(CONFIG_SND_ARK_SOC_CS4270) || defined(CONFIG_SND_ARK_SOC_CS4270_MODULE)

static struct platform_device ark1680_cs4270_audio = {
	.name			= "ark_cs4270",
	.id				= -1,
};
#endif


void __init ark1680_add_device_audio(void) 
{

#if defined(CONFIG_SND_ARK_SOC_I2S) || defined(CONFIG_SND_ARK_SOC_I2S_MODULE)
	/* I2S PAD for DATA OUT */
	ark_sys_pad_config(SYS_PAD_CTRL0A, 0x7, 8, 0x0);
	//ark_sys_pad_config(SYS_PAD_CTRL06, 0xFFFF, 8, 0x4444);
	ark_sys_pad_config(SYS_PAD_CTRL06, 0x1, 31, 0x1);
	ark_sys_pad_config(SYS_PAD_CTRL09, 0x3, 20, 0x1);
	platform_device_register(&ark1680_i2s_device);
	platform_device_register(&ark1680_i2s2_device);
#endif

#if defined(CONFIG_SND_ARK_SOC_DMA) || defined(CONFIG_SND_ARK_SOC_DMA_MODULE)
	platform_device_register(&ark1680_pcm_device);
#endif

#if defined(CONFIG_SND_ARK_SOC_ARK_SDDAC) || defined(CONFIG_SND_ARK_SOC_ARK_SDDAC_MODULE)
	platform_device_register(&ark1680_sddac_audio);
	platform_device_register(&ark1680_sddac_dev);
	platform_device_register(&ark1680_sddac_dai_dev);
	platform_device_register(&ark1680_cs4334_dev);
#endif

#if defined(CONFIG_SND_ARK_SOC_CS4270) || defined(CONFIG_SND_ARK_SOC_CS4270_MODULE)
     platform_device_register(&ark1680_cs4270_audio);
#endif

}

#if defined(CONFIG_ARK_REMOTE) || defined(CONFIG_ARK_REMOTE_MODULE)
static struct resource remote_resources[] = {
	[0] = {
			.start 	= RC_BASE,
			.end	= RC_BASE + 0xFF,
			.flags	= IORESOURCE_MEM,
	},
	[1] = {
			.start 	= RCRT_INT,
			.end	= RCRT_INT,
			.flags	= IORESOURCE_IRQ,
	},
};

static struct ark_remote_pdata remote_pdata = {
	.ir_mode 		= ARK_REMOTE_NEC,
};

static struct platform_device ark1680_remote_device = {
	.name			= "ark_remote",
	.id				= -1,
	.resource		= remote_resources,
	.num_resources	= ARRAY_SIZE(remote_resources),
	.dev			= {
		.platform_data = &remote_pdata,
	},
};

void __init ark1680_add_device_remote(void)
{
	platform_device_register(&ark1680_remote_device);
}
#endif

/* ================== Display Controller ============================ */
#if defined(CONFIG_ARK_DISP) || defined(CONFIG_ARK_DISP_MODULE)
static struct resource ark_disp_resource [] = {
        [0] = {
                .start = LCD_BASE,
                .end   = (LCD_BASE + 0x800),
                .flags = IORESOURCE_MEM,
        },
        [1] = {
                .start = SYS_BASE,
                .end   = (SYS_BASE + 0x200),
                .flags = IORESOURCE_MEM,
        },
        [2] = {
                .start = GPIO_BASE,
                .end   = (GPIO_BASE + 0xF0),
                .flags = IORESOURCE_MEM,
        },
        [3] = {
                .start = SCAL_LD_INT,
                .end   = SCAL_LD_INT,
                .flags = IORESOURCE_IRQ,
        },
        [4] = {
                .start = POST_SCALER_INT,
                .end   = POST_SCALER_INT,
                .flags = IORESOURCE_IRQ,
        }
};

static struct platform_device ark_disp_device = {
        .name                    = "ark-display",
        .id                      = -1,
        .num_resources           = ARRAY_SIZE(ark_disp_resource),
        .resource                = ark_disp_resource,
};
#endif /* CONFIG_ARK_DISP */

void __init ark1680_add_device_display(void) 
{
	/* TODO: Configure: clock, reset... */

#if     defined(CONFIG_ARK_DISP) || defined(CONFIG_ARK_DISP_MODULE)
        platform_device_register(&ark_disp_device);
#endif /* CONFIG_ARK_DISP */
}

/* ================================================================== */


/* ================== Prescaler Controller ============================ */
#if defined(CONFIG_ARK_PRESCAL) || defined(CONFIG_ARK_PRESCAL_MODULE)
static struct resource ark_prescal_resource [] = {
        [0] = {
                .start = PRE_SCALER_BASE,
                .end   = (PRE_SCALER_BASE + 0x800),
                .flags = IORESOURCE_MEM,
        },
        [1] = {
//                .start = SYS_BASE, 
//                .end   = (SYS_BASE + 0x200),
//                .flags = IORESOURCE_MEM,
//        },
//        [2] = {
                .start = PRE_SCALER_INT,
                .end   = PRE_SCALER_INT,
                .flags = IORESOURCE_IRQ,
        }
};
static struct platform_device ark_prescal_device = {
        .name                    = "ark-prescaler",
        .id                      = -1,
        .num_resources           = ARRAY_SIZE(ark_prescal_resource),
        .resource                = ark_prescal_resource,
};

#endif /* CONFIG_ARK_PRESCAL */

void __init ark1680_add_device_prescal(void) 
{
#if     defined(CONFIG_ARK_PRESCAL) || defined(CONFIG_ARK_PRESCAL_MODULE)
        platform_device_register(&ark_prescal_device);
#endif /* CONFIG_ARK_PRESCAL */
}
/* ================================================================== */


/* ================== Jpeg Controller ============================ */
#if defined(CONFIG_ARK_JPEG) || defined(CONFIG_ARK_JPEG_MODULE)
static struct resource ark_jpeg_resource [] = {
        [0] = {
                .start = JPEG_BASE,
                .end   = (JPEG_BASE + 0x800),
                .flags = IORESOURCE_MEM,
        },
        [1] = {
//                .start = SYS_BASE, 
//                .end   = (SYS_BASE + 0x200),
//                .flags = IORESOURCE_MEM,
//        },
//        [2] = {
                .start = JPEG_INT,
                .end   = JPEG_INT,
                .flags = IORESOURCE_IRQ,
        }
};
static struct platform_device ark_jpeg_device = {
        .name                    = "ark-jpeg",
        .id                      = -1,
        .num_resources           = ARRAY_SIZE(ark_jpeg_resource),
        .resource                = ark_jpeg_resource,
};

#endif /* CONFIG_ARK_JPEG */

void __init ark1680_add_device_jpeg(void) 
{
#if     defined(CONFIG_ARK_JPEG) || defined(CONFIG_ARK_JPEG_MODULE)
        platform_device_register(&ark_jpeg_device);
#endif /* CONFIG_ARK_JPEG */
}
/* ================================================================== */


/* ================== Deinterlace Controller ============================ */
#if defined(CONFIG_ARK_DEINTERLACE) || defined(CONFIG_ARK_DEINTERLACE_MODULE)
static struct resource ark_deinterlace_resource [] = {
        [0] = {
                .start = DEINTERLACE_BASE,
                .end   = (DEINTERLACE_BASE + 0x80),
                .flags = IORESOURCE_MEM,
        },
        [1] = {
 //               .start = SYS_BASE, 
 //               .end   = (SYS_BASE + 0x200),
 //               .flags = IORESOURCE_MEM,
 //       },
 //       [2] = {
                .start = DEINTERLACE_INT,
                .end   = DEINTERLACE_INT,
                .flags = IORESOURCE_IRQ,
        }
};
static struct platform_device ark_deinterlace_device = {
        .name                    = "ark-deinterlace",
        .id                      = -1,
        .num_resources           = ARRAY_SIZE(ark_deinterlace_resource),
        .resource                = ark_deinterlace_resource,
};
#endif /* CONFIG_ARK_DEINTERLACE */

void __init ark1680_add_device_deinterlace(void) 
{
#if     defined(CONFIG_ARK_DEINTERLACE) || defined(CONFIG_ARK_DEINTERLACE_MODULE)
        platform_device_register(&ark_deinterlace_device);
#endif /* CONFIG_ARK_DEINTERLACE */
}
/* ================================================================== */


/* ================== ITU656in Controller ============================ */
#if defined(CONFIG_ARK_ITU656) || defined(CONFIG_ARK_ITU656_MODULE)
static struct resource ark_itu656_resource [] = {
        [0] = {
                .start = ITU656_BASE,
                .end   = (ITU656_BASE + 0x980),
                .flags = IORESOURCE_MEM,
        },
        [1] = {
                .start = SYS_BASE,
                .end   = (SYS_BASE + 0x200),
                .flags = IORESOURCE_MEM,
        },
        [2] = {
                .start = ITU656_INT,
                .end   = ITU656_INT,
                .flags = IORESOURCE_IRQ,
        }
};
static struct platform_device ark_itu656_device = {
        .name                    = "ark-itu656",
        .id                      = -1,
        .num_resources           = ARRAY_SIZE(ark_itu656_resource),
        .resource                = ark_itu656_resource,
};
#endif /* CONFIG_ARK_ITU656 */

void __init ark1680_add_device_itu656(void) 
{
#if     defined(CONFIG_ARK_ITU656) || defined(CONFIG_ARK_ITU656_MODULE)
        platform_device_register(&ark_itu656_device);
#endif /* CONFIG_ARK_ITU656 */
}
/* ================================================================== */


/* ================== PWM Controller ============================ */
#if defined(CONFIG_ARK_PWM) || defined(CONFIG_ARK_PWM_MODULE)
static struct resource ark_pwm_resource [] = {
        [0] = {
                .start = PWM_BASE,
                .end   = (PWM_BASE + 0x80),
                .flags = IORESOURCE_MEM,
        }
};
static struct platform_device ark_pwm_device = {
        .name                    = "ark-pwm",
        .id                      = -1,
        .num_resources           = ARRAY_SIZE(ark_pwm_resource),
        .resource                = ark_pwm_resource,
};
#endif /* CONFIG_ARK_PWM */

void __init ark1680_add_device_pwm(void) 
{
#if     defined(CONFIG_ARK_PWM) || defined(CONFIG_ARK_PWM_MODULE)
        platform_device_register(&ark_pwm_device);
#endif /* CONFIG_ARK_PWM */
}
/* ================================================================== */

/* ================== Watchdog Timer Controller ============================ */
#if defined(CONFIG_ARK_WDT) || defined(CONFIG_ARK_WDT_MODULE)
static struct resource ark_wdt_resource [] = {
        [0] = {
                .start = WDT_BASE,
                .end   = (WDT_BASE + 0x40),
                .flags = IORESOURCE_MEM,
        },
        [1] = {
                .start = WDT_INT,
                .end   = WDT_INT,
                .flags = IORESOURCE_IRQ,
        }
};
static struct platform_device ark_wdt_device = {
        .name                    = "ark-wdt",
        .id                      = -1,
        .num_resources           = ARRAY_SIZE(ark_wdt_resource),
        .resource                = ark_wdt_resource,
};
#endif /* CONFIG_ARK_WDT */

void __init ark1680_add_device_wdt(void) 
{
#if defined(CONFIG_ARK_WDT) || defined(CONFIG_ARK_WDT_MODULE)
	platform_device_register(&ark_wdt_device);
#endif /* CONFIG_ARK_WDT */
}

/* ================== ADC Controller ============================ */
#if defined(CONFIG_ARK_ADC)
static struct resource ark_adc_resource [] = {
        [0] = {
                .start = ADC_BASE,
                .end   = (ADC_BASE + SZ_64),
                .flags = IORESOURCE_MEM,
        },
        [1] = {
                .start = ADC_INT,
                .end   = ADC_INT,
                .flags = IORESOURCE_IRQ,
        }
};
static struct platform_device ark_adc_device = {
        .name                    = "ark-adc",
        .id                      = -1,
        .num_resources           = ARRAY_SIZE(ark_adc_resource),
        .resource                = ark_adc_resource,
};

void __init ark1680_add_device_adc(void) 
{
	platform_device_register(&ark_adc_device);
}
#endif /* CONFIG_ARK_ADC */
/* ================================================================== */

#if defined(CONFIG_TOUCHSCREEN_ARK1680)
static struct resource ark_ts_resources[] = {
	{
		.start = ADC_BASE,
		.end   = ADC_BASE + SZ_64,
		.flags = IORESOURCE_MEM,
	}, 
	{
	    .start = ADC_INT,
	    .end   = ADC_INT,
	    .flags = IORESOURCE_IRQ,
	},
};

struct platform_device ark_ts_device = {
    .name =  "ark1680-ts",
    .id = -1,
    .num_resources = ARRAY_SIZE(ark_ts_resources),
	.resource = ark_ts_resources,
};

void __init ark1680_add_device_ts(void) 
{
	platform_device_register(&ark_ts_device);
}
#endif

//
// **************************************************************************
//
#if defined(CONFIG_USB_MUSB_HDRC) || defined(CONFIG_USB_MUSB_HDRC_MODULE)
static struct musb_hdrc_config musb_config = {
	.multipoint	= true,
	.num_eps	= 6,
	.ram_bits	= 12,
	.dma		= true,
	.dma_channels = 2,
};

static struct musb_hdrc_platform_data usb_data0 = {//
	.mode       = MUSB_OTG,
	.config		= &musb_config,
	.coreNumber = 1,
};

static struct musb_hdrc_platform_data usb_data1 = {
	.mode       = MUSB_OTG,
	.config		= &musb_config,
};

static u64 usb_dmamask = DMA_BIT_MASK(32);
static struct resource ark1680_usb0_resources[] = {
	{
		.start		= USB0_BASE,
		.end		= USB0_BASE + SZ_4K - 1,
		.flags		= IORESOURCE_MEM,
	},
	{
		.start		= USB0_INT,
		.end		= USB0_INT,
		.flags		= IORESOURCE_IRQ,
		.name		= "mc",
	},
	{
		.start		= USB0_DMA_INT,
		.end		= USB0_DMA_INT,
		.flags      = IORESOURCE_IRQ,
		.name		= "dma"
	},
};

static struct resource ark1680_usb1_resources[] = {
	{
		.start		= USB1_BASE,
		.end		= USB1_BASE + SZ_4K - 1,
		.flags		= IORESOURCE_MEM,
	},
	{
		.start		= USB1_INT,
		.end		= USB1_INT,
		.flags		= IORESOURCE_IRQ,
		.name		= "mc",
	},
	{
		.start		= USB1_DMA_INT,
		.end		= USB1_DMA_INT,
		.flags      = IORESOURCE_IRQ,
		.name		= "dma"
	},
};

static struct platform_device usb0_dev = {
	.name           = "musb0-ark1680",
	.id             = -1,
	.dev = {
		.platform_data		= &usb_data0,
		.dma_mask			= &usb_dmamask,
		.coherent_dma_mask  = DMA_BIT_MASK(32),
	},
	.resource       = ark1680_usb0_resources,
	.num_resources  = ARRAY_SIZE(ark1680_usb0_resources),
};

static struct platform_device usb1_dev = {
	.name           = "musb1-ark1680",
	.id             = -1,
	.dev = {
		.platform_data		= &usb_data1,
		.dma_mask			= &usb_dmamask,
		.coherent_dma_mask  = DMA_BIT_MASK(32),
	},
	.resource       = ark1680_usb1_resources,
	.num_resources  = ARRAY_SIZE(ark1680_usb1_resources),
};

void __init ark1680_add_device_usb(void)
{
    platform_device_register(&usb0_dev);
    platform_device_register(&usb1_dev);
}

#endif	/* defined(CONFIG_USB_MUSB_HDRC) || defined(CONFIG_USB_MUSB_HDRC_MODULE) */

/* HX170DEC controller */
#if defined(CONFIG_ARK_HX170DEC) || defined(CONFIG_ARK_HX170DEC_MODULE)
static struct resource hx170dec_resources[] = {
	[0] = {
			.start 	= MFC_BASE,
			.end 	= MFC_BASE + 0x193,
			.flags 	= IORESOURCE_MEM,
	},
	[1] = {
			.start 	= MFC_INT_0,
			.end	= MFC_INT_0,
			.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device ark1680_hx170dec_device = {
	.name 			= "hx170dec",
	.id				= 0,
	.resource 		= hx170dec_resources,
	.num_resources 	= ARRAY_SIZE(hx170dec_resources),
};
#endif

void __init ark1680_add_device_hx170dec(void) 
{
#if defined(CONFIG_ARK_HX170DEC) || defined(CONFIG_ARK_HX170DEC_MODULE)
	platform_device_register(&ark1680_hx170dec_device);
#endif
}

#if defined(CONFIG_BACKLIGHT_PWM)

/* ============================================================== */
/*pwm backlight controller*/
static struct platform_pwm_backlight_data pwm_backlight_data = 
{
	.pwm_id = 1,
	.max_brightness = 100,
	.dft_brightness = 30,
	.pwm_period_ns = 100000,
};

static struct platform_device pwm_backlight_device = 
{
	.name 			= "pwm-backlight",
	.id				= -1,
	.dev			= {
		.platform_data 	= &pwm_backlight_data,
	},
};

void __init ark1680_add_device_pwm_backlight(void) 
{
	platform_device_register(&pwm_backlight_device);
}	
#endif

//
// **************************************************************************
//
/* ================================================================== */

void __init ark1680_machine_init(void)
{
	printk(KERN_ERR "%s %d: -->\n", __FUNCTION__, __LINE__);
	ark1680_add_device_gpio();
	ark1680_add_device_uart();
	ark1680_add_device_dmac();
	ark1680_add_device_i2c(ark1680_i2c_devices, ARRAY_SIZE(ark1680_i2c_devices));
	analog_i2c_add_device_i2c(i2c_analog_devices, ARRAY_SIZE(i2c_analog_devices));
	ark1680_add_device_mci();
	ark1680_add_device_nand();
	ark1680_add_device_audio();
	ark1680_add_device_display();
	ark1680_add_device_prescal(); 
	ark1680_add_device_jpeg(); 
	ark1680_add_device_deinterlace(); 
	ark1680_add_device_itu656(); 
	ark1680_add_device_pwm();
	ark1680_add_device_wdt();
#if defined(CONFIG_ARK_ADC)
	ark1680_add_device_adc();
#endif

#if defined(CONFIG_TOUCHSCREEN_ARK1680)
	ark1680_add_device_ts();
#endif

#if defined(CONFIG_USB_MUSB_HDRC) || defined(CONFIG_USB_MUSB_HDRC_MODULE)
	ark1680_add_device_usb();

	u32 sys_vich_base = ioremap(VICH_BASE, SZ_512);
	u32 sys_vicl_base = ioremap(VICL_BASE, SZ_512);
	if (sys_vich_base == NULL || sys_vicl_base == NULL ) {
        printk(KERN_ERR "%s %d: failed to ioremap VICH/L registers", __FUNCTION__, __LINE__);
		return -ENODEV;
	}
	else {
		(*(volatile unsigned int *)(sys_vich_base + 0x10)) |= ( BIT(7) | BIT(8) );
	}
#endif
#if defined(CONFIG_BACKLIGHT_PWM)
	ark1680_add_device_pwm_backlight();
#endif
}

