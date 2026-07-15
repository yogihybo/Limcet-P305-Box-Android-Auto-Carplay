/*
 * ark_pcm.h --
 *
 * ALSA PCM interface for the ARK 
 *
 */

#ifndef __ARK_PCM_H__
#define __ARK_PCM_H__

#include <linux/dw_dmac.h>

struct ark_pcm_dma_params {
//	int 			channel;				/* channel ID */
	unsigned long 	dma_addr;
	int 			burstsize;				/* 16 */
	int 			addrwidth;				/* DMA_SLAVE_BUSWIDTH_4_BYTES */

	/* DMA engine client structure */
	struct dw_dma_slave	*dws;
};

#endif

