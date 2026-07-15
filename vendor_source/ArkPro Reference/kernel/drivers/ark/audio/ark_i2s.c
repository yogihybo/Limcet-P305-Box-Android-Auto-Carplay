/*
 * ark_i2s.c -- ALSA SoC Audio Layer
 *
 * Jack Tang <jacktang@astri.org>
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/clk.h>
#include <mach/va_map.h>
#include "ark_i2s.h"
#include "ark_pcm.h"
#include "ark_i2s_sddac_regs.h"
#include <linux/ark/ark_i2s.h>

#undef ARK_I2S_DEBUG
#ifdef ARK_I2S_DEBUG
#define DBG(f, a...) printk(KERN_DEBUG "ASOC %s-%d: "f, __FUNCTION__, __LINE__, ##a)
#else
#define DBG(...)
#endif

#define ERR(f, a...) printk(KERN_ERR "ASOC %s-%d: "f, __FUNCTION__, __LINE__, ##a)


struct ark_i2s_dev {
	struct  platform_device		*pdev;
	void __iomem 			*base;
	struct clk 				*clk;
	struct ark_pcm_dma_params 	dma_params[2];
};

static void setup_i2s2()
{
	void __iomem	*SYS_base_IIS = ARK1680_VA_SYS;
	uint32_t val;

	val = readl(SYS_base_IIS + ARK_SYS_PAD_CTRL06);
	val &= ~(ARK_SYS_I2S_BCLK_MASK
			|ARK_SYS_I2S_MCLK_MASK
			|ARK_SYS_I2S_SADATA_MASK
			|ARK_SYS_I2S_SYNC_MASK);
	writel(val, SYS_base_IIS + ARK_SYS_PAD_CTRL06);


	val = readl(SYS_base_IIS + ARK_SYS_PAD_CTRL09);
	val |= (ARK_SYS_I2S2_BCLK
			|ARK_SYS_I2S2_SADATA
			|ARK_SYS_I2S2_SYNC);
	writel(val, SYS_base_IIS + ARK_SYS_PAD_CTRL09);

	val = readl(SYS_base_IIS + ARK_SYS_PAD_CTRL0C);
	val |= ARK_SYS_I2S_MCLK_AUX;
	writel(val, SYS_base_IIS + ARK_SYS_PAD_CTRL0C);

	val = readl(SYS_base_IIS + ARK_SYS_PAD_CTRL0A);
	val  |= (7<<8);
	writel(val, SYS_base_IIS + ARK_SYS_PAD_CTRL0A);
}

static void dump_i2s_registers(struct ark_i2s_dev *i2s)
{
	printk("ARK_I2SSDDAC_SACR0  = 0x%08x\n", readl(i2s->base + ARK_I2SSDDAC_SACR0));
	printk("ARK_I2SSDDAC_SACR1  = 0x%08x\n", readl(i2s->base + ARK_I2SSDDAC_SACR1));
	printk("ARK_I2SSDDAC_SACR1  = 0x%08x\n", readl(i2s->base + ARK_I2SSDDAC_DACR0));
	printk("ARK_I2SSDDAC_SASR0  = 0x%08x\n", readl(i2s->base + ARK_I2SSDDAC_SASR0));
	printk("ARK_I2SSDDAC_DACR1  = 0x%08x\n", readl(i2s->base + ARK_I2SSDDAC_DACR1));
	printk("ARK_I2SSDDAC_SAIMR  = 0x%08x\n", readl(i2s->base + ARK_I2SSDDAC_SAIMR));
	printk("ARK_I2SSDDAC_SAICR  = 0x%08x\n", readl(i2s->base + ARK_I2SSDDAC_SAICR));
	printk("ARK_I2SSDDAC_ADCR0  = 0x%08x\n", readl(i2s->base + ARK_I2SSDDAC_ADCR0));
	printk("ARK_I2SSDDAC_SADR  = 0x%08x\n", readl(i2s->base + ARK_I2SSDDAC_SADR));
}


static void ark_i2s_txctrl(struct ark_i2s_dev *i2s, int on)
{
	uint32_t val;
	if (on) {
		val = readl(i2s->base + ARK_I2SSDDAC_SACR1);
		val &= ~ARK_I2SSDDAC_SACR1_DIS_PLAY;                     //enable play
		writel(val, i2s->base + ARK_I2SSDDAC_SACR1);
		val = readl(i2s->base + ARK_I2SSDDAC_SACR0);
		val |= ARK_I2SSDDAC_SACR0_TDMAENA;                       // enable tx dma
		writel(val, i2s->base + ARK_I2SSDDAC_SACR0);
	} else {
		val = readl(i2s->base + ARK_I2SSDDAC_SACR1);
		val |= ARK_I2SSDDAC_SACR1_DIS_PLAY;                     //disable play
		writel(val, i2s->base + ARK_I2SSDDAC_SACR1);
		val = readl(i2s->base + ARK_I2SSDDAC_SACR0);
		val &= ~ARK_I2SSDDAC_SACR0_TDMAENA;                 // disable tx dma
		writel(val, i2s->base + ARK_I2SSDDAC_SACR0);
	}
	return;
}

static void ark_i2s_rxctrl(struct ark_i2s_dev *i2s, int on)
{
	uint32_t val;
	if (on) {
		val = readl(i2s->base + ARK_I2SSDDAC_SACR1);
		val &= ~ARK_I2SSDDAC_SACR1_DIS_REC;                     //enable record
		writel(val, i2s->base + ARK_I2SSDDAC_SACR1);
		val = readl(i2s->base + ARK_I2SSDDAC_SACR0);
		val |= ARK_I2SSDDAC_SACR0_RDMAENA;         // enable rx dma
		writel(val, i2s->base + ARK_I2SSDDAC_SACR0);
	} else {
		val = readl(i2s->base + ARK_I2SSDDAC_SACR1);
		val |= ARK_I2SSDDAC_SACR1_DIS_REC;                     //disable record
		writel(val, i2s->base + ARK_I2SSDDAC_SACR1);
		val = readl(i2s->base + ARK_I2SSDDAC_SACR0);
		val |= ARK_I2SSDDAC_SACR0_RDMAENA;         // disable rx dma
		writel(val, i2s->base + ARK_I2SSDDAC_SACR0);
	}
	return ;
}

static int ark_i2s_startup(
	struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct ark_i2s_dev *i2s = snd_soc_dai_get_drvdata(dai);
	unsigned int val;
	void __iomem	*IIS_base_IIS	 = i2s->base;
	void __iomem	*SYS_base_IIS = ARK1680_VA_SYS;
printk("IIS_base_IIS=%p \n", IIS_base_IIS);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		val = readl(ARK1680_VA_SYS + ARK_SYS_PAD_CTRL06);
		writel(val | ARK_SYS_I2S_DATA_DIR_OUT, ARK1680_VA_SYS + ARK_SYS_PAD_CTRL06);
		val = readl(ARK1680_VA_SYS + ARK_SYS_PAD_CTRL0C);
		writel(val  | ARK_SYS_I2S_MCLK_AUX, ARK1680_VA_SYS + ARK_SYS_PAD_CTRL0C);
	
		val = readl(i2s->base + ARK_I2SSDDAC_SACR0);
		val |= ARK_I2SSDDAC_SACR0_I2SEN;//i2s enable
		val |= ARK_I2SSDDAC_SACR0_BCKD;// Bitclock output
		val |= ARK_I2SSDDAC_SACR0_TFTH; // set tfth
		val |= ARK_I2SSDDAC_SACR0_SARADC_DATA; // set saradc data
		writel(val, i2s->base + ARK_I2SSDDAC_SACR0);

		val = readl(i2s->base + ARK_I2SSDDAC_SAIMR);
		val &= ~(ARK_I2SSDDAC_SAIMR_TFS 
				|ARK_I2SSDDAC_SAIMR_TUR);
		writel(val, i2s->base + ARK_I2SSDDAC_SAIMR);

		val = readl(i2s->base + ARK_I2SSDDAC_SAICR);
		val |= (ARK_I2SSDDAC_SAICR_TXCR 
				|ARK_I2SSDDAC_SAICR_TUR);
		writel(val, i2s->base + ARK_I2SSDDAC_SAICR);

		val = readl(i2s->base + ARK_I2SSDDAC_SAICR);
		val &= ~(ARK_I2SSDDAC_SAICR_TXCR 
				|ARK_I2SSDDAC_SAICR_TUR);
		writel(val, i2s->base + ARK_I2SSDDAC_SAICR);

		
		val = readl(i2s->base + ARK_I2SSDDAC_SACR0);
		val |= ARK_I2SSDDAC_SACR0_TDMAENA;         // enable tx dma
		writel(val, i2s->base + ARK_I2SSDDAC_SACR0);
		
		val = readl(i2s->base + ARK_I2SSDDAC_SACR1);
		val &= ~ARK_I2SSDDAC_SACR1_DIS_PLAY;                     //enable play
		writel(val, i2s->base + ARK_I2SSDDAC_SACR1);
		writel(0, i2s->base + ARK_I2SSDDAC_DACR0);
	} else if(substream->stream == SNDRV_PCM_STREAM_CAPTURE){
#if 0
		setup_i2s2();
		val = readl(ARK1680_VA_SYS + ARK_SYS_PAD_CTRL06);
		writel(val &(~ ARK_SYS_I2S_DATA_DIR_OUT), ARK1680_VA_SYS + ARK_SYS_PAD_CTRL06);

		val = readl(i2s->base + ARK_I2SSDDAC_SACR0);
		val &= ~(ARK_I2SSDDAC_SACR0_DACCLK_EDGE
				|ARK_I2SSDDAC_SACR0_RDMAENA
				|ARK_I2SSDDAC_SACR0_LOOPBACK);
		val |= (ARK_I2SSDDAC_SACR0_MIC_LINE_SEL
				|ARK_I2SSDDAC_SACR0_SARADC_POW_EN
				|ARK_I2SSDDAC_SACR0_SARADC_DATA
				|ARK_I2SSDDAC_SACR0_SARADC_EN
				|ARK_I2SSDDAC_SACR0_DAC_PD
				|ARK_I2SSDDAC_SACR0_VREF_PD
				|ARK_I2SSDDAC_SACR0_RFTH
				|ARK_I2SSDDAC_SACR0_BCKD
				|ARK_I2SSDDAC_SACR0_SYNCD
				|ARK_I2SSDDAC_SACR0_I2SEN);
		writel(val, i2s->base + ARK_I2SSDDAC_SACR0);

		val = readl(i2s->base + ARK_I2SSDDAC_SAIMR);
		val &= ~(ARK_I2SSDDAC_SAIMR_RFS 
				|ARK_I2SSDDAC_SAIMR_ROR);
		writel(val, i2s->base + ARK_I2SSDDAC_SAIMR);

		val = readl(i2s->base + ARK_I2SSDDAC_SAICR);
		val |= (ARK_I2SSDDAC_SAICR_RXCR 
				|ARK_I2SSDDAC_SAICR_ROR);
		writel(val, i2s->base + ARK_I2SSDDAC_SAICR);

		val = readl(i2s->base + ARK_I2SSDDAC_SAICR);
		val &= ~(ARK_I2SSDDAC_SAICR_RXCR 
				|ARK_I2SSDDAC_SAICR_ROR);
		writel(val, i2s->base + ARK_I2SSDDAC_SAICR);

		val = readl(i2s->base + ARK_I2SSDDAC_SACR0);
		val |= ARK_I2SSDDAC_SACR0_RDMAENA;                    // enable rx dma
		writel(val, i2s->base + ARK_I2SSDDAC_SACR0);

		val = readl(i2s->base + ARK_I2SSDDAC_SACR1);
		val &= ~ARK_I2SSDDAC_SACR1_DIS_REC;                     //enable record
		writel(val, i2s->base + ARK_I2SSDDAC_SACR1);
#else
	val = readl(SYS_base_IIS+0x1d8);
			val &= ~((0xf<<8)|(0xf<<12)|(0xf<<16)|(0xf<<20)|(1<<31));
			writel(val,SYS_base_IIS+0x1d8);

			val = readl(SYS_base_IIS+0x1e4);
			val &= ~((0x3<<28)|(0x3<<26)|(0x3<<24));
			val |= ((3<<28)|(3<<26)|(3<<24));
			writel(val,SYS_base_IIS+0x1e4);

			val = readl(SYS_base_IIS+0x1f0);
			val &= ~(0x1<<10);
			val |= (0x1<<10);
			writel(val,SYS_base_IIS+0x1f0);

			val = readl(SYS_base_IIS+0x1e8);
			val &= ~ (7<<8);
			writel(val,SYS_base_IIS+0x1e8);
			
			val = readl(SYS_base_IIS+0x1e8);
			val  |=(7<<8);
			writel(val,SYS_base_IIS+0x1e8);
			
			//\u017e\u017d位IIS
			writel(0x10ef0f10,IIS_base_IIS+0);


			//硬\u0152跏\u0152化
			val = 0;
			val = (1<<26)+(1<<25)+(0<<24)+(1<<23)+(1<<22) + (1<<21) + (0x10<<16) \
					+ (15<<8)+ (0x0<<7)+(0x0<<6) +(0x0<<5)+ (0x0<<3) + (0x1<<2) + (0x1<<1) + 0x1;
			writel(val,IIS_base_IIS+0x00);
			writel(0x03,IIS_base_IIS+0x4);
			writel(0x0,IIS_base_IIS+0x14);

			//配置采样率
			writel(0x9ef05,SYS_base_IIS+0x6c);
			writel(0x1001513,SYS_base_IIS+0x174);		

			
			//清中断标謀u0178
			writel(0xffffffff,IIS_base_IIS+0x18);
			writel(0x0,IIS_base_IIS+0x18);
	
			//开DMA中断
			val = readl(IIS_base_IIS+0x0);
			val  |= (0x1 << 3)|(0x1<<6);
			writel(val,IIS_base_IIS+0x0);

			//开蔦u0152耚u0152音
			writel(0x00000000,SYS_base_IIS+0x1d8);
			writel(0x2,IIS_base_IIS+0x4);
#endif
	}

	/* Tx/Rx Config */
	snd_soc_dai_set_dma_data(dai, substream, &i2s->dma_params[substream->stream]);
	
	return 0;
}

static int ark_i2s_hw_params(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params,
	struct snd_soc_dai *dai)
{
	struct ark_i2s_dev *i2s = snd_soc_dai_get_drvdata(dai);
	
	DBG("-->\n");

	return 0;
}


static int ark_i2s_trigger(
	struct snd_pcm_substream *substream, int cmd, struct snd_soc_dai *dai)
{
	int ret = 0;
	struct ark_i2s_dev *i2s = snd_soc_dai_get_drvdata(dai);

	DBG("-->\n");

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		/* TODO: start i2s */

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			ark_i2s_txctrl(i2s, 1);
		else
			ark_i2s_rxctrl(i2s, 1);

		/* TODO: Start DMA */

		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		/* TODO: stop i2s */
		
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			ark_i2s_txctrl(i2s, 0);
		else
			ark_i2s_rxctrl(i2s, 0);

		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int ark_i2s_set_fmt(
	struct snd_soc_dai *dai, unsigned int fmt)
{
	
	DBG("-->%d---------\n",fmt);
	return 0;
}

static int ark_i2s_probe(struct snd_soc_dai *dai)
{
	struct ark_i2s_dev *i2s = snd_soc_dai_get_drvdata(dai);
	int ret = 0;
	unsigned int sacr0;
	
	DBG("-->\n");

	return ret;
}

static int ark_i2s_remove(struct snd_soc_dai *dai)
{
	struct ark_i2s_dev *i2s = snd_soc_dai_get_drvdata(dai);
	
	DBG("-->\n");

	ark_i2s_txctrl(i2s, 0);
	ark_i2s_rxctrl(i2s, 0);

	return 0;
}

#ifdef CONFIG_PM
static int ark_i2s_suspend(struct snd_soc_dai *cpu_dai)
{
	DBG("-->\n");

	/* TODO: suspend i2s, disable clock */

	return 0;
}

static int ark_i2s_resume(struct snd_soc_dai *cpu_dai)
{
	DBG("-->\n");

	/* TODO: enable clock and resume i2s */

	return 0;
}
#else
#define ark_i2s_suspend 	NULL
#define ark_i2s_resume 		NULL
#endif

/* I2S supported rate and format */
#define ARK_I2S_RATES \
	(SNDRV_PCM_RATE_11025 | SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 | \
	SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | \
	SNDRV_PCM_RATE_64000 | SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 | \
	SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_8000)

#define ARK_I2S_FORMAT (SNDRV_PCM_FMTBIT_S16_LE) /* TODO: 18 and 20bits width */

static const struct snd_soc_dai_ops ark_i2s_dai_ops = {
	.startup		= ark_i2s_startup,
	.trigger 		= ark_i2s_trigger,
	.hw_params 		= ark_i2s_hw_params,
	.set_fmt 		= ark_i2s_set_fmt,
};

static struct snd_soc_dai_driver ark_i2s_dai = {
	.id 		= 0,
	.probe 		= ark_i2s_probe,
	.remove 	= ark_i2s_remove,
	.suspend 	= ark_i2s_suspend,
	.resume 	= ark_i2s_resume,
	.playback 	= {
		.channels_min 	= 1,
		.channels_max 	= 2,
		.rates 			= ARK_I2S_RATES,
		.formats 		= ARK_I2S_FORMAT,
	},
	.capture 	= {
		
		.channels_min 	= 1,
		.channels_max 	= 2,
		.rates 			= ARK_I2S_RATES,
		.formats 		= ARK_I2S_FORMAT,
	},
	.ops 		=  &ark_i2s_dai_ops,
};
EXPORT_SYMBOL_GPL(ark_i2s_dai);


static int ark_i2s_drv_probe(struct platform_device *pdev)
{
	struct ark_i2s_dev *i2s;
	struct resource *mem, *ioarea;
	struct ark_i2s_platform_data *pdata = pdev->dev.platform_data;
	int ret = 0;

	DBG("-->\n");
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "no mem resource\n");
		return -ENODEV;
	}

	ioarea = request_mem_region(mem->start, resource_size(mem), pdev->name);
	if(!ioarea) {
		ERR("Failed to request_mem_region\n");
	 	return -EBUSY;
	}

	i2s = kzalloc(sizeof(struct ark_i2s_dev), GFP_KERNEL);
	if (!i2s) {
		ERR("Failed to allocate ark_i2s_dev\n");
		ret =  -ENOMEM;
		goto err_release_mem;
	}

	i2s->base = ioremap(mem->start, resource_size(mem));
	if (!i2s->base) {
		ERR("Failed to remap register space\n");
		ret = -ENXIO;
		goto err_free_mem;
	}

	DBG("Reg base phy: 0x%x, cpu: 0x%p, dma_base 0x%x\n", 
		mem->start, i2s->base, pdata->dma_base);
	
	/* DMA parameters */
	i2s->dma_params[SNDRV_PCM_STREAM_PLAYBACK].dma_addr = (pdata->dma_base + ARK_I2SSDDAC_SADR);
	i2s->dma_params[SNDRV_PCM_STREAM_PLAYBACK].burstsize = 16;
	i2s->dma_params[SNDRV_PCM_STREAM_PLAYBACK].addrwidth = DMA_SLAVE_BUSWIDTH_4_BYTES;
	i2s->dma_params[SNDRV_PCM_STREAM_PLAYBACK].dws = &pdata->tx_dws;

	i2s->dma_params[SNDRV_PCM_STREAM_CAPTURE].dma_addr = (pdata->dma_base + ARK_I2SSDDAC_SADR);
	i2s->dma_params[SNDRV_PCM_STREAM_CAPTURE].burstsize = 16;
	i2s->dma_params[SNDRV_PCM_STREAM_CAPTURE].addrwidth = DMA_SLAVE_BUSWIDTH_4_BYTES;
	i2s->dma_params[SNDRV_PCM_STREAM_CAPTURE].dws = &pdata->rx_dws;
	
	/* Get i2s clock */
	i2s->clk = clk_get(&pdev->dev, "i2s");
	if (IS_ERR(i2s->clk)) {
		dev_err(&pdev->dev, "Cannot get the i2s clock\n");
		ret = -ENOENT;
		goto err_iounmap;
	}
	clk_enable(i2s->clk);

	i2s->pdev = pdev;

	/* Pre-assign snd_soc_dai_set_drvdata */
	dev_set_drvdata(&pdev->dev, i2s);

	ret = snd_soc_register_dai(&pdev->dev, &ark_i2s_dai);
	if (ret !=0)
		goto err_clk;

	return 0;

err_clk:
	clk_disable(i2s->clk);
	clk_put(i2s->clk);
err_iounmap:
	iounmap(i2s->base);
err_free_mem:
	kfree(i2s);
err_release_mem:
	release_mem_region(mem->start, resource_size(mem));

	return ret;
}

static int ark_i2s_drv_remove(struct platform_device *pdev)
{
	struct ark_i2s_dev *i2s = platform_get_drvdata(pdev);
	struct resource *mem;
	
	DBG("-->\n");

	snd_soc_unregister_dai(&pdev->dev);
	
	clk_disable(i2s->clk);
	clk_put(i2s->clk);
	iounmap(i2s->base);
	kfree(i2s);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	release_mem_region(mem->start, resource_size(mem));
	
	return 0;
}

static struct platform_driver ark_i2s_driver = {
	.probe 		= ark_i2s_drv_probe,
	.remove		= ark_i2s_drv_remove,
	.driver		= {
		.name 	= "ark_i2s_dev",
		.owner 	= THIS_MODULE,
	},
};

module_platform_driver(ark_i2s_driver);


MODULE_AUTHOR("Jack Tang, jacktang@astri.org");
MODULE_DESCRIPTION("ARK I2S SoC Interface");
MODULE_LICENSE("GPL");

