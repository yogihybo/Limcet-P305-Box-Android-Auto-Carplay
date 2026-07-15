/* 
 * Name:
 * 		irqs.h
 *
 */

#ifndef _ARK_IRQS_H_
#define _ARK_IRQS_H_


/* ============================================================================
 * 	IRQ Request Number
 * ============================================================================
 */

#define DMA_INT					0
#define GPIO_INT				1
#define RTC_PERIOD_INT			2
#define WDT_INT					3
#define ADC_INT					4
#define RCRT_INT				5
#define I2S_INT					6
#define TIMER0_INT				7
#define TIMER1_INT				8
#define SCAL_LD_INT				9		// LCD
#define JPEG_INT				10
#define POST_SCALER_INT			11
#define ITU656_INT				12
#define USB0_DMA_INT			13
#define USB0_INT				14
#define TIMER2_INT				15
#define NAND_INT				16
#define I2C_INT					17
#define SSI_INT					18
#define DEMUX_INT				19
#define UART0_INT				20
#define UART1_INT				21
#define UART2_INT				22
#define RTC_ALM_INT				23
#define MFC_INT_0				24
#define MFC_INT_1				25
#define SDHC0_INT				26
#define M2MDMA_INT				27
#define UART3_INT				28
#define TIMER3_INT				29
#define PRE_SCALER_INT			30
#define ARM_DMA_INT				31
#define ARM_PMU_INT				32
#define ARM_VALRESET_INT		33
#define ARM_VALFIQ_INT			34
#define ARM_VALIRQ_INT			35
#define ARM_DMAEXT_ERR_INT		36
#define ARM_DMASIRQ_INT			37
#define SDHC1_INT				38
#define GPU_INT					41
#define DEINTERLACE_INT			42

/* default NR_IRQS */
#define NR_IRQS					64

#endif /* _ARK_IRQS_H_ */

