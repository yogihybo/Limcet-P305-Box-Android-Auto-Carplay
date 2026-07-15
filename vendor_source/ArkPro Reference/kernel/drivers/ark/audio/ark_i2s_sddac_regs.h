/* 
 * ark_i2s_sddac_regs.h
 *
 */
#define ARK_SYS_PAD_CTRL06						0x1d8
#define ARK_SYS_I2S_DATA_DIR_OUT				(1 << 31)
#define ARK_SYS_I2S_BCLK_MASK					(0xf << 20)
#define ARK_SYS_I2S_MCLK_MASK					(0xf << 16)
#define ARK_SYS_I2S_SADATA_MASK				(0xf << 12)
#define ARK_SYS_I2S_SYNC_MASK					(0xf << 8)

#define ARK_SYS_PAD_CTRL09						0x1e4
#define ARK_SYS_I2S2_BCLK							(3 << 28)
#define ARK_SYS_I2S2_SADATA						(3 << 26)
#define ARK_SYS_I2S2_SYNC						(3 << 24)

#define ARK_SYS_PAD_CTRL0A						0x1e8


#define ARK_SYS_PAD_CTRL0C						0x1f0
#define ARK_SYS_I2S_MCLK_AUX				(1 << 10)
/* Register definition */

#define ARK_I2SSDDAC_SACR0					0x00
#define ARK_I2SSDDAC_SACR0_I2SEN			(1 << 0)
#define ARK_I2SSDDAC_SACR0_SYNCD			(1 << 1)
#define ARK_I2SSDDAC_SACR0_BCKD		 		(1 << 2)
#define ARK_I2SSDDAC_SACR0_TDMAENA 			(1 << 3)
#define ARK_I2SSDDAC_SACR0_RESET	 		(1 << 4)
#define ARK_I2SSDDAC_SACR0_LOOPBACK			(1 << 5)
#define ARK_I2SSDDAC_SACR0_RDMAENA			(1 << 6)
#define ARK_I2SSDDAC_SACR0_DACCLK_EDGE		(1 << 7)
#define ARK_I2SSDDAC_SACR0_TFTH				(0xF << 8)
#define ARK_I2SSDDAC_SACR0_RFTH				(0x10 << 16)
#define ARK_I2SSDDAC_SACR0_VREF_PD			(1 << 21)
#define ARK_I2SSDDAC_SACR0_DAC_PD			(1 << 22)
#define ARK_I2SSDDAC_SACR0_SARADC_EN		(1 << 23)
#define ARK_I2SSDDAC_SACR0_SARADC_DATA		(1 << 24)
#define ARK_I2SSDDAC_SACR0_SARADC_POW_EN	(1 << 25)
#define ARK_I2SSDDAC_SACR0_MIC_LINE_SEL		(1 << 26)
#define ARK_I2SSDDAC_SACR0_ADC_PGA_OP		(1 << 27)
#define ARK_I2SSDDAC_SACR0_SARADC_VREF		(1 << 28)

#define ARK_I2SSDDAC_SACR1					0x04
#define ARK_I2SSDDAC_SACR1_DIS_REC			(1 << 0)
#define ARK_I2SSDDAC_SACR1_DIS_PLAY			(1 << 1)

#define ARK_I2SSDDAC_DACR0					0x08
#define ARK_I2SSDDAC_DACR0_LVOL				(0x7f << 0)
#define ARK_I2SSDDAC_DACR0_RVOL				(0x7f << 8)

#define ARK_I2SSDDAC_SASR0					0x0C
#define ARK_I2SSDDAC_SASR0_TNF				(1 << 0)
#define ARK_I2SSDDAC_SASR0_RNE				(1 << 1)
#define ARK_I2SSDDAC_SASR0_BSY				(1 << 2)
#define ARK_I2SSDDAC_SASR0_TFS				(1 << 3)
#define ARK_I2SSDDAC_SASR0_RFS				(1 << 4)
#define ARK_I2SSDDAC_SASR0_TUR				(1 << 5)
#define ARK_I2SSDDAC_SASR0_ROR				(1 << 6)
#define ARK_I2SSDDAC_SASR0_TFL				(0x3F << 8)
#define ARK_I2SSDDAC_SASR0_RFL				(0x3F << 16)

#define ARK_I2SSDDAC_DACR1					0x10
#define ARK_I2SSDDAC_DACR1_VOL_TH			(0x1F << 0)
#define ARK_I2SSDDAC_DACR1_LRSW				(1 << 5)

/* Interrupt Mask */
#define ARK_I2SSDDAC_SAIMR					0x14
#define ARK_I2SSDDAC_SAIMR_TFS				(1 << 3)
#define ARK_I2SSDDAC_SAIMR_RFS				(1 << 4)
#define ARK_I2SSDDAC_SAIMR_TUR				(1 << 5)
#define ARK_I2SSDDAC_SAIMR_ROR				(1 << 6)

/* Interrupt Clear */
#define ARK_I2SSDDAC_SAICR					0x18
#define ARK_I2SSDDAC_SAICR_TXCR				(1 << 3)
#define ARK_I2SSDDAC_SAICR_RXCR				(1 << 4)
#define ARK_I2SSDDAC_SAICR_TUR				(1 << 5)
#define ARK_I2SSDDAC_SAICR_ROR				(1 << 6)

/* SARADC Control Register */
#define ARK_I2SSDDAC_ADCR0					0x1C
#define ARK_I2SSDDAC_ADCR0_LCH_VOL			(0xF << 0)
#define ARK_I2SSDDAC_ADCR0_RCH_VOL			(0xF << 4)
#define ARK_I2SSDDAC_ADCR0_MIC_ENH			(1 << 8)
#define ARK_I2SSDDAC_ADCR0_LCH_FILTER		(0x3 << 9)
#define ARK_I2SSDDAC_ADCR0_RCH_FILTER		(0x3 << 11)

/* Serial Audio Data Register (TX and RX FIFO Access) */
#define ARK_I2SSDDAC_SADR					0x80


/* FIFO Size */
#define ARK_I2S_TXFIFO_SIZE					32
#define ARK_I2S_RXFIFO_SIZE					32

/* DMA Port Number */
#define ARK_I2S_DMA_TX 						1 	/* TODO: refer to Arkmicro DMA */
#define ARK_I2S_DMA_RX						2	/* TODO: refer to Arkmicro DMA */

/* TX and RX thershold */
#define ARK_I2S_TX_TH						15
#define ARK_I2S_RX_TH						16

