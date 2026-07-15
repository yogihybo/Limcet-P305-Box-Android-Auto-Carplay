/* 
 * ark_pcm.c -- ALSA SoC Audio Layer
 *
 * Jack Tang <jacktang@astri.org>
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>

#include "ark_pcm.h"
#include <linux/ark/ark_dw_dmac.h>

#undef ARK_PCM_DEBUG
#ifdef ARK_PCM_DEBUG
#define DBG(f, a...) 	printk(KERN_DEBUG "ASOC %s-%d: "f, __FUNCTION__, __LINE__, ##a)
#else
#define DBG(...)
#endif

#define ERR(f, a...) 	printk(KERN_ERR "ASOC %s-%d: "f, __FUNCTION__, __LINE__, ##a)

/*
 * frame = number of bytes per sample
 * period = number of frame in between each hardware interrupt
 *
 */
#if 1
static struct snd_pcm_hardware ark_pcm_hardware = {
	.info 				= (SNDRV_PCM_INFO_MMAP |
 						SNDRV_PCM_INFO_MMAP_VALID |
						SNDRV_PCM_INFO_PAUSE |
 						SNDRV_PCM_INFO_RESUME | 
						SNDRV_PCM_INFO_INTERLEAVED |
						SNDRV_PCM_INFO_BLOCK_TRANSFER),
	.formats 			= SNDRV_PCM_FMTBIT_S16_LE,
	.rates 				= (SNDRV_PCM_RATE_11025 | SNDRV_PCM_RATE_16000 |
						SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_32000 |
						SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
						SNDRV_PCM_RATE_64000 | SNDRV_PCM_RATE_88200 |
						SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 | 
						SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_8000),
	.rate_min 			= 8000,//11025
	.rate_max			= 192000,
	.channels_min 		= 1,
	.channels_max 		= 2,
	.buffer_bytes_max 	= 64 * 4096,
	.period_bytes_min 	= 1024,
	.period_bytes_max 	= 1024,
	.periods_min 		= 1,
	.periods_max 		= 64,
//	.fifo_size 			= 32,
};
#else
static struct snd_pcm_hardware ark_pcm_hardware = {
	.info 				= (SNDRV_PCM_INFO_MMAP |
 						SNDRV_PCM_INFO_MMAP_VALID |
						SNDRV_PCM_INFO_PAUSE |
 						SNDRV_PCM_INFO_RESUME | 
						SNDRV_PCM_INFO_INTERLEAVED |
						SNDRV_PCM_INFO_BLOCK_TRANSFER),
	.formats 			= SNDRV_PCM_FMTBIT_S16_LE,
	.rates 				= (SNDRV_PCM_RATE_11025 | SNDRV_PCM_RATE_16000 |
						SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_32000 |
						SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
						SNDRV_PCM_RATE_64000 | SNDRV_PCM_RATE_88200 |
						SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 | 
						SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_8000),
	.rate_min 			= 8000,//11025,
	.rate_max			= 192000,
	.channels_min 		= 1,
	.channels_max 		= 2,
	.buffer_bytes_max 	= 64 * 4096,
	.period_bytes_min 	= 4096,
	.period_bytes_max 	= 4096,
	.periods_min 		= 1,
	.periods_max 		= 64,
//	.fifo_size 			= 32,
};

#endif
enum {
	DMA_READY = 0,
};

struct ark_pcm_runtime_data {
	struct dma_chan				*dma_chan;
	unsigned int				pos;
	struct ark_pcm_dma_params 	*dma_params;
	unsigned long				flags;
	struct dw_cyclic_desc		*cdesc;
	struct snd_pcm_substream 	*substream;
};



static bool filter(struct dma_chan *chan, void *slave)
{
	struct dw_dma_slave *dws = slave;
	
	DBG("-->\n");

	if (dws->dma_dev == chan->device->dev) {
		chan->private = dws;
		DBG("-->\n");
		return true;
	} else
		return false;
}

/* This function is called by the DMA driver */
static void ark_pcm_dma_period_done(void *arg)
{
	struct ark_pcm_runtime_data *prtd = arg;

	if (prtd)
		snd_pcm_period_elapsed(prtd->substream);
}

static int ark_pcm_prepare_dma(struct ark_pcm_runtime_data *prtd,
	struct snd_pcm_substream *substream,
	enum dma_data_direction direction)
{
	struct dma_chan *chan = prtd->dma_chan;
	struct dw_cyclic_desc *cdesc;
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long buffer_len, period_len;

	DBG("direction=%d-->\n", direction);

	/* no byte transfer is allowed */
	if (runtime->dma_addr & 1 || runtime->buffer_size & 1) {
		DBG("too complex transfer\n");
		return -EINVAL;
	}

	buffer_len = frames_to_bytes(runtime, runtime->buffer_size);
	period_len = frames_to_bytes(runtime, runtime->period_size);
printk("+++buffer_len=%lu period_len=%lu+++\n", buffer_len, period_len);
//printk("\033[;31m%s:%d 0x%08x \033[0m\n", __func__, __LINE__, current);
	cdesc = dw_dma_cyclic_prep(chan, runtime->dma_addr, buffer_len,
				period_len, direction);
	if (IS_ERR(cdesc)) {
		DBG("could not prepare cyclic DMA\n");
		return PTR_ERR(cdesc);
	}

	cdesc->period_callback = ark_pcm_dma_period_done;
	cdesc->period_callback_param = prtd;

	prtd->cdesc = cdesc;

	set_bit(DMA_READY, &prtd->flags);

	DBG("<--\n");
	
	return 0;
}

/* 
 * This is called when the hardware parameter (hw_params) is set up by 
 * the application, that is, once when the buffer size, the period size
 * the format, etc. are defined for the pcm substream.
 *
 * Request DMA channel and set runtime params
 *
 */
static int ark_pcm_hw_params(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct ark_pcm_runtime_data *prtd = substream->runtime->private_data;
	int ret;

	DBG("-->\n");

/*	ret = snd_hwparams_to_dma_slave_config(substream, params, &slave_config);
	if (ret) {
		ERR("hwparams to dma slave configure failed\n");
		return ret;
	}
*/
//	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);

	ret = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
	if (ret < 0)
		return ret;

	/* snd_pcm_lib_malloc_pages returns 1 if bufferis changed */
	if (ret == 1)
		if (test_and_clear_bit(DMA_READY, &prtd->flags))
			dw_dma_cyclic_free(prtd->dma_chan);

	DBG("<--\n");

	return 0;
}

static int ark_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct ark_pcm_runtime_data *prtd = substream->runtime->private_data;

	DBG("-->\n");

	if (test_and_clear_bit(DMA_READY, &prtd->flags))
		dw_dma_cyclic_free(prtd->dma_chan);

	DBG("<--\n");

	return snd_pcm_lib_free_pages(substream);
}

static int ark_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct ark_pcm_runtime_data *prtd = substream->runtime->private_data;
	int ret = 0;

	if (!test_bit(DMA_READY, &prtd->flags)) {
		if (substream->stream== SNDRV_PCM_STREAM_PLAYBACK)
			ret = ark_pcm_prepare_dma(prtd, substream, DMA_TO_DEVICE);
		else if (substream->stream== SNDRV_PCM_STREAM_CAPTURE)
			ret = ark_pcm_prepare_dma(prtd, substream, DMA_FROM_DEVICE);
	}
	DBG("<--\n");

	return ret;
	
}
extern void ark_audio_mute(int mute);
#include <linux/delay.h>
static int ark_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct ark_pcm_runtime_data *prtd = substream->runtime->private_data;
	int ret = 0;

	DBG("-->\n");

	switch (cmd) {
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	DBG("SNDRV_PCM_TRIGGER_PAUSE_RELEASE\n");
		case SNDRV_PCM_TRIGGER_RESUME:
	DBG("SNDRV_PCM_TRIGGER_RESUME\n");
		case SNDRV_PCM_TRIGGER_START:
	DBG("SNDRV_PCM_TRIGGER_START\n");
			/* clock enable */
			//ark_audio_mute(1);
			ret = dw_dma_cyclic_start(prtd->dma_chan);printk("%s:%d\n", __func__, __LINE__);
			volatile int i;
			for(i=0;i < 1000000;i++);
			//msleep(50);
			ark_audio_mute(0);
			break;
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	DBG("SNDRV_PCM_TRIGGER_PAUSE_PUSH\n");
		case SNDRV_PCM_TRIGGER_SUSPEND:
	DBG("SNDRV_PCM_TRIGGER_SUSPEND\n");
		case SNDRV_PCM_TRIGGER_STOP:
	DBG("SNDRV_PCM_TRIGGER_STOP\n");
			ark_audio_mute(1);
			dw_dma_cyclic_stop(prtd->dma_chan);printk("%s:%d\n", __func__, __LINE__);
			/* clock disable */
			break;
		default:
			return -EINVAL;
			break;
	}

	DBG("<--\n");

	return ret;
}

static snd_pcm_uframes_t ark_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct ark_pcm_runtime_data *prtd = substream->runtime->private_data;
	struct snd_pcm_runtime	*runtime = substream->runtime;
	snd_pcm_uframes_t frames;
	unsigned long bytes;
	static int countt= 0;

	if (substream->stream== SNDRV_PCM_STREAM_PLAYBACK)
		bytes = dw_dma_get_src_addr(prtd->dma_chan);
	else if (substream->stream== SNDRV_PCM_STREAM_CAPTURE)
		bytes = dw_dma_get_dst_addr(prtd->dma_chan);
	bytes -= runtime->dma_addr;

	frames = bytes_to_frames(runtime, bytes);

	if (frames >= runtime->buffer_size)
		frames -= runtime->buffer_size;
	///if ((countt++) % 100 == 0)
	//	printk("\033[;31m%s:%d frames=%d \033[0m\n", __func__, __LINE__, frames);

	//DBG("<--\n");
	return frames;
}

static int ark_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct ark_pcm_runtime_data *prtd;
	struct dw_dma_slave *sdata;
	struct dma_slave_config slave_config = {0};
	dma_cap_mask_t mask;
	int ret = 0;

	DBG("-->\n");

	ret = snd_pcm_hw_constraint_integer(substream->runtime,
			SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;

	/* Set hw params */
	snd_soc_set_runtime_hwparams(substream, &ark_pcm_hardware);

	prtd = kzalloc(sizeof(*prtd), GFP_KERNEL);
	if (!prtd)
		return -ENOMEM;

	prtd->substream = substream;
	prtd->dma_params = snd_soc_dai_get_dma_data(rtd->cpu_dai, substream);
	sdata = prtd->dma_params->dws;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	DBG("-->\n");
	/* request DMA channel */
	prtd->dma_chan = dma_request_channel(mask, filter, sdata);
	if (!prtd->dma_chan) 
	{
		kfree(prtd);
		ERR("DMA request channel failed\n");
		return -EINVAL;
	}
	printk("\033[;31m%s:%d chan_id=%d \033[0m\n", __func__, __LINE__, prtd->dma_chan->chan_id);
	
	/* configure dma channel */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		slave_config.dst_addr = prtd->dma_params->dma_addr;
		slave_config.dst_addr_width = prtd->dma_params->addrwidth;
		slave_config.direction = DMA_MEM_TO_DEV;
	} else {
		slave_config.src_addr = prtd->dma_params->dma_addr;
		slave_config.src_addr_width = prtd->dma_params->addrwidth;
		slave_config.direction = DMA_DEV_TO_MEM;
	}
	
	slave_config.src_maxburst = prtd->dma_params->burstsize;
	slave_config.dst_maxburst = prtd->dma_params->burstsize;
	slave_config.device_fc = false;
	
	ret = dmaengine_slave_config(prtd->dma_chan, &slave_config);
	if (ret) {
		ERR("Failed to configure dma channel\n");
		goto err_dma;
	}

	//snd_dmaengine_pcm_set_data(substream, dma_params);

	substream->runtime->private_data = prtd;

	DBG("<--\n");

	return 0;

err_dma:
	dma_release_channel(prtd->dma_chan);
	kfree(prtd);
	return ret;
}

static int ark_pcm_close(struct snd_pcm_substream *substream)
{
	struct ark_pcm_runtime_data *prtd = substream->runtime->private_data;

	DBG("-->\n");

	dma_release_channel(prtd->dma_chan);
	kfree(prtd);

	DBG("<--\n");

	return 0;
}

/* Mapping a stream buffer from virtual space into physical space */
static int ark_pcm_mmap(
	struct snd_pcm_substream *substream, struct vm_area_struct *vma)
{
	struct snd_dma_buffer *buf = &substream->dma_buffer;

	DBG("-->\n");

	return remap_pfn_range(vma, vma->vm_start, buf->addr >> PAGE_SHIFT, 
				vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

static struct snd_pcm_ops ark_pcm_ops = {
	.open 		= ark_pcm_open,
	.close 		= ark_pcm_close,
	.ioctl 		= snd_pcm_lib_ioctl,
	.hw_params 	= ark_pcm_hw_params,
	.hw_free 	= ark_pcm_hw_free,
	.prepare 	= ark_pcm_prepare,
	.trigger 	= ark_pcm_trigger,
	.pointer 	= ark_pcm_pointer,
	.mmap 		= ark_pcm_mmap,
};

/* allocate dma buffer for playback/capture stream */
static int ark_pcm_preallocate_dma_buffer(struct snd_pcm *pcm, int stream)
{
	struct snd_pcm_substream *substream = pcm->streams[stream].substream;
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	size_t size = ark_pcm_hardware.buffer_bytes_max;

	DBG("-->\n");	

	buf->dev.type = SNDRV_DMA_TYPE_DEV;
	buf->dev.dev = pcm->card->dev;
	buf->private_data = NULL;
	buf->area = dma_alloc_coherent(pcm->card->dev, size, 
					&buf->addr, GFP_KERNEL | GFP_DMA);

	DBG("area(vm)=0x%p, addr(dma)=0x%p, size=%d\n", 
		(void *)buf->area, (void *)buf->addr, size);

	if (!buf->area) {
		ERR("dma_alloc_coherent failed\n");
		return -ENOMEM;
	}

	buf->bytes = size;

	DBG("<--\n");

	return 0;	
}

static u64 ark_pcm_dmamask = DMA_BIT_MASK(32);

/* Preallocate DMA buffer for playback and capture stream */
static int ark_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_card *card = rtd->card->snd_card;
	struct snd_pcm *pcm = rtd->pcm;
	int ret = 0;
	
	DBG("-->\n");

	if (!card->dev->dma_mask)
		card->dev->dma_mask = &ark_pcm_dmamask;
	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);
	
	
	DBG("-->\n");

	if (pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream) 
	{
		
		DBG("-->\n");
		ret = ark_pcm_preallocate_dma_buffer(pcm, SNDRV_PCM_STREAM_PLAYBACK);
		DBG("-->\n");

		if (ret)
			goto out;
	}

	if (pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream) {
		ret = ark_pcm_preallocate_dma_buffer(pcm, SNDRV_PCM_STREAM_CAPTURE);

		if (ret)
			goto out;
	}

out:
	return ret;
}

/* Free DMA area for streams of PCM instance */
static void ark_pcm_free_dma_buffers(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream;
	struct snd_dma_buffer *buf;
	int stream;

	DBG("-->\n");

	for (stream = 0; stream < 2; stream++) {
		substream = pcm->streams[stream].substream;
		if (!substream)
			continue;

		buf = &substream->dma_buffer;
		if (!buf->area)
			continue;

		dma_free_coherent(pcm->card->dev, buf->bytes, 
			buf->area, buf->addr);

		buf->area = NULL;
	}
}


static struct snd_soc_platform_driver ark_soc_platform = {
	.ops 		= &ark_pcm_ops,
	.pcm_new 	= ark_pcm_new,
	.pcm_free 	= ark_pcm_free_dma_buffers,
};

static int __devinit ark_soc_platform_probe(struct platform_device *pdev)
{
	DBG("-->\n");

	return snd_soc_register_platform(&pdev->dev, &ark_soc_platform);
}

static int __devexit ark_soc_platform_remove(struct platform_device *pdev)
{
	DBG("-->\n");

	snd_soc_unregister_platform(&pdev->dev);
	return 0;
}

static struct platform_driver ark_pcm_driver = {
	.driver = {
		.name = "ark-audio",
		.owner = THIS_MODULE,
	},
	
	.probe = ark_soc_platform_probe,
	.remove = __devexit_p(ark_soc_platform_remove),
};
module_platform_driver(ark_pcm_driver);


MODULE_AUTHOR("Jack Tang <jacktang@astri.org>");
MODULE_DESCRIPTION("ARK PCM DMA Driver");
MODULE_LICENSE("GPL");

