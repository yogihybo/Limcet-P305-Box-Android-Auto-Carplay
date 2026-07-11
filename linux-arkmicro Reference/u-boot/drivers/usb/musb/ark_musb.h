#ifndef __MUSB_ARK_H__
#define __MUSB_ARK_H__
#include <usb.h>
#include "musb_core.h"

/* Base address of Sys registers */
#define MUSB_ARK_SYS_BASE 0xe4900000

/* Base address of Gpio registers */
#define MUSB_ARK_GPIO_BASE 0xe4600000

/* Base address of MUSB registers */
#define MUSB_ARK_USB_BASE 0xe0100000

/* Base address of MUSB1 registers */
#define MUSB_ARK_USB1_BASE 0xe0400000

/* Base address of MUSB DMA registers */
#define MUSB_ARK_USB_DMA_BASE (MUSB_ARK_USB_BASE + 0x200)

/* No Dynamic FIFO */
//#define MUSB_NO_DYNAMIC_FIFO

/* Bit masks for USB_DMA_INTERRUPT */

#define DMA0_INT                0x1     /* DMA0 pending interrupt */
#define DMA1_INT                0x2     /* DMA1 pending interrupt */

/* Bit masks for USB_DMAxCONTROL */

#define DMA_ENA                 0x1     /* DMA enable */
#define DIRECTION               0x2     /* direction of DMA transfer */
#define MODE                    0x4     /* DMA Bus error */
#define INT_ENA                 0x8     /* Interrupt enable */
#define EPNUM                   0xf0    /* EP number */
#define BUSERROR                0x100   /* DMA Bus error */

struct ark1680_musb_dma_regs {
        u8 interrupt;
	u8 reserved0[3];
        u16 control;
	u16 reserved1;
        u32 addr;
        u32 count;
} __attribute__((packed, aligned(USB_DMA_MINALIGN)));

int musb_platform_init(void);
#endif
